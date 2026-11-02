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
#include "cli/CliError.h"
#include "cli/Config.h"
#include "cli/Describe.h"
#include "cli/Fgd.h"
#include "cli/Json.h"
#include "cli/LoadMap.h"
#include "cli/ParseUtils.h"
#include "cli/Query.h"
#include "cli/Show.h"
#include "cli/TextFormat.h"
#include "version/Version.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <exception>
#include <optional>
#include <ranges>
#include <string_view>

namespace tb::cli
{
namespace
{

constexpr auto UsageText = R"(usage: tbcli <command> <map> [options]

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
  --limit <n>           query: return at most n matches (default: 200)
  --offset <n>          query: skip the first n matches (default: 0)
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
  -h, --help            print this help
  --version             print the version

tbcli.json is found by walking up from the map's directory.
)";

/** An option that only one command accepts. */
struct CommandOption
{
  std::string_view name;
  std::string_view command;
};

constexpr auto CommandOptions = std::array<CommandOption, 8>{{
  {"--key-entities", "describe"},
  {"--limit", "query"},
  {"--offset", "query"},
  {"--count-only", "query"},
  {"--within", "query"},
  {"--intersects", "query"},
  {"--touching", "query"},
  {"--raw", "show"},
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
  QueryOptions queryOptions;
  bool raw = false;
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

/** A box given as six numbers `x1,y1,z1,x2,y2,z2`, separated by commas or spaces. */
vm::bbox3d parseBox(const std::string_view name, const std::string& value)
{
  auto numbers = std::vector<double>{};
  for (const auto part : value | std::views::split(','))
  {
    for (const auto word :
         std::string_view{part.begin(), part.end()} | std::views::split(' '))
    {
      const auto str = std::string_view{word.begin(), word.end()};
      if (str.empty())
      {
        continue;
      }

      const auto number = parseDecimal(str);
      if (!number)
      {
        throw usageError(fmt::format("{}: '{}' is not a number", name, str));
      }
      numbers.push_back(*number);
    }
  }

  if (numbers.size() != 6)
  {
    throw usageError(
      fmt::format("{} requires a box x1,y1,z1,x2,y2,z2, not '{}'", name, value));
  }

  const auto min = vm::vec3d{numbers[0], numbers[1], numbers[2]};
  const auto max = vm::vec3d{numbers[3], numbers[4], numbers[5]};
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
      result.queryOptions.limit = parseCount(name, value());
    }
    else if (name == "--offset")
    {
      result.queryOptions.offset = parseCount(name, value());
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
    const auto& commandOption =
      *std::ranges::find(CommandOptions, option, &CommandOption::name);
    if (commandOption.command != command)
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

Json runCommand(const Arguments& arguments, Logger& logger)
{
  const auto& positionals = arguments.positionals;
  const auto& command = positionals.front();

  if (command == "describe")
  {
    checkArguments(arguments, command, {"a map file"});
    const auto [loadedMap, config] = loadMapArgument(arguments, logger);
    return describe(loadedMap, config.keyEntities);
  }
  if (command == "query")
  {
    checkArguments(arguments, command, {"a map file"}, 1);
    auto [loadedMap, config] = loadMapArgument(arguments, logger);
    auto options = arguments.queryOptions;
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

  throw usageError(fmt::format("unknown command '{}'", command));
}

} // namespace

RunResult run(const std::vector<std::string>& args, std::string_view /* in */)
{
  auto result = RunResult{};
  auto logger = StringLogger{result.err};

  try
  {
    const auto arguments = parseArguments(args);
    if (arguments.help)
    {
      result.out = UsageText;
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

    result.out = render(runCommand(arguments, logger), arguments.outputFormat);
    result.exitCode = ExitCode::Ok;
  }
  catch (const CliError& e)
  {
    // Errors are always reported as a JSON envelope so that callers can rely on it.
    result.out = e.toJson().dump() + "\n";
    result.exitCode = e.exitCode();
    if (e.exitCode() == ExitCode::Usage)
    {
      result.err += UsageText;
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
