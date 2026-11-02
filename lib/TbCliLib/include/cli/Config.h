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

#include "cli/IssueTypes.h"

#include <chrono>
#include <filesystem>
#include <map>
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
  /** The qbsp executable, overriding `compilers.qbsp`. */
  std::optional<std::filesystem::path> qbsp;
};

/** A compiler entry of tbcli.json's `compilers`. */
struct ToolConfig
{
  /**
   * The executable: an absolute path, or a bare name that is looked up in PATH when
   * the tool runs.
   */
  std::optional<std::filesystem::path> path;
  /** How long the tool may run before it is stopped. */
  std::optional<std::chrono::milliseconds> timeout;
};

/**
 * tbcli.json's `compilers`. Each tool is given as a path, or as an object `{path,
 * timeout}` with the timeout in seconds.
 */
struct CompilersConfig
{
  ToolConfig qbsp;
};

/**
 * A `verify.ignore` entry: suppresses the issues of a type, or only the one with the
 * given key, or only those on the nodes a TbQl query matches and the nodes and faces
 * inside them.
 */
struct IssueSuppression
{
  const IssueTypeDef* type = nullptr;
  std::optional<std::string> key;
  std::optional<std::string> query;
};

/** The `verify` section of tbcli.json. */
struct VerifyConfig
{
  /** `verify.severity`: severities that replace the default of an issue type. */
  std::map<const IssueTypeDef*, Severity> severity;
  /** `verify.ignore` */
  std::vector<IssueSuppression> ignore;
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
  CompilersConfig compilers;
  VerifyConfig verify;
};

/** A timeout given in seconds, rounded up to whole milliseconds. */
std::chrono::milliseconds toMilliseconds(double seconds);

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
