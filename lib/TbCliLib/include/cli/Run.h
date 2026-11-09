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

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::cli
{
class CliError;
class LiveSession;

/**
 * The outcome of one tbcli invocation: everything a caller observes from the process.
 */
struct RunResult
{
  std::string out;
  std::string err;
  int exitCode = 0;
};

/** What tbcli reports for the given error: its envelope on stdout and its exit code. */
RunResult errorResult(const CliError& error);

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

/**
 * Like run, but a command that names the session's map runs against the open map
 * instead of loading the file: reads report the live rev, `describe` adds a `session`
 * block `{live, epoch, map}`, and `apply` and the ops edit the open map as one undo
 * step and write nothing (see applyLive). A command that names another map fails with
 * `no_session`. The editor's configuration is authoritative, so `--game-config` and
 * `--fgd` fail with `invalid_argument`. Relative paths are resolved against the working
 * directory of the caller, so arguments should come from forwardableInvocation.
 *
 * Must be called on the thread that owns the session's map. Never throws.
 */
RunResult run(
  const std::vector<std::string>& args, std::string_view in, LiveSession& session);

/**
 * The command of the given command line if it edits the map (apply and every op), or
 * nothing if it doesn't or the command line is malformed.
 */
std::optional<std::string> mutatingCommand(const std::vector<std::string>& args);

/**
 * The absolute path of the map that the given command line runs against, if its
 * command takes one, can run in a live session and the command line is well-formed.
 * `rooms` and `verify --compile` can't run in a live session yet, so they run in file
 * mode.
 */
std::optional<std::filesystem::path> liveMapArgument(
  const std::vector<std::string>& args);

/** A command line and its stdin. */
struct Invocation
{
  std::vector<std::string> args;
  std::string in;
};

/**
 * The given command line and stdin rewritten so that they mean the same in another
 * process with another working directory, such as the editor: the map path and every
 * option that names a file (`--game-config`, `--fgd`, `--quake-dir`, `-o`,
 * `--artifacts`, `--from-artifacts` and compilers given as paths) are made absolute,
 * and the batch file of `apply` is read and passed as stdin.
 *
 * @throw CliError `file_not_found` if the batch file cannot be read, or the error of a
 * malformed command line
 */
Invocation forwardableInvocation(const std::vector<std::string>& args, std::string in);

} // namespace tb::cli
