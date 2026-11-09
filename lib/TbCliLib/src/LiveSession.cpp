/*
 Copyright (C) 2026 Drew Williams

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "cli/LiveSession.h"

#include "cli/ParseUtils.h"
#include "mdl/CommandProcessor.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/Node.h"
#include "mdl/WorldNode.h"

#include "kd/contracts.h"

#include <fmt/format.h>

#include <algorithm>
#include <iterator>
#include <random>

namespace tb::cli
{

LiveSession::~LiveSession() = default;

std::optional<ParsedLiveRev> parseLiveRev(const std::string_view rev)
{
  constexpr auto prefix = std::string_view{"live-"};
  if (!rev.starts_with(prefix))
  {
    return std::nullopt;
  }
  const auto colon = rev.rfind(':');
  if (colon == std::string_view::npos || colon <= prefix.size())
  {
    return std::nullopt;
  }
  const auto counter = parseUnsigned(rev.substr(colon + 1));
  if (!counter)
  {
    return std::nullopt;
  }
  return ParsedLiveRev{rev.substr(prefix.size(), colon - prefix.size()), *counter};
}

std::string formatLiveRev(const std::string_view epoch, const uint64_t counter)
{
  return fmt::format("live-{}:{}", epoch, counter);
}

std::string_view toString(const ChangeKind kind)
{
  switch (kind)
  {
  case ChangeKind::Created:
    return "created";
  case ChangeKind::Modified:
    return "modified";
  case ChangeKind::Removed:
    return "removed";
  }
  return "modified";
}

std::string_view toString(const ChangeOrigin origin)
{
  switch (origin)
  {
  case ChangeOrigin::Mapper:
    return "mapper";
  case ChangeOrigin::Agent:
    return "agent";
  }
  return "mapper";
}

namespace
{

void appendDescendants(const mdl::Node& node, std::vector<uint64_t>& serialNumbers)
{
  for (const auto* child : node.children())
  {
    serialNumbers.push_back(child->serialNumber());
    appendDescendants(*child, serialNumbers);
  }
}

bool hasChangedDescendant(
  const mdl::Node& node, const std::unordered_set<uint64_t>& changed)
{
  return std::ranges::any_of(node.children(), [&](const auto* child) {
    return changed.contains(child->serialNumber())
           || hasChangedDescendant(*child, changed);
  });
}

} // namespace

ChangeLog::ChangeLog(mdl::Map& map, const size_t capacity)
  : m_map{map}
  , m_capacity{capacity}
{
  m_notifierConnection += m_map.nodesWereAddedNotifier.connect(
    [&](const auto& nodes) { nodesChanged(nodes, ChangeKind::Created); });
  m_notifierConnection += m_map.nodesWereRemovedNotifier.connect(
    [&](const auto& nodes) { nodesChanged(nodes, ChangeKind::Removed); });
  m_notifierConnection += m_map.nodesDidChangeNotifier.connect(
    [&](const auto& nodes) { nodesChanged(nodes, ChangeKind::Modified); });
  // a lock touches what is below the locked node, such as a layer's lock. Groups are
  // left out: selecting locks linked groups softly, and an op on a locked group fails
  // anyway. Hiding and showing don't count.
  m_notifierConnection +=
    m_map.nodeLockingDidChangeNotifier.connect([&](const auto& nodes) {
      auto lockedNodes = std::vector<mdl::Node*>{};
      std::ranges::copy_if(nodes, std::back_inserter(lockedNodes), [](const auto* node) {
        return !dynamic_cast<const mdl::GroupNode*>(node);
      });
      nodesChanged(lockedNodes, ChangeKind::Modified);
    });

  auto& commandProcessor = m_map.commandProcessor();
  m_notifierConnection += commandProcessor.transactionDoneNotifier.connect(
    [&](const std::string&, const bool observable, const bool isModification) {
      transactionEnded(
        observable,
        isModification,
        m_map.commandProcessor().isRedoing() ? ChangeVia::Redo : ChangeVia::Do);
    });
  m_notifierConnection += commandProcessor.transactionUndoneNotifier.connect(
    [&](const std::string&, const bool observable, const bool isModification) {
      // undoing a transaction that is still open cancels it, which is no undo step
      transactionEnded(
        observable,
        isModification,
        m_map.commandProcessor().isTransactionActive() ? ChangeVia::Do : ChangeVia::Undo);
    });
}

uint64_t ChangeLog::counter() const
{
  return m_counter;
}

bool ChangeLog::covers(const uint64_t since) const
{
  return since >= m_floor;
}

std::vector<Change> ChangeLog::changesSince(const uint64_t since) const
{
  auto result = std::vector<Change>{};
  std::ranges::copy_if(m_changes, std::back_inserter(result), [&](const auto& change) {
    return change.counter > since;
  });
  return result;
}

std::unordered_set<uint64_t> ChangeLog::changedSince(const uint64_t since) const
{
  auto result = std::unordered_set<uint64_t>{};
  for (const auto& change : m_changes)
  {
    if (change.counter > since)
    {
      result.insert(change.serialNumber);
      result.insert(change.removedDescendants.begin(), change.removedDescendants.end());
    }
  }
  return result;
}

ChangeLog::AgentChanges::AgentChanges(ChangeLog& log)
  : m_log{log}
  , m_recordedBefore{log.m_recorded}
{
  m_log.m_origin = ChangeOrigin::Agent;
}

ChangeLog::AgentChanges::~AgentChanges()
{
  m_log.m_origin = ChangeOrigin::Mapper;
  m_log.trim();
}

void ChangeLog::AgentChanges::discard()
{
  m_log.discardAfter(m_recordedBefore);
}

void ChangeLog::nodesChanged(const std::vector<mdl::Node*>& nodes, const ChangeKind kind)
{
  for (const auto* node : nodes)
  {
    if (m_pendingKeys.emplace(node->serialNumber(), kind).second)
    {
      auto change = Change{.serialNumber = node->serialNumber(), .kind = kind};
      if (kind == ChangeKind::Removed)
      {
        appendDescendants(*node, change.removedDescendants);
      }
      m_pending.push_back(std::move(change));
    }
  }
}

void ChangeLog::transactionEnded(
  const bool observable, const bool isModification, const ChangeVia via)
{
  if (!observable)
  {
    // the changes count when the transaction around this one ends
    return;
  }
  if (!isModification)
  {
    // only the selection changed; inside a long-running transaction, the commands that
    // it ran directly have not counted yet
    if (!m_map.commandProcessor().isTransactionActive())
    {
      clearPending();
    }
    return;
  }

  ++m_counter;
  for (auto& change : m_pending)
  {
    change.counter = m_counter;
    change.origin = m_origin;
    change.via = via;
    if (change.kind == ChangeKind::Modified && m_origin == ChangeOrigin::Mapper)
    {
      // a newer modification stands for an older one that is the node's latest change,
      // so that dragging a node, which changes it many times, fills the log only once
      const auto latest = std::ranges::find(
        m_changes.rbegin(), m_changes.rend(), change.serialNumber, &Change::serialNumber);
      if (latest != m_changes.rend() && latest->kind == ChangeKind::Modified)
      {
        m_changes.erase(std::next(latest).base());
      }
    }
    m_changes.push_back(std::move(change));
    ++m_recorded;
  }
  clearPending();
  trim();
}

void ChangeLog::clearPending()
{
  m_pending.clear();
  m_pendingKeys.clear();
}

void ChangeLog::trim()
{
  if (m_origin == ChangeOrigin::Agent)
  {
    return;
  }
  while (m_changes.size() > m_capacity)
  {
    m_floor = m_changes.front().counter;
    m_changes.pop_front();
  }
}

void ChangeLog::discardAfter(const uint64_t recorded)
{
  const auto count = m_recorded - recorded;
  m_changes.erase(m_changes.end() - static_cast<long>(count), m_changes.end());
  m_recorded = recorded;
  clearPending();
}

std::string_view toString(const Touch touch)
{
  switch (touch)
  {
  case Touch::Changed:
    return "changed";
  case Touch::AncestorChanged:
    return "ancestor_changed";
  case Touch::DescendantChanged:
    return "descendant_changed";
  }
  return "changed";
}

std::optional<Touch> howTouched(
  const mdl::Node& node, const std::unordered_set<uint64_t>& changed)
{
  if (changed.contains(node.serialNumber()))
  {
    return Touch::Changed;
  }
  for (const auto* ancestor = node.parent();
       ancestor && !dynamic_cast<const mdl::WorldNode*>(ancestor);
       ancestor = ancestor->parent())
  {
    if (changed.contains(ancestor->serialNumber()))
    {
      return Touch::AncestorChanged;
    }
  }
  const auto hasDescendants = dynamic_cast<const mdl::GroupNode*>(&node)
                              || dynamic_cast<const mdl::EntityNode*>(&node);
  return hasDescendants && hasChangedDescendant(node, changed)
           ? std::optional{Touch::DescendantChanged}
           : std::nullopt;
}

LiveRev::LiveRev(mdl::Map& map, const size_t changeLogCapacity)
  : m_epoch{fmt::format("{:08x}", std::random_device{}())}
  , m_changeLog{map, changeLogCapacity}
{
}

const std::string& LiveRev::epoch() const
{
  return m_epoch;
}

std::string LiveRev::str() const
{
  return formatLiveRev(m_epoch, m_changeLog.counter());
}

ChangeLog& LiveRev::changeLog()
{
  return m_changeLog;
}

void AgentHistory::requestDone(
  const std::string& revBefore,
  const std::string& revAfter,
  const std::optional<uint64_t> storedBatch)
{
  if (revBefore != m_rev)
  {
    // the map changed without the agent since its last request
    m_batches.clear();
  }
  if (storedBatch)
  {
    m_batches.push_back(*storedBatch);
  }
  m_rev = revAfter;
}

std::optional<uint64_t> AgentHistory::mostRecentBatch(const std::string& rev) const
{
  return rev == m_rev && !m_batches.empty() ? std::optional{m_batches.back()}
                                            : std::nullopt;
}

void AgentHistory::batchUndone(const std::string& revAfter)
{
  contract_pre(!m_batches.empty());

  m_batches.pop_back();
  m_rev = revAfter;
}

} // namespace tb::cli
