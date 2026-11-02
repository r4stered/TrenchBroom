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

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tb::cli
{

/** How a process that runProcess started ended. */
struct ProcessResult
{
  /** The process's exit code, unless it was stopped for running too long. */
  std::optional<int> exitCode;
  /** Whether the process was stopped because it ran longer than its timeout. */
  bool timedOut = false;
};

/**
 * Runs an executable with the given arguments in the given working directory, and
 * waits until it exits or the timeout passes, in which case it is killed. Its standard
 * input, output and error are the null device: tbcli reads the files a tool writes,
 * not what it prints.
 *
 * This does not use the editor's compile runner, which depends on Qt's process
 * handling.
 *
 * @throw CliError if the process cannot be started
 */
ProcessResult runProcess(
  const std::filesystem::path& executable,
  const std::vector<std::string>& arguments,
  const std::filesystem::path& workDir,
  std::chrono::milliseconds timeout);

/**
 * Whether the given path is a bare executable name such as `qbsp`, without a directory
 * part, which findExecutable looks up in PATH.
 */
bool isBareName(const std::filesystem::path& path);

/**
 * The executable the given path names: the path itself if it is not a bare name, or
 * else the first match in the directories of the PATH environment variable. On
 * Windows, `.exe` is appended to a name without an extension. Returns nullopt if there
 * is no such file, or, outside Windows, if it is not executable.
 */
std::optional<std::filesystem::path> findExecutable(const std::filesystem::path& path);

} // namespace tb::cli
