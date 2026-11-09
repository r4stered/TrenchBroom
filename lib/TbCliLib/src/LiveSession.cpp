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

} // namespace tb::cli
