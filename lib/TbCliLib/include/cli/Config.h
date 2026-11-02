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
#include <vector>

namespace tb::cli
{

inline constexpr auto ConfigFileName = "tbcli.json";

/**
 * Values given on the command line. Each one that is set replaces the corresponding
 * tbcli.json key. Relative paths are resolved against the working directory.
 */
struct ConfigOverrides
{
  std::optional<std::string> mapFormat;
  std::optional<std::filesystem::path> gameConfig;
  std::vector<std::filesystem::path> fgd;
  std::optional<std::filesystem::path> quakeDir;
  std::optional<std::vector<std::string>> keyEntities;
};

/**
 * The effective configuration for one map: tbcli.json found by walking up from the
 * map, with command line overrides applied. Paths are absolute; relative paths in
 * tbcli.json are resolved against the directory containing it.
 */
struct Config
{
  /** The tbcli.json that was found, if any. */
  std::optional<std::filesystem::path> file;
  /** The map format. Only "valve" is supported. */
  std::string mapFormat = "valve";
  std::optional<std::filesystem::path> gameConfig;
  std::vector<std::filesystem::path> fgd;
  std::optional<std::filesystem::path> quakeDir;
  /** `describe.key_entities`: overrides the classes describe lists in full. */
  std::optional<std::vector<std::string>> keyEntities;
};

/**
 * Returns the nearest tbcli.json in the directory containing the given map or any of
 * its ancestors.
 */
std::optional<std::filesystem::path> findConfigFile(const std::filesystem::path& mapPath);

/**
 * Discovers and reads the configuration for the given map and applies the overrides.
 *
 * @throw CliError if tbcli.json cannot be read or is malformed, or the map format is
 * not Valve 220
 */
Config loadConfig(const std::filesystem::path& mapPath, const ConfigOverrides& overrides);

} // namespace tb::cli
