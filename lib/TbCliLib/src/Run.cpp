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

#include "base/Logger.h"
#include "cli/Apply.h"
#include "cli/CliError.h"
#include "cli/Config.h"
#include "cli/Describe.h"
#include "cli/Fgd.h"
#include "cli/FileUtils.h"
#include "cli/Guide.h"
#include "cli/Json.h"
#include "cli/LoadMap.h"
#include "cli/Ops.h"
#include "cli/ParseUtils.h"
#include "cli/Query.h"
#include "cli/Rooms.h"
#include "cli/Show.h"
#include "cli/TextFormat.h"
#include "cli/Verify.h"
#include "version/Version.h"

#include "kd/overload.h"
#include "kd/string_utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <exception>
#include <iterator>
#include <optional>
#include <ranges>
#include <string_view>
#include <variant>

namespace tb::cli
{
namespace
{

constexpr auto UsageTemplate = R"(usage: tbcli <command> <map> [options]

commands:
  describe <map>        summarize a Valve 220 map
  query <map> [<ql>]    list the nodes or faces a TbQl query matches, in file
                        order (without a query, every node but the world);
                        quote string values, as in 'classname is "light"',
                        since a query that refers to unknown names searches
                        for its own text instead
  show <map> <path>     one node or face in full
  fgd <map> <classname> the entity definition of a class, with base classes
                        resolved
  apply <map> <batch-file | ->
                        run a JSON array of ops, read from a file or stdin,
                        all or nothing, and write the map once
  <op> <map> --<arg> <value>...
                        run one op; tbcli <op> --help lists its arguments.
                        An op's target is --target <path>, or
                        --query '<ql>' --expect <n | any>
  verify <map>          run the in-process checks: load diagnostics, the
                        editor's validators and tbcli's own checks; exits 4
                        with --fail-on. Missing materials are checked only
                        with a Quake directory, else the cli_checks stage
                        lists missing_material as unchecked. With --compile,
                        also compile the map and report the compilers' logs
                        and any leak
  rooms <map>           a heuristic graph of the map's rooms: runs qbsp (never
                        vis or light) on the map's bytes in a temporary
                        directory, first as verify does to find a leak, then
                        with -nodetail -forceprt1, and merges the leafs of that
                        run into rooms, cutting at constrictions and where the
                        contents change. A leaking map fails with map_leaks,
                        carrying the leak issue
  ops [--json]          the JSON Schema of every op and the issue types
  guide [<topic>]       start here: how to work on maps with tbcli, as Markdown.
                        Without a topic, the edit and verify loop, the rules and
                        the topics:
                        {}
  guide {} [<map>]
                        a topic that depends on the project, read from the
                        tbcli.json found by walking up from the map, or else
                        from the working directory
  guide --install <dir> write into dir a SKILL.md and AGENTS.snippet.md, an
                        optional snippet to paste into an agents file; both
                        only point agents at tbcli guide

node paths:
  layer:<n>/group:<n>/entity:<n>/brush:<n>/face:<n>, where each index counts
  the preceding siblings of the same kind in file order and layer:0 is the
  default layer; world is worldspawn. Layers and groups can also be named by
  their persistent id, as in layer:id:<id> or group:id:<id>, and a path can
  start with group:id:<id>. Paths are only valid for the rev they came from.

options:
  --format json|text    output format (default: json)
  --game-config <path>  game config, overrides tbcli.json "game_config"
  --fgd <path>          FGD file, overrides tbcli.json "fgd"
  --quake-dir <path>    Quake directory, overrides tbcli.json "quake_dir"
  --map-format valve    map format, overrides tbcli.json "format"
  --key-entities <classes>
                        comma-separated classes describe lists in full,
                        overrides tbcli.json "describe.key_entities"
  --qbsp <path>         qbsp executable, overrides tbcli.json
                        "compilers.qbsp"; a bare name is looked up in PATH
  --vis <path>          vis executable, overrides tbcli.json "compilers.vis"
  --light <path>        light executable, overrides tbcli.json
                        "compilers.light"
  --limit <n>           query, verify, rooms: return at most n matches, issues,
                        rooms or cells (default: 200)
  --offset <n>          query, verify, rooms: skip the first n matches, issues,
                        rooms or cells (default: 0)
  --count-only          query: return only the number of matches
  --within <box>        query, rooms: keep matches, rooms or cells whose
                        bounds lie inside the box
  --intersects <box>    query: keep matches whose bounds overlap the box
  --touching <path>     query: keep matches whose bounds overlap or touch the
                        bounds of the node or face at the path (not world),
                        leaving out that node, what contains it and what it
                        contains
                        (spatial filters compare bounding boxes and tolerate
                        0.01 units, so boxes that only touch intersect; a box
                        is "x1,y1,z1,x2,y2,z2")
  --raw                 show: add the plane points of faces; rooms: add the
                        polygons of each opening's cell portals
  --type <types>        verify: list only issues of these comma-separated types
  --min-severity <s>    verify: list only issues of at least severity s
                        (error, warning or info)
  --target <path | ql>  verify: list only issues on the node or face at the
                        path, or on the nodes the query matches, or inside them;
                        a value that starts with world, layer: or group: is a
                        path, anything else a query
  --fail-on <s>         verify: exit 4 if an issue that tbcli.json does not
                        suppress has at least severity s
  --compile[=bsp|vis|light]
                        verify: also run qbsp (with -keepprt, without
                        -leaktest) on the verified bytes in a temporary
                        directory; =vis then runs vis -fast, =light runs vis
                        -fast and light. Turns the logs into compiler issues
                        and a leak into a leak issue; a leak or a failed qbsp
                        skips vis and light, and a failed run is a
                        compile_fatal issue, not an exit code
  --timeout <seconds>   verify --compile, rooms: stop each compiler after this
                        long, overriding the timeouts in tbcli.json (defaults:
                        qbsp 300, vis 300, light 600)
  --artifacts <dir>     verify --compile: keep the .bsp, .pts, .prt and logs
                        in dir, replacing those of an earlier run, with a
                        sidecar <map>.bsp.tbcli.json holding {{rev, leaked}};
                        a failed run keeps no .bsp or .prt. Without it, they
                        are deleted
  --from-artifacts <dir>
                        rooms: read the .bsp, .prt and sidecar that verify
                        --compile --artifacts kept in dir instead of running
                        qbsp; they must be of the map's rev, must not have
                        leaked, and the .prt must have one cell per leaf (no
                        detail or liquids), else stale_artifact
  --split-ratio <r>     rooms: cut where the opening is less than r times the
                        cross-section of the space on either side (0 to 1,
                        default: 0.5)
  --cells               rooms: list the unmerged cells (the .prt's leafs) and
                        their portals instead of the rooms
  --room <id>           rooms: list only the room with this id, or its cells;
                        nothing if there is no such room
  --json                ops: print JSON (the default)
  --install <dir>       guide: write the agent files into dir instead
  -h, --help            print this help
  --version             print the version

mutating options (apply and every op):
{}
ops:
{}
tbcli.json is found by walking up from the map's directory.
)";

std::string usageText()
{
  auto ops = std::string{};
  for (const auto& def : opDefs())
  {
    ops += fmt::format("  {}\n", def.name);
  }
  auto configTopics = std::vector<std::string_view>{};
  std::ranges::copy_if(
    guideTopics(), std::back_inserter(configTopics), guideTopicReadsConfig);
  return fmt::format(
    fmt::runtime(UsageTemplate),
    kdl::str_join(guideTopics(), ", "),
    kdl::str_join(configTopics, "|"),
    mutatingOptionsHelp(),
    ops);
}

/** Which commands accept a command option. */
enum class AcceptedBy
{
  /** the command named by the option */
  Command,
  /** apply and every op */
  MutatingCommands,
};

/** An option that only one command, or only the mutating commands, accept. */
struct CommandOption
{
  std::string_view name;
  /** The command that accepts the option, unless acceptedBy is MutatingCommands. */
  std::string_view command;
  AcceptedBy acceptedBy = AcceptedBy::Command;
};

bool isMutatingCommand(const std::string_view command)
{
  return command == "apply" || findOpDef(command) != nullptr;
}

constexpr auto CommandOptions = std::array<CommandOption, 33>{{
  {"--key-entities", "describe"},
  {"--limit", "query"},
  {"--limit", "verify"},
  {"--limit", "rooms"},
  {"--offset", "query"},
  {"--offset", "verify"},
  {"--offset", "rooms"},
  {"--count-only", "query"},
  {"--within", "query"},
  {"--within", "rooms"},
  {"--intersects", "query"},
  {"--touching", "query"},
  {"--raw", "show"},
  {"--raw", "rooms"},
  {"--type", "verify"},
  {"--min-severity", "verify"},
  {"--target", "verify"},
  {"--fail-on", "verify"},
  {"--compile", "verify"},
  {"--timeout", "verify"},
  {"--timeout", "rooms"},
  {"--artifacts", "verify"},
  {"--from-artifacts", "rooms"},
  {"--split-ratio", "rooms"},
  {"--cells", "rooms"},
  {"--room", "rooms"},
  {"--json", "ops"},
  {"--install", "guide"},
  {"--expect-rev", {}, AcceptedBy::MutatingCommands},
  {"--dry-run", {}, AcceptedBy::MutatingCommands},
  {"-o", {}, AcceptedBy::MutatingCommands},
  {"--allow-locked", {}, AcceptedBy::MutatingCommands},
  {"--allow-lossy", {}, AcceptedBy::MutatingCommands},
}};

enum class OutputFormat
{
  Json,
  Text,
};

class StringLogger : public Logger
{
private:
  std::string& m_out;

public:
  explicit StringLogger(std::string& out)
    : m_out{out}
  {
  }

private:
  void doLog(const LogLevel level, const std::string_view message) override
  {
    const auto levelName = [&]() {
      switch (level)
      {
      case LogLevel::Debug:
        return "debug";
      case LogLevel::Info:
        return "info";
      case LogLevel::Warn:
        return "warning";
      case LogLevel::Error:
        return "error";
      }
      return "info";
    }();
    m_out += fmt::format("{}: {}\n", levelName, message);
  }
};

struct Arguments
{
  std::vector<std::string> positionals;
  OutputFormat outputFormat = OutputFormat::Json;
  ConfigOverrides configOverrides;
  /** --limit and --offset, which query and verify share */
  Paging paging;
  QueryOptions queryOptions;
  bool raw = false;
  VerifyOptions verifyOptions;
  /** --compile */
  bool compile = false;
  /** --timeout and --artifacts; the compilers come from the configuration */
  CompileOptions compileOptions;
  /** --split-ratio, --cells, --room and --from-artifacts */
  RoomsOptions roomsOptions;
  ApplyOptions applyOptions;
  /** guide --install: the directory to write the agent files into */
  std::optional<std::filesystem::path> guideInstall;
  /** The flags of an op's subcommand, as (name without dashes, value). */
  OpFlags opFlags;
  /** The options given that only some commands accept. */
  std::vector<std::string> commandOptions;
  bool help = false;
  bool version = false;
};

/** The non-empty elements of a comma-separated list. */
std::vector<std::string> splitList(const std::string_view str)
{
  auto result = std::vector<std::string>{};
  for (const auto element : str | std::views::split(','))
  {
    if (!element.empty())
    {
      result.emplace_back(element.begin(), element.end());
    }
  }
  return result;
}

size_t parseCount(const std::string_view name, const std::string& value)
{
  const auto result = parseUnsigned(value);
  if (!result)
  {
    throw usageError(
      fmt::format("{} requires a non-negative integer, not '{}'", name, value));
  }
  return *result;
}

Severity parseSeverityOption(const std::string_view name, const std::string& value)
{
  const auto result = parseSeverity(value);
  if (!result)
  {
    throw usageError(
      fmt::format("{} requires error, warning or info, not '{}'", name, value));
  }
  return *result;
}

const IssueTypeDef* parseIssueTypeOption(const std::string& value)
{
  const auto* type = findIssueType(value);
  if (!type)
  {
    throw usageError(fmt::format("--type: unknown issue type '{}'", value));
  }
  return type;
}

CompileLevel parseCompileLevel(const std::string& value)
{
  if (value == "bsp")
  {
    return CompileLevel::Bsp;
  }
  if (value == "vis")
  {
    return CompileLevel::Vis;
  }
  if (value == "light")
  {
    return CompileLevel::Light;
  }
  throw usageError(fmt::format("--compile accepts bsp, vis or light, not '{}'", value));
}

/** A box given as six numbers `x1,y1,z1,x2,y2,z2`, separated by commas or spaces. */
vm::bbox3d parseBox(const std::string_view name, const std::string& value)
{
  const auto numbers = parseNumberList(value);
  if (!numbers)
  {
    throw usageError(fmt::format("{}: '{}' is not a list of numbers", name, value));
  }

  if (numbers->size() != 6)
  {
    throw usageError(
      fmt::format("{} requires a box x1,y1,z1,x2,y2,z2, not '{}'", name, value));
  }

  const auto min = vm::vec3d{(*numbers)[0], (*numbers)[1], (*numbers)[2]};
  const auto max = vm::vec3d{(*numbers)[3], (*numbers)[4], (*numbers)[5]};
  if (!vm::bbox3d::is_valid(min, max))
  {
    throw usageError(
      fmt::format(
        "{}: the first corner of '{}' must be the minimum on every axis", name, value));
  }
  return vm::bbox3d{min, max};
}

Arguments parseArguments(const std::vector<std::string>& args)
{
  auto result = Arguments{};

  for (size_t i = 0; i < args.size(); ++i)
  {
    const auto& arg = args[i];
    if (arg == "-h")
    {
      result.help = true;
      continue;
    }
    if (!arg.starts_with("-") || arg == "-")
    {
      // a lone "-" is reserved for reading from stdin
      result.positionals.push_back(arg);
      continue;
    }
    if (arg == "-o")
    {
      if (i + 1 >= args.size())
      {
        throw usageError("-o requires a value");
      }
      result.applyOptions.output = std::filesystem::path{args[++i]};
      result.commandOptions.emplace_back(arg);
      continue;
    }
    if (!arg.starts_with("--"))
    {
      throw usageError(fmt::format("unknown option {}", arg));
    }

    auto name = std::string_view{arg};
    auto inlineValue = std::optional<std::string>{};
    if (const auto eq = name.find('='); eq != std::string_view::npos)
    {
      inlineValue = std::string{name.substr(eq + 1)};
      name = name.substr(0, eq);
    }

    const auto value = [&]() -> std::string {
      if (inlineValue)
      {
        return *inlineValue;
      }
      if (i + 1 >= args.size())
      {
        throw usageError(fmt::format("{} requires a value", name));
      }
      return args[++i];
    };

    const auto* opDef =
      result.positionals.empty() ? nullptr : findOpDef(result.positionals.front());
    const auto opCommand = opDef != nullptr;

    const auto noValue = [&]() {
      if (inlineValue)
      {
        throw usageError(fmt::format("{} does not take a value", name));
      }
    };

    // an op's own flags, such as --target, are checked against its definition when the
    // op is built
    const auto addOpFlag = [&]() {
      const auto flag = std::string{name.substr(2)};
      if (!inlineValue && isBoolFlag(*opDef, flag))
      {
        // a bool flag alone means true; a following true or false is its value
        const auto hasValue = i + 1 < args.size() && parseBool(args[i + 1]).has_value();
        result.opFlags.emplace_back(flag, hasValue ? args[++i] : "true");
        return;
      }
      result.opFlags.emplace_back(flag, value());
    };

    if (opCommand && definesFlag(*opDef, name.substr(2)))
    {
      // an op's arguments take precedence over options of the same name, such as
      // set_uv's --offset over the paging option
      addOpFlag();
      continue;
    }

    if (name == "--help")
    {
      noValue();
      result.help = true;
    }
    else if (name == "--version")
    {
      noValue();
      result.version = true;
    }
    else if (name == "--format")
    {
      const auto outputFormat = value();
      if (outputFormat == "json")
      {
        result.outputFormat = OutputFormat::Json;
      }
      else if (outputFormat == "text")
      {
        result.outputFormat = OutputFormat::Text;
      }
      else
      {
        throw usageError(
          fmt::format("unknown output format '{}', expected json or text", outputFormat));
      }
    }
    else if (name == "--game-config")
    {
      result.configOverrides.gameConfig = std::filesystem::path{value()};
    }
    else if (name == "--fgd")
    {
      result.configOverrides.fgd.emplace_back(value());
    }
    else if (name == "--quake-dir")
    {
      result.configOverrides.quakeDir = std::filesystem::path{value()};
    }
    else if (name == "--map-format")
    {
      // validated with the rest of the configuration
      result.configOverrides.mapFormat = value();
    }
    else if (name == "--key-entities")
    {
      result.configOverrides.keyEntities = splitList(value());
    }
    else if (findCompilerTool(name.substr(2)))
    {
      // --qbsp, --vis and --light
      result.configOverrides.compilers.insert_or_assign(
        std::string{name.substr(2)}, std::filesystem::path{value()});
    }
    else if (name == "--limit")
    {
      result.paging.limit = parseCount(name, value());
    }
    else if (name == "--offset")
    {
      result.paging.offset = parseCount(name, value());
    }
    else if (name == "--count-only")
    {
      noValue();
      result.queryOptions.countOnly = true;
    }
    else if (name == "--within")
    {
      result.queryOptions.within = parseBox(name, value());
    }
    else if (name == "--intersects")
    {
      result.queryOptions.intersects = parseBox(name, value());
    }
    else if (name == "--touching")
    {
      result.queryOptions.touching = value();
    }
    else if (name == "--raw")
    {
      noValue();
      result.raw = true;
    }
    else if (name == "--json")
    {
      // JSON is the default output; the flag is accepted for explicitness
      noValue();
    }
    else if (name == "--install")
    {
      result.guideInstall = std::filesystem::path{value()};
    }
    else if (name == "--expect-rev")
    {
      result.applyOptions.expectRev = value();
    }
    else if (name == "--dry-run")
    {
      noValue();
      result.applyOptions.dryRun = true;
    }
    else if (name == "--allow-locked")
    {
      noValue();
      result.applyOptions.allowLocked = true;
    }
    else if (name == "--allow-lossy")
    {
      noValue();
      result.applyOptions.allowLossy = true;
    }
    else if (opCommand)
    {
      // the flags that the op does not define, such as --query, or unknown ones that
      // building the op refuses. This must come before the verify options below, which
      // share names with op flags.
      addOpFlag();
      continue;
    }
    else if (name == "--type")
    {
      std::ranges::transform(
        splitList(value()),
        std::back_inserter(result.verifyOptions.types),
        parseIssueTypeOption);
    }
    else if (name == "--min-severity")
    {
      result.verifyOptions.minSeverity = parseSeverityOption(name, value());
    }
    else if (name == "--target")
    {
      result.verifyOptions.target = value();
    }
    else if (name == "--fail-on")
    {
      result.verifyOptions.failOn = parseSeverityOption(name, value());
    }
    else if (name == "--compile")
    {
      // the value is optional, so --compile never takes the next argument
      result.compileOptions.level = parseCompileLevel(inlineValue.value_or("bsp"));
      result.compile = true;
    }
    else if (name == "--timeout")
    {
      const auto seconds = value();
      const auto parsed = parseDecimal(seconds);
      if (!parsed || *parsed <= 0.0)
      {
        throw usageError(
          fmt::format(
            "--timeout requires a positive number of seconds, not '{}'", seconds));
      }
      result.compileOptions.timeout = toMilliseconds(*parsed);
    }
    else if (name == "--artifacts")
    {
      result.compileOptions.artifacts =
        std::filesystem::absolute(std::filesystem::path{value()});
    }
    else if (name == "--from-artifacts")
    {
      result.roomsOptions.fromArtifacts =
        std::filesystem::absolute(std::filesystem::path{value()});
    }
    else if (name == "--split-ratio")
    {
      const auto ratio = value();
      const auto parsed = parseDecimal(ratio);
      if (!parsed || *parsed < 0.0 || *parsed > 1.0)
      {
        throw usageError(
          fmt::format("--split-ratio requires a number from 0 to 1, not '{}'", ratio));
      }
      result.roomsOptions.splitRatio = *parsed;
    }
    else if (name == "--cells")
    {
      noValue();
      result.roomsOptions.cells = true;
    }
    else if (name == "--room")
    {
      result.roomsOptions.room = parseCount(name, value());
    }
    else
    {
      throw usageError(fmt::format("unknown option {}", name));
    }

    if (
      std::ranges::find(CommandOptions, name, &CommandOption::name)
      != CommandOptions.end())
    {
      result.commandOptions.emplace_back(name);
    }
  }

  return result;
}

std::string render(const Json& json, const OutputFormat outputFormat)
{
  return outputFormat == OutputFormat::Text ? toText(json) : json.dump() + "\n";
}

/**
 * Checks the arguments of a command: the required arguments, each described for the
 * error message, at most `optional` more, and no options that belong to another command.
 */
void checkArguments(
  const Arguments& arguments,
  const std::string_view command,
  const std::vector<std::string_view>& required,
  const size_t optional = 0)
{
  const auto given = arguments.positionals.size() - 1;
  if (given < required.size())
  {
    throw usageError(fmt::format("{} requires {}", command, required[given]));
  }
  if (given > required.size() + optional)
  {
    throw usageError(
      fmt::format(
        "unexpected argument '{}'",
        arguments.positionals[required.size() + optional + 1]));
  }
  for (const auto& option : arguments.commandOptions)
  {
    // an option that several commands accept has an entry for each of them
    const auto accepted =
      std::ranges::any_of(CommandOptions, [&](const auto& commandOption) {
        return commandOption.name == option
               && (commandOption.acceptedBy == AcceptedBy::MutatingCommands
                     ? isMutatingCommand(command)
                     : commandOption.command == command);
      });
    if (!accepted)
    {
      throw usageError(fmt::format("{} does not accept {}", command, option));
    }
  }
}

/** Loads the map given as the command's first argument. */
std::pair<LoadedMap, Config> loadMapArgument(const Arguments& arguments, Logger& logger)
{
  const auto mapPath = std::filesystem::path{arguments.positionals[1]};
  auto config = loadConfig(mapPath, arguments.configOverrides);
  auto loadedMap = loadMap(mapPath, config, logger);
  return {std::move(loadedMap), std::move(config)};
}

/** Reads the batch of an apply command from a file or, given "-", from stdin. */
Json readBatch(const std::string& source, const std::string_view in)
{
  auto text = std::string{in};
  if (source != "-")
  {
    auto contents = readFile(std::filesystem::path{source});
    if (!contents)
    {
      throw CliError{
        ErrorCode::FileNotFound, fmt::format("cannot read batch {}", source)};
    }
    text = std::move(*contents);
  }

  auto batch = parseJson(text);
  if (!batch)
  {
    const auto error = std::get<JsonParseError>(batch.error());
    throw CliError{
      ErrorCode::InvalidBatch,
      fmt::format("the batch is not valid JSON: {}", error.msg),
      std::nullopt,
      error.line};
  }
  return std::move(batch).value();
}

/** Output that is printed as it is, whatever the output format, such as Markdown. */
struct RawOutput
{
  std::string text;
};

/** What a command prints, and its exit code. */
struct CommandResult
{
  std::variant<Json, RawOutput> output;
  int exitCode = ExitCode::Ok;

