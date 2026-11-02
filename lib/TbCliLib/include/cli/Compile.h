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

#include "cli/Config.h"
#include "cli/IssueTypes.h"
#include "cli/Process.h"

#include "vm/vec.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::cli
{

/** The name of qbsp, which its issues carry as `tool` and its stage as `name`. */
inline constexpr auto QbspTool = std::string_view{"qbsp"};

/** The timeout of qbsp unless tbcli.json or --timeout sets one. */
inline constexpr auto DefaultQbspTimeout = std::chrono::seconds{300};

/** A severity-marked line of a compiler's log. */
struct CompilerMessage
{
  Severity severity = Severity::Warning;
  /**
   * The text after the severity marker, as the tool wrote it, except that the path of
   * the working directory is left out.
   */
  std::string message;
  /** The line of the map that the message names as `<map>[line N]`. */
  std::optional<size_t> line;
  /** The classname of the entity that the message names as `<classname> at <origin>`. */
  std::optional<std::string> classname;
  /** The origin of the entity that the message names as `<classname> at <origin>`. */
  std::optional<vm::vec3d> origin;
};

/**
 * The severity-marked lines of an ericw-tools log: `WARNING: <message>` is a warning,
 * `ERROR: <message>` and the line after `************ ERROR ************` are errors.
 * Every other line, including `INFO:` lines and statistics, is left out.
 *
 * Locations in messages are written as `<map>[line N]`, where `<map>` is the map file
 * as the tool was given it, and as `<classname> at <x> <y> <z>`, with the origin in
 * parentheses or brackets in some messages and its numbers in their shortest form,
 * such as `32.5`.
 *
 * Occurrences of `workDir` with a trailing separator are removed from the messages, so
 * that they do not depend on the temporary directory the tool ran in.
 */
std::vector<CompilerMessage> parseCompilerLog(
  std::string_view log, const std::filesystem::path& workDir);

/**
 * A compiler message without the line numbers it names, so that it stays the same when
 * lines are added or removed before what it is about.
 */
std::string withoutLineNumbers(const std::string& message);

/** One run of a compiler. */
struct ToolRun
{
  /** The tool's name, such as `qbsp`, which compiler issues carry as `tool`. */
  std::string tool;
  ProcessResult process;
  std::chrono::milliseconds timeout{0};
  double ms = 0.0;
  /** The severity-marked lines of the tool's log, in log order. */
  std::vector<CompilerMessage> messages;

  /** Whether the tool exited with a non-zero code or was stopped. */
  bool failed() const;
};

struct CompileOptions
{
  /** The compilers from tbcli.json, with the command line overrides applied. */
  CompilersConfig compilers;
  /** Replaces the timeouts of every tool. */
  std::optional<std::chrono::milliseconds> timeout;
  /** Where to keep the artifacts; they are deleted if this is unset. */
  std::optional<std::filesystem::path> artifacts;
};

struct CompileResult
{
  std::vector<ToolRun> runs;
  /** Whether qbsp wrote a pointfile, which it does only if the map leaks. */
  bool leaked = false;
};

/**
 * Compiles the given map bytes in a new temporary directory, as `<mapName>.map`: runs
 * qbsp without `-leaktest` and with `-keepprt`, and parses the log the tool writes
 * (`<mapName>.log`), not what it prints.
 *
 * The temporary directory is deleted afterwards. If `options.artifacts` is set, the
 * artifacts (`.bsp`, `.pts`, `.prt` and `.log`) are first copied there, replacing
 * those of an earlier run, together with a sidecar `<mapName>.bsp.tbcli.json` holding
 * `{rev, leaked}`. If a tool failed, its `.bsp` and `.prt` are not kept, since they may
 * be partial.
 *
 * A tool that fails or runs too long is reported in its ToolRun, not thrown.
 *
 * @throw CliError `compiler_missing` if qbsp is not configured or not found, or
 * `write_failed` if the map or the artifacts cannot be written
 */
CompileResult compile(
  std::string_view mapBytes,
  const std::string& mapName,
  const std::string& rev,
  const CompileOptions& options);

} // namespace tb::cli
