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

#include "mdl/CommandProcessor.h"
#include "mdl/Map.h"

#include "kd/contracts.h"

#include <fmt/format.h>

#include <random>

namespace tb::cli
{

LiveSession::~LiveSession() = default;

LiveRev::LiveRev(mdl::Map& map)
  : m_epoch{fmt::format("{:08x}", std::random_device{}())}
{
  // the same changes that the editor reports as document changes
  const auto countChange =
    [&](const std::string&, const bool observable, const bool isModification) {
      if (observable && isModification)
      {
        ++m_counter;
      }
    };

  auto& commandProcessor = map.commandProcessor();
  m_notifierConnection += commandProcessor.transactionDoneNotifier.connect(countChange);
  m_notifierConnection += commandProcessor.transactionUndoneNotifier.connect(countChange);
}

const std::string& LiveRev::epoch() const
{
  return m_epoch;
}

std::string LiveRev::str() const
{
  return fmt::format("live-{}:{}", m_epoch, m_counter);
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