  // NOLINTNEXTLINE(google-explicit-constructor): most commands only produce output
  CommandResult(Json output_)
    : output{std::move(output_)}
  {
  }

  // NOLINTNEXTLINE(google-explicit-constructor): like the Json constructor
  CommandResult(RawOutput output_)
    : output{std::move(output_)}
  {
  }

  CommandResult(Json output_, const int exitCode_)
    : output{std::move(output_)}
    , exitCode{exitCode_}
  {
  }
};

Json runOps(const Arguments& arguments, const std::vector<Op>& ops, Logger& logger)
{
  auto [loadedMap, config] = loadMapArgument(arguments, logger);
  return apply(loadedMap, ops, arguments.applyOptions, config.verify);
}

CommandResult runGuide(const Arguments& arguments)
{
  const auto& positionals = arguments.positionals;
  if (arguments.guideInstall)
  {
    checkArguments(arguments, "guide", {});
    return installGuide(std::filesystem::absolute(*arguments.guideInstall));
  }

  // a topic that reads tbcli.json takes an optional map to look for it from
  const auto readsConfig =
    positionals.size() > 1 && guideTopicReadsConfig(positionals[1]);
  checkArguments(arguments, "guide", {}, readsConfig ? 2 : 1);
  if (positionals.size() == 1)
  {
    return RawOutput{guideText(std::nullopt)};
  }

  const auto& topic = positionals[1];
  if (!readsConfig)
  {
    // reading the guide does not depend on a valid tbcli.json
    return RawOutput{guideText(topic)};
  }

  // the map's file name is not used, so the working directory needs none
  const auto searchFrom = positionals.size() > 2
                            ? std::filesystem::path{positionals[2]}
                            : std::filesystem::current_path() / ConfigFileName;
  return RawOutput{
    guideText(topic, loadConfig(searchFrom, arguments.configOverrides).guide)};
}

CommandResult runCommand(
  const Arguments& arguments, const std::string_view in, Logger& logger)
{
  const auto& positionals = arguments.positionals;
  const auto& command = positionals.front();

  if (command == "describe")
  {
    checkArguments(arguments, command, {"a map file"});
    auto [loadedMap, config] = loadMapArgument(arguments, logger);
    const auto issueReport = collectIssues(loadedMap, config.verify);
    return describe(loadedMap, config.keyEntities, issueReport);
  }
  if (command == "query")
  {
    checkArguments(arguments, command, {"a map file"}, 1);
    auto [loadedMap, config] = loadMapArgument(arguments, logger);
    auto options = arguments.queryOptions;
    options.paging = arguments.paging;
    if (positionals.size() > 2)
    {
      options.query = positionals[2];
    }
    return query(loadedMap, options);
  }
  if (command == "show")
  {
    checkArguments(arguments, command, {"a map file", "a node path"});
    auto [loadedMap, config] = loadMapArgument(arguments, logger);
    return show(loadedMap, positionals[2], arguments.raw);
  }
  if (command == "fgd")
  {
    checkArguments(arguments, command, {"a map file", "a classname"});
    const auto [loadedMap, config] = loadMapArgument(arguments, logger);
    return fgd(loadedMap, positionals[2]);
  }
  if (command == "apply")
  {
    checkArguments(arguments, command, {"a map file", "a batch file or -"});
    // the batch is checked before the map is loaded
    const auto ops = parseBatch(readBatch(positionals[2], in));
    return runOps(arguments, ops, logger);
  }
  if (command == "verify")
  {
    checkArguments(arguments, command, {"a map file"});
    const auto& compileOptions = arguments.compileOptions;
    if (!arguments.compile && (compileOptions.timeout || compileOptions.artifacts))
    {
      throw usageError(
        fmt::format(
          "{} requires --compile", compileOptions.timeout ? "--timeout" : "--artifacts"));
    }
    auto [loadedMap, config] = loadMapArgument(arguments, logger);
    auto options = arguments.verifyOptions;
    options.paging = arguments.paging;
    if (arguments.compile)
    {
      options.compile = compileOptions;
      options.compile->compilers = config.compilers;
    }
    auto [output, failed] = verify(loadedMap, config.verify, options);
    return {std::move(output), failed ? ExitCode::FailOn : ExitCode::Ok};
  }
  if (command == "rooms")
  {
    checkArguments(arguments, command, {"a map file"});
    auto options = arguments.roomsOptions;
    options.paging = arguments.paging;
    options.within = arguments.queryOptions.within;
    options.raw = arguments.raw;
    options.timeout = arguments.compileOptions.timeout;
    if (options.fromArtifacts && options.timeout)
    {
      throw usageError("--timeout cannot be used with --from-artifacts");
    }
    auto [loadedMap, config] = loadMapArgument(arguments, logger);
    return rooms(loadedMap, config, options);
  }
  if (command == "ops")
  {
    checkArguments(arguments, command, {});
    return opsJson();
  }
  if (command == "guide")
  {
    return runGuide(arguments);
  }
  if (const auto* def = findOpDef(command))
  {
    checkArguments(arguments, command, {"a map file"});
    return runOps(arguments, {parseOpFlags(*def, arguments.opFlags)}, logger);
  }

  throw usageError(fmt::format("unknown command '{}'", command));
}

} // namespace

