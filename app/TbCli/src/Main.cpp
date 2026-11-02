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
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv)
{
  const auto args = std::vector<std::string>(argv + 1, argv + argc);

  auto in = std::string{};
  if (tb::cli::readsStdin(args))
  {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    in.assign(std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{});
  }

  const auto result = tb::cli::run(args, in);
  std::cerr << result.err << std::flush;
  std::cout << result.out << std::flush;
  return result.exitCode;
}
