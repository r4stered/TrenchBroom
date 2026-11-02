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

#include <array>
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

/** The name of vis, which its issues carry as `tool` and its stage as `name`. */
inline constexpr auto VisTool = std::string_view{"vis"};

/** The name of light, which its issues carry as `tool` and its stage as `name`. */
inline constexpr auto LightTool = std::string_view{"light"};

/** The timeout of qbsp unless tbcli.json or --timeout sets one. */
inline constexpr auto DefaultQbspTimeout = std::chrono::seconds{300};

/** The timeout of vis unless tbcli.json or --timeout sets one. */
inline constexpr auto DefaultVisTimeout = std::chrono::seconds{300};

/** The timeout of light unless tbcli.json or --timeout sets one. */
inline constexpr auto DefaultLightTimeout = std::chrono::seconds{600};

/** A compiler that `--compile` runs. */
struct CompilerTool
{
  /** The tool's name, which is its key in tbcli.json's `compilers` and its flag. */
  std::string_view name;
  /** The member of CompilersConfig that configures the tool. */
  ToolConfig CompilersConfig::* config;
  std::chrono::seconds defaultTimeout;
  /** The flag the tool is given before its input, if any. */
  std::string_view flag;
  /** The input the tool is given, by what follows the map name. */
  std::string_view input;
  /** The log the tool writes, by what follows the map name. */
  std::string_view log;
};

/**
 * The compilers in the order they run: qbsp without `-leaktest` and with `-keepprt` on
 * the map, then `vis -fast` and light without `-extra` on the bsp.
 */
inline constexpr auto CompilerTools = std::array<CompilerTool, 3>{{
  {QbspTool, &CompilersConfig::qbsp, DefaultQbspTimeout, "-keepprt", ".map", ".log"},
  {VisTool, &CompilersConfig::vis, DefaultVisTimeout, "-fast", ".bsp", "-vis.log"},
  {LightTool, &CompilersConfig::light, DefaultLightTimeout, "", ".bsp", "-light.log"},
}};

/** The compiler with the given name, if there is one. */
const CompilerTool* findCompilerTool(std::string_view name);

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

/** How far `--compile` goes: each level runs the tools of the levels before it, too. */
enum class CompileLevel
{
  /** qbsp */
  Bsp,
  /** qbsp, then `vis -fast` */
  Vis,
  /** qbsp, `vis -fast`, then light */
  Light,
};

struct CompileOptions
{
  CompileLevel level = CompileLevel::Bsp;
  /** The compilers from tbcli.json, with the command line overrides applied. */
  CompilersConfig compilers;
  /** Replaces the timeouts of every tool. */
  std::optional<std::chrono::milliseconds> timeout;
  /** Where to keep the artifacts; they are deleted if this is unset. */
  std::optional<std::filesystem::path> artifacts;
};

struct CompileResult
{
  /** The tools that ran, in the order they ran. */
  std::vector<ToolRun> runs;
  /**
   * The tools of the requested level that did not run because qbsp leaked or failed, in
   * the order they would have run.
   */
  std::vector<std::string> skipped;
  /** Whether qbsp wrote a pointfile, which it does only if the map leaks. */
  bool leaked = false;
  /**
   * The points of the pointfile, from the occupant to the void, if qbsp wrote one that
   * could be read.
   */
  std::vector<vm::vec3d> leakTrail;
};

/**
 * Compiles the given map bytes in a new temporary directory, as `<mapName>.map`: runs
 * qbsp without `-leaktest` and with `-keepprt`, then, as the level asks, `vis -fast`
 * and light without `-extra` on `<mapName>.bsp`. Parses the logs the tools write
 * (`<mapName>.log`, `<mapName>-vis.log` and `<mapName>-light.log`), not what they
 * print. The map leaks if qbsp writes a pointfile (`<mapName>.pts`); then, or if qbsp
 * fails, vis and light are skipped.
 *
 * The temporary directory is deleted afterwards. If `options.artifacts` is set, the
 * artifacts (`.bsp`, `.pts`, `.prt` and the logs) are first copied there, replacing
 * those of an earlier run, together with a sidecar `<mapName>.bsp.tbcli.json` holding
 * `{rev, leaked}`. If a tool failed, the `.bsp` and `.prt` are not kept, since they may
 * be partial.
 *
 * A tool that fails or runs too long is reported in its ToolRun, not thrown.
 *
 * @throw CliError `compiler_missing` if a tool of the level is not configured or not
 * found, or `write_failed` if the map or the artifacts cannot be written
 */
CompileResult compile(
  std::string_view mapBytes,
  const std::string& mapName,
  const std::string& rev,
  const CompileOptions& options);

} // namespace tb::cli
