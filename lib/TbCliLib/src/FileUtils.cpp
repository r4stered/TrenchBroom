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

#include "cli/FileUtils.h"

#include <fstream>
#include <iterator>

namespace tb::cli
{

std::optional<std::string> readFile(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  if (!stream)
  {
    return std::nullopt;
  }

  auto result =
    std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
  if (stream.bad())
  {
    return std::nullopt;
  }
  return result;
}

} // namespace tb::cli
