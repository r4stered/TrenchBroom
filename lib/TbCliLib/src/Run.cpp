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
#include "cli/Json.h"
#include "cli/LoadMap.h"
#include "cli/Ops.h"
#include "cli/ParseUtils.h"
#include "cli/Query.h"
#include "cli/Show.h"
#include "cli/TextFormat.h"
#include "cli/Verify.h"
#include "version/Version.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <exception>
#include <iterator>
#include <optional>
#include <ranges>
#include <string_view>

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
                        lists missing_material as unchecked
  ops [--json]          the JSON Schema of every op and the issue types

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
  --limit <n>           query, verify: return at most n matches or issues
                        (default: 200)
  --offset <n>          query, verify: skip the first n matches or issues
                        (default: 0)
  --count-only          query: return only the number of matches
  --within <box>        query: keep matches whose bounds lie inside the box
  --intersects <box>    query: keep matches whose bounds overlap the box
  --touching <path>     query: keep matches whose bounds overlap or touch the
                        bounds of the node or face at the path (not world),
                        leaving out that node, what contains it and what it
                        contains
                        (spatial filters compare bounding boxes and tolerate
                        0.01 units, so boxes that only touch intersect; a box
                        is "x1,y1,z1,x2,y2,z2")
  --raw                 show: add the plane points of faces
  --type <types>        verify: list only issues of these comma-separated types
  --min-severity <s>    verify: list only issues of at least severity s
                        (error, warning or info)
  --target <path | ql>  verify: list only issues on the node or face at the
                        path, or on the nodes the query matches, or inside them;
                        a value that starts with world, layer: or group: is a
                        path, anything else a query
  --fail-on <s>         verify: exit 4 if an issue that tbcli.json does not
                        suppress has at least severity s
  --json                ops: print JSON (the default)
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
  return fmt::format(fmt::runtime(UsageTemplate), mutatingOptionsHelp(), ops);
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

constexpr auto CommandOptions = std::array<CommandOption, 20>{{
  {"--key-entities", "describe"},
  {"--limit", "query"},
  {"--limit", "verify"},
  {"--offset", "query"},
  {"--offset", "verify"},
  {"--count-only", "query"},
  {"--within", "query"},
  {"--intersects", "query"},
  {"--touching", "query"},
  {"--raw", "show"},
  {"--type", "verify"},
  {"--min-severity", "verify"},
  {"--target", "verify"},
  {"--fail-on", "verify"},
  {"--json", "ops"},
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
  ApplyOptions applyOptions;
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
      // an op's own flags, such as --target, are checked against its definition when
      // the op is built. This must come before the verify options below, which share
      // names with op flags.
      const auto flag = std::string{name.substr(2)};
      if (!inlineValue && isBoolFlag(*opDef, flag))
      {
        // a bool flag alone means true; a following true or false is its value
        const auto hasValue = i + 1 < args.size() && parseBool(args[i + 1]).has_value();
        result.opFlags.emplace_back(flag, hasValue ? args[++i] : "true");
        continue;
      }
      result.opFlags.emplace_back(flag, value());
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

/** What a command prints, and its exit code. */
struct CommandResult
{
  Json output;
  int exitCode = ExitCode::Ok;

  // NOLINTNEXTLINE(google-explicit-constructor): most commands only produce output
  CommandResult(Json output_)
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
    auto [loadedMap, config] = loadMapArgument(arguments, logger);
    auto options = arguments.verifyOptions;
    options.paging = arguments.paging;
    auto [output, failed] = verify(loadedMap, config.verify, options);
    return {std::move(output), failed ? ExitCode::FailOn : ExitCode::Ok};
  }
  if (command == "ops")
  {
    checkArguments(arguments, command, {});
    return opsJson();
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
    result.out = render(output, arguments.outputFormat);
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
