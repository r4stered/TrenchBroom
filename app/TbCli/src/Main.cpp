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

#include "cli/Run.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
  const auto args = std::vector<std::string>(argv + 1, argv + argc);

  // No command reads stdin yet; it is passed through once one does.
  const auto result = tb::cli::run(args);
  std::cerr << result.err << std::flush;
  std::cout << result.out << std::flush;
  return result.exitCode;
}
