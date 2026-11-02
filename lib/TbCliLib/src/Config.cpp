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

#include "cli/Config.h"

#include "cli/CliError.h"
#include "cli/FileUtils.h"
#include "cli/Json.h"

#include <fmt/format.h>
#include <fmt/std.h>

namespace tb::cli
{
namespace
{

CliError fileConfigError(
  const std::filesystem::path& file,
  const std::string& message,
  const std::optional<size_t> line = std::nullopt)
{
  return configError(fmt::format("{}: {}", file, message), line);
}

std::filesystem::path resolve(
  const std::filesystem::path& base, const std::filesystem::path& path)
{
  return (path.is_absolute() ? path : base / path).lexically_normal();
}

std::optional<std::filesystem::path> readPath(
  const Json& object,
  const std::string_view key,
  const std::filesystem::path& file,
  const std::filesystem::path& base)
{
  const auto& value = object[key];
  if (value.isNull())
  {
    return std::nullopt;
  }
  if (!value.isString())
  {
    throw fileConfigError(file, fmt::format("'{}' must be a string", key));
  }
  return resolve(base, std::filesystem::path{value.asString()});
}

std::vector<std::filesystem::path> readPaths(
  const Json& object,
  const std::string_view key,
  const std::filesystem::path& file,
  const std::filesystem::path& base)
{
  const auto& value = object[key];
  if (value.isNull())
  {
    return {};
  }
  if (value.isString())
  {
    return {resolve(base, std::filesystem::path{value.asString()})};
  }

  const auto typeError = [&]() {
    return fileConfigError(
      file, fmt::format("'{}' must be a string or an array of strings", key));
  };
  if (!value.isArray())
  {
    throw typeError();
  }

  auto result = std::vector<std::filesystem::path>{};
  for (const auto& element : value.asArray())
  {
    if (!element.isString())
    {
      throw typeError();
    }
    result.push_back(resolve(base, std::filesystem::path{element.asString()}));
  }
  return result;
}

std::optional<std::vector<std::string>> readStrings(
  const Json& object, const std::string_view key, const std::filesystem::path& file)
{
  const auto& value = object[key];
  if (value.isNull())
  {
    return std::nullopt;
  }

  const auto typeError = [&]() {
    return fileConfigError(file, fmt::format("'{}' must be an array of strings", key));
  };
  if (!value.isArray())
  {
    throw typeError();
  }

  auto result = std::vector<std::string>{};
  for (const auto& element : value.asArray())
  {
    if (!element.isString())
    {
      throw typeError();
    }
    result.push_back(element.asString());
  }
  return result;
}

void readDescribeConfig(
  Config& config, const Json& object, const std::filesystem::path& file)
{
  const auto& describe = object["describe"];
  if (describe.isNull())
  {
    return;
  }
  if (!describe.isObject())
  {
    throw fileConfigError(file, "'describe' must be an object");
  }
  config.keyEntities = readStrings(describe, "key_entities", file);
}

void readConfigFile(Config& config, const std::filesystem::path& file)
{
  const auto contents = readFile(file);
  if (!contents)
  {
    throw fileConfigError(file, "cannot read file");
  }

  const auto json = parseJson(*contents);
  if (!json)
  {
    const auto error = std::get<JsonParseError>(json.error());
    throw fileConfigError(file, error.msg, error.line);
  }

  const auto& object = json.value();
  if (!object.isObject())
  {
    throw fileConfigError(file, "must contain a JSON object");
  }

  const auto base = file.parent_path();
  if (const auto& mapFormat = object["format"]; !mapFormat.isNull())
  {
    if (!mapFormat.isString())
    {
      throw fileConfigError(file, "'format' must be a string");
    }
    config.mapFormat = mapFormat.asString();
  }
  config.gameConfig = readPath(object, "game_config", file, base);
  config.fgd = readPaths(object, "fgd", file, base);
  config.quakeDir = readPath(object, "quake_dir", file, base);
  readDescribeConfig(config, object, file);
}

} // namespace

std::optional<std::filesystem::path> findConfigFile(const std::filesystem::path& mapPath)
{
  auto dir = std::filesystem::absolute(mapPath).lexically_normal().parent_path();
  while (true)
  {
    const auto candidate = dir / ConfigFileName;
    auto ec = std::error_code{};
    if (std::filesystem::is_regular_file(candidate, ec))
    {
      return candidate;
    }

    const auto parent = dir.parent_path();
    if (parent == dir || parent.empty())
    {
      return std::nullopt;
    }
    dir = parent;
  }
}

Config loadConfig(const std::filesystem::path& mapPath, const ConfigOverrides& overrides)
{
  auto config = Config{};

  if (const auto file = findConfigFile(mapPath))
  {
    config.file = file;
    readConfigFile(config, *file);
  }

  const auto cwd = std::filesystem::current_path();
  if (overrides.mapFormat)
  {
    config.mapFormat = *overrides.mapFormat;
  }
  if (overrides.gameConfig)
  {
    config.gameConfig = resolve(cwd, *overrides.gameConfig);
  }
  if (!overrides.fgd.empty())
  {
    config.fgd.clear();
    for (const auto& fgd : overrides.fgd)
    {
      config.fgd.push_back(resolve(cwd, fgd));
    }
  }
  if (overrides.quakeDir)
  {
    config.quakeDir = resolve(cwd, *overrides.quakeDir);
  }
  if (overrides.keyEntities)
  {
    config.keyEntities = overrides.keyEntities;
  }

  // checked once the overrides are applied, so a flag can override a bad key
  if (config.mapFormat != "valve")
  {
    throw CliError{
      ErrorCode::NotValve220,
      fmt::format(
        "unsupported map format '{}': tbcli supports only Valve 220 maps (\"valve\")",
        config.mapFormat)};
  }

  return config;
}

} // namespace tb::cli