bool readsStdin(const std::vector<std::string>& args)
{
  try
  {
    const auto arguments = parseArguments(args);
    const auto& positionals = arguments.positionals;
    return !arguments.help && positionals.size() == 3 && positionals[0] == "apply"
           && positionals[2] == "-";
  }
  catch (const CliError&)
  {
    // run reports the error without reading stdin
    return false;
  }
}

RunResult run(const std::vector<std::string>& args, const std::string_view in)
{
  auto result = RunResult{};
  auto logger = StringLogger{result.err};

  try
  {
    const auto arguments = parseArguments(args);
    if (arguments.help)
    {
      const auto* def = arguments.positionals.empty()
                          ? nullptr
                          : findOpDef(arguments.positionals.front());
      result.out = def ? opHelp(*def) : usageText();
      return result;
    }
    if (arguments.version)
    {
      result.out = fmt::format("tbcli {}\n", VERSION_STR);
      return result;
    }
    if (arguments.positionals.empty())
    {
      throw usageError("no command given");
    }

    auto [output, exitCode] = runCommand(arguments, in, logger);
    result.out = std::visit(
      kdl::overload(
        [&](const Json& json) { return render(json, arguments.outputFormat); },
        [](RawOutput& raw) { return std::move(raw.text); }),
      output);
    result.exitCode = exitCode;
  }
  catch (const CliError& e)
  {
    // Errors are always reported as a JSON envelope so that callers can rely on it.
    result.out = e.toJson().dump() + "\n";
    result.exitCode = e.exitCode();
    // the usage helps with a malformed command line, not with a malformed batch
    if (e.code() == ErrorCode::Usage)
    {
      result.err += usageText();
    }
  }
  catch (const std::exception& e)
  {
    result.out = CliError{ErrorCode::InternalError, e.what()}.toJson().dump() + "\n";
    result.exitCode = ExitCode::Failed;
  }

  return result;
}

} // namespace tb::cli
