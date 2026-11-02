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
#include "cli/Json.h"
#include "cli/LoadMap.h"
#include "cli/TextFormat.h"
#include "version/Version.h"

#include <fmt/format.h>

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

options:
  --format json|text    output format (default: json)
  --game-config <path>  game config, overrides tbcli.json "game_config"
  --fgd <path>          FGD file, overrides tbcli.json "fgd"
  --quake-dir <path>    Quake directory, overrides tbcli.json "quake_dir"
  --map-format valve    map format, overrides tbcli.json "format"
  --key-entities <classes>
                        comma-separated classes describe lists in full,
                        overrides tbcli.json "describe.key_entities"
  -h, --help            print this help
  --version             print the version

tbcli.json is found by walking up from the map's directory.
)";

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
    else
    {
      throw usageError(fmt::format("unknown option {}", name));
    }
  }

  return result;
}

std::string render(const Json& json, const OutputFormat outputFormat)
{
  return outputFormat == OutputFormat::Text ? toText(json) : json.dump() + "\n";
}

std::filesystem::path mapArgument(const Arguments& arguments, std::string_view command)
{
  if (arguments.positionals.size() < 2)
  {
    throw usageError(fmt::format("{} requires a map file", command));
  }
  if (arguments.positionals.size() > 2)
  {
    throw usageError(fmt::format("unexpected argument '{}'", arguments.positionals[2]));
  }
  return std::filesystem::path{arguments.positionals[1]};
}

Json runCommand(const Arguments& arguments, Logger& logger)
{
  const auto& command = arguments.positionals.front();
  if (command == "describe")
  {
    const auto mapPath = mapArgument(arguments, command);
    const auto config = loadConfig(mapPath, arguments.configOverrides);
    const auto loadedMap = loadMap(mapPath, config, logger);
    return describe(loadedMap, config.keyEntities);
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
