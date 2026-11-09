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

#include "cli/LiveLink.h"
#include "cli/Run.h"

#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv)
{
  // --live and --file force the live link or file mode; the last one given wins
  auto args = std::vector<std::string>{};
  auto mode = tb::cli::LinkMode::Auto;
  for (const auto* arg : std::vector<const char*>(argv + 1, argv + argc))
  {
    if (arg == std::string_view{"--live"})
    {
      mode = tb::cli::LinkMode::Live;
    }
    else if (arg == std::string_view{"--file"})
    {
      mode = tb::cli::LinkMode::File;
    }
    else
    {
      args.emplace_back(arg);
    }
  }

  auto in = std::string{};
  if (tb::cli::readsStdin(args))
  {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    in.assign(std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{});
  }

  // a command on a map that an editor has open runs against the editor's map
  auto live = tb::cli::runLive(args, in, mode);
  const auto result = live ? std::move(*live) : tb::cli::run(args, in);
  std::cerr << result.err << std::flush;
  std::cout << result.out << std::flush;
  return result.exitCode;
}
