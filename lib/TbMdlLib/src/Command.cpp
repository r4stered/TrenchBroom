/*
 Copyright (C) 2010 Kristian Duske

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

#include "mdl/Command.h"

#include <atomic>
#include <string>

namespace tb::mdl
{
namespace
{

uint64_t nextSerialNumber()
{
  static auto serialNumber = std::atomic<uint64_t>{0};
  return serialNumber.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

Command::Command(std::string name)
  : m_state{CommandState::Default}
  , m_name{std::move(name)}
  , m_serialNumber{nextSerialNumber()}
{
}

Command::~Command() = default;

Command::CommandState Command::state() const
{
  return m_state;
}

const std::string& Command::name() const
{
  return m_name;
}

uint64_t Command::serialNumber() const
{
  return m_serialNumber;
}

bool Command::performDo(Map& map)
{
  m_state = CommandState::Doing;
  const auto result = doPerformDo(map);
  m_state = result ? CommandState::Done : CommandState::Default;
  return result;
}

bool Command::isModification() const
{
  return false;
}

} // namespace tb::mdl
