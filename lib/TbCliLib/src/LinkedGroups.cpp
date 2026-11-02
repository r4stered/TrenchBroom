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

#include "cli/LinkedGroups.h"

#include "cli/Ops.h"
#include "cli/Targets.h"
#include "mdl/GroupNode.h"
#include "mdl/Node.h"
#include "mdl/Object.h"
#include "mdl/WorldNode.h"

#include <algorithm>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace tb::cli
{
namespace
{

using Entry = LinkedGroupSnapshot::Entry;
using Instance = LinkedGroupSnapshot::Instance;

/** The link id of a group, entity, brush or patch; empty for the world and layers. */
std::string linkIdOf(const mdl::Node& node)
{
  const auto* object = dynamic_cast<const mdl::Object*>(&node);
  return object ? object->linkId() : std::string{};
}

void collectGroups(const mdl::Node& node, std::vector<mdl::GroupNode*>& groups)
{
  for (auto* child : node.children())
  {
    if (auto* group = dynamic_cast<mdl::GroupNode*>(child))
    {
      groups.push_back(group);
    }
    collectGroups(*child, groups);
  }
}

/** Records the nodes below the given one, depth first, in file order. */
void recordEntries(
  const mdl::Node& parent,
  const std::optional<size_t> parentIndex,
  std::vector<Entry>& entries)
{
  for (auto* child : parent.children())
  {
    const auto index = entries.size();
    entries.push_back(Entry{child, linkIdOf(*child), parentIndex});
    recordEntries(*child, index, entries);
  }
}

/** Whether the node or one of its ancestors is among the given nodes. */
bool isInside(const mdl::Node& node, const std::unordered_set<const mdl::Node*>& nodes)
{
  for (const auto* each = &node; each; each = each->parent())
  {
    if (nodes.contains(each))
    {
      return true;
    }
  }
  return false;
}

/** How the nodes in a linked group now correspond to the entries it had before. */
struct Pairing
{
  /** For each entry, the node in the group that corresponds to it now, if any. */
  std::vector<mdl::Node*> current;
  /** For each node in the group now, the entry it corresponds to, if any. */
  std::vector<std::optional<size_t>> entryOfNow;
  /** The nodes in the group that correspond to no entry. */
  std::unordered_set<const mdl::Node*> unpaired;
  /** How many entries are still in the group as the same node. */
  size_t kept = 0;
};

/**
 * Pairs the nodes in the group now with its entries: a node that is still in the group
 * with itself, and then a copy with the first unpaired entry that has its link id and
 * whose parent is paired with the copy's parent.
 *
 * @param now the nodes in the group now, depth first
 */
Pairing pairByIdentity(
  const mdl::GroupNode& group,
  const std::vector<Entry>& entries,
  const std::vector<mdl::Node*>& now)
{
  auto result = Pairing{
    std::vector<mdl::Node*>(entries.size(), nullptr),
    std::vector<std::optional<size_t>>(now.size()),
    {},
    0};
  auto entryOf = std::unordered_map<const mdl::Node*, size_t>{};

  const auto nowSet = std::unordered_set<const mdl::Node*>{now.begin(), now.end()};
  for (size_t i = 0; i < entries.size(); ++i)
  {
    if (nowSet.contains(entries[i].node))
    {
      result.current[i] = entries[i].node;
      entryOf.emplace(entries[i].node, i);
      ++result.kept;
    }
  }

  // the unpaired entries by their parent entry (-1 for the group) and link id
  using Key = std::pair<long long, std::string>;
  auto candidates = std::map<Key, std::deque<size_t>>{};
  const auto parentKey = [](const std::optional<size_t> parent) {
    return parent ? static_cast<long long>(*parent) : -1LL;
  };
  for (size_t i = 0; i < entries.size(); ++i)
  {
    if (!result.current[i])
    {
      candidates[{parentKey(entries[i].parent), entries[i].linkId}].push_back(i);
    }
  }

  for (size_t j = 0; j < now.size(); ++j)
  {
    auto* node = now[j];
    if (const auto it = entryOf.find(node); it != entryOf.end())
    {
      result.entryOfNow[j] = it->second;
      continue;
    }

    auto parentEntry = std::optional<size_t>{};
    if (const auto* parent = node->parent(); parent != &group)
    {
      const auto it = entryOf.find(parent);
      if (it == entryOf.end())
      {
        result.unpaired.insert(node);
        continue;
      }
      parentEntry = it->second;
    }

    auto& matching = candidates[{parentKey(parentEntry), linkIdOf(*node)}];
    if (matching.empty())
    {
      result.unpaired.insert(node);
      continue;
    }
    result.current[matching.front()] = node;
    result.entryOfNow[j] = matching.front();
    entryOf.emplace(node, matching.front());
    matching.pop_front();
  }
  return result;
}

/**
 * Pairs the nodes in a linked copy that the map library refilled with copies of the
 * edited group's nodes, the source: the copy's nodes now and its entries are laid out as
 * the source's are, so they correspond where the source's correspond. Unlike link ids,
 * this tells apart linked groups nested in the same group, whose contents share link ids.
 */
Pairing pairLikeSource(
  const Pairing& source, size_t entryCount, const std::vector<mdl::Node*>& now)
{
  auto result =
    Pairing{std::vector<mdl::Node*>(entryCount, nullptr), source.entryOfNow, {}, 0};
  for (size_t j = 0; j < now.size(); ++j)
  {
    if (const auto entry = source.entryOfNow[j])
    {
      result.current[*entry] = now[j];
    }
    else
    {
      result.unpaired.insert(now[j]);
    }
  }
  return result;
}

/**
 * Records the copies that replaced the group's nodes, and reports as modified those that
 * correspond to a modified node, and the outermost new nodes, which correspond to created
 * ones, unless the op created them itself.
 */
void recordCopies(
  const std::vector<Entry>& entries,
  const Pairing& pairing,
  const std::vector<mdl::Node*>& now,
  OpOutcome& outcome)
{
  auto modifiedLinkIds = std::unordered_set<std::string>{};
  for (const auto* node : outcome.modified)
  {
    modifiedLinkIds.insert(linkIdOf(*node));
  }
  const auto created =
    std::unordered_set<const mdl::Node*>{outcome.created.begin(), outcome.created.end()};

  for (size_t i = 0; i < entries.size(); ++i)
  {
    auto* copy = pairing.current[i];
    if (copy && copy != entries[i].node)
    {
      outcome.replaced.push_back({entries[i].node, copy});
      if (modifiedLinkIds.contains(entries[i].linkId) && !isInside(*copy, created))
      {
        appendOnce(outcome.modified, copy);
      }
    }
  }

  for (auto* node : now)
  {
    if (
      pairing.unpaired.contains(node) && !pairing.unpaired.contains(node->parent())
      && !isInside(*node, created))
    {
      appendOnce(outcome.modified, node);
    }
  }
}

/**
 * Reports as removed the outermost nodes that the group lost, unless the op removed them
 * itself or moved them elsewhere in the map.
 */
void recordLost(
  const std::vector<Entry>& entries,
  const Pairing& pairing,
  const mdl::WorldNode& worldNode,
  OpOutcome& outcome)
{
  const auto removed =
    std::unordered_set<const mdl::Node*>{outcome.removed.begin(), outcome.removed.end()};

  for (size_t i = 0; i < entries.size(); ++i)
  {
    const auto& entry = entries[i];
    const auto parentLost = entry.parent && !pairing.current[*entry.parent];
    if (
      !pairing.current[i] && !parentLost && !removed.contains(entry.node)
      && !isInMap(*entry.node, worldNode))
    {
      appendOnce(outcome.removed, entry.node);
    }
  }
}

/** The node's latest copy, if it was replaced, or else the node itself. */
mdl::Node* latestCopy(
  mdl::Node* node, const std::unordered_map<const mdl::Node*, mdl::Node*>& copies)
{
  for (auto it = copies.find(node); it != copies.end(); it = copies.find(node))
  {
    node = it->second;
  }
  return node;
}

} // namespace

LinkedGroupSnapshot::LinkedGroupSnapshot(const mdl::WorldNode& worldNode)
{
  auto groups = std::vector<mdl::GroupNode*>{};
  collectGroups(worldNode, groups);

  auto linkSetSizes = std::unordered_map<std::string, size_t>{};
  for (const auto* group : groups)
  {
    ++linkSetSizes[group->linkId()];
  }

  for (auto* group : groups)
  {
    if (linkSetSizes[group->linkId()] > 1)
    {
      auto instance = Instance{group, {}};
      recordEntries(*group, std::nullopt, instance.entries);
      m_instances.push_back(std::move(instance));
    }
  }
}

void LinkedGroupSnapshot::recordLinkedUpdates(
  const mdl::WorldNode& worldNode, OpOutcome& outcome) const
{
  // the groups still in the map, with their nodes now and how those pair with before
  struct State
  {
    const Instance* instance;
    std::vector<mdl::Node*> now;
    Pairing pairing;
  };
  auto states = std::vector<State>{};
  for (const auto& instance : m_instances)
  {
    if (isInMap(*instance.group, worldNode))
    {
      auto now = nodeAndDescendants(*instance.group);
      now.erase(now.begin());
      auto pairing = pairByIdentity(*instance.group, instance.entries, now);
      states.push_back({&instance, std::move(now), std::move(pairing)});
    }
  }

  // the source of a link set's update is the group that kept the most of its nodes
  const auto sourceOf = [&](const State& state) -> const State* {
    const auto* result = static_cast<const State*>(nullptr);
    for (const auto& other : states)
    {
      if (
        other.instance->group->linkId() == state.instance->group->linkId()
        && other.pairing.kept > 0
        && (!result || other.pairing.kept > result->pairing.kept))
      {
        result = &other;
      }
    }
    return result;
  };

  auto changedLinkSets = std::unordered_set<std::string>{};
  for (const auto& state : states)
  {
    const auto& [group, entries] = *state.instance;
    if (std::ranges::equal(state.now, entries, {}, {}, &Entry::node))
    {
      continue;
    }
    changedLinkSets.insert(group->linkId());

    const auto* source = state.pairing.kept == 0 ? sourceOf(state) : nullptr;
    const auto pairing = source && source->now.size() == state.now.size()
                             && source->instance->entries.size() == entries.size()
                           ? pairLikeSource(source->pairing, entries.size(), state.now)
                           : state.pairing;
    recordCopies(entries, pairing, state.now, outcome);
    recordLost(entries, pairing, worldNode, outcome);
  }

  // a linked group inside a copy was replaced by a copy itself
  auto copies = std::unordered_map<const mdl::Node*, mdl::Node*>{};
  for (const auto& [replaced, copy] : outcome.replaced)
  {
    copies[replaced] = copy;
  }
  for (const auto& instance : m_instances)
  {
    auto* group = latestCopy(instance.group, copies);
    if (changedLinkSets.contains(linkIdOf(*group)) && isInMap(*group, worldNode))
    {
      appendOnce(outcome.instancesAffected, group);
    }
  }
}

} // namespace tb::cli
