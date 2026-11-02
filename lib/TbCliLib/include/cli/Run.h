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

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tb::cli
{

/**
 * The outcome of one tbcli invocation: everything a caller observes from the process.
 */
struct RunResult
{
  std::string out;
  std::string err;
  int exitCode = 0;
};

/**
 * Whether run needs stdin for the given arguments: only `apply <map> -` reads its batch
 * from it, so that other commands do not wait for input.
 */
bool readsStdin(const std::vector<std::string>& args);

/**
 * The tbcli entry point. `args` are the command line arguments without the program
 * name, `in` is the contents of stdin. Never throws.
 */
RunResult run(const std::vector<std::string>& args, std::string_view in = {});

} // namespace tb::cli
