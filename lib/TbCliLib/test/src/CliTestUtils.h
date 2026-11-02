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

#include "cli/Json.h"
#include "cli/LoadMap.h"
#include "cli/Run.h"
#include "fs/TestEnvironment.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_tostring.hpp>

namespace tb::cli
{

/** A file below the checked-in fixture directory lib/TbCliLib/test/fixture. */
std::filesystem::path fixturePath(const std::filesystem::path& relativePath);

/** The Quake game config shipped with TrenchBroom, copied next to the fixtures. */
std::filesystem::path quakeGameConfigPath();

/** The Quake FGD shipped with TrenchBroom. */
std::filesystem::path quakeFgdPath();

/**
 * A directory holding a Quake palette (palette.lmp) and a texture wad
 * (cr8_a_excerpt.wad, containing the textures "added" and "cr8_czg_1").
 */
std::filesystem::path texturesFixturePath();

/**
 * A temporary directory for one test. Fixture maps are copied into it so that tests
 * can let tbcli read and write them, and tbcli.json files are written into it so that
 * configuration discovery can be exercised.
 */
class CliTestEnvironment
{
private:
  fs::TestEnvironment m_env;

public:
  CliTestEnvironment();

  /** The absolute path of the given path relative to this environment. */
  std::filesystem::path path(const std::filesystem::path& relativePath) const;

  /** Copies a file into this environment and returns its absolute path. */
  std::filesystem::path copyFile(
    const std::filesystem::path& source, const std::filesystem::path& relativePath);

  /** Copies a fixture file into this environment and returns its absolute path. */
  std::filesystem::path copyFixture(
    const std::filesystem::path& fixtureRelativePath,
    const std::filesystem::path& relativePath);

  /** Writes a file into this environment and returns its absolute path. */
  std::filesystem::path writeFile(
    const std::filesystem::path& relativePath, std::string_view contents);

  /** Writes tbcli.json into the given directory of this environment. */
  std::filesystem::path writeConfig(
    const std::filesystem::path& relativeDir, const Json& config);

  std::string readFile(const std::filesystem::path& relativePath) const;
};

/** A tbcli.json that points at the Quake game config and FGD shipped with TrenchBroom. */
Json quakeConfig();

/**
 * Lets tbcli load and check materials in the given environment: copies the texture wad
 * into it as textures.wad, which fixture maps name in their wad property, and writes a
 * tbcli.json with a Quake directory that holds the palette.
 */
void setUpMaterials(CliTestEnvironment& env);

/**
 * Loads a fixture map in-process with the Quake game config and FGD. The logger must
 * outlive the result.
 */
LoadedMap loadFixtureMap(const std::filesystem::path& fixture, Logger& logger);

/** Calls tbcli in-process. Paths are passed as UTF-8 strings. */
RunResult runCli(const std::vector<std::string>& args, std::string_view in = {});

/** The argument string for a path. */
std::string arg(const std::filesystem::path& path);

/** Parses stdout as JSON, failing the test if it is not valid JSON. */
Json parseOutput(const RunResult& result);

/** Parses a JSON literal written in a test, failing the test if it is not valid JSON. */
Json parseJsonLiteral(std::string_view str);

/** Calls tbcli in-process, requires it to succeed and returns its parsed output. */
Json runJson(const std::vector<std::string>& args, std::string_view in = {});

/** The rev that describe reports for the given map. */
std::string revOf(const std::string& map);

/** The number of brushes that describe reports for the given map. */
size_t brushCount(const std::string& map);

/**
 * Checks that the result is an error envelope with the given code and exit code, and
 * with the given `op_index` or none.
 */
void checkError(
  const RunResult& result,
  const std::string& code,
  int exitCode,
  std::optional<size_t> opIndex = std::nullopt);

/** A batch entry that creates a box in the default layer. */
inline constexpr auto CreateBoxOp = std::string_view{
  R"({"op": "create_box", "min": [0, 0, 0], "max": [64, 32, 16], "material": "floor"})"};

/**
 * For edit tests: a fixture map (maps/simple.map unless another is given) copied into a
 * temp dir with a tbcli.json, and its original contents.
 */
struct EditFixture
{
  CliTestEnvironment env;
  std::filesystem::path fileName;
  std::string map;
  std::string original;

  explicit EditFixture(const std::filesystem::path& fixture = "maps/simple.map");

  /** Whether the map file still has its original contents. */
  bool unchanged() const;

  /** Runs `apply <map> -` with the given batch on stdin and the given options. */
  RunResult apply(std::string_view batch, const std::vector<std::string>& options = {});
};

} // namespace tb::cli

template <>
struct Catch::StringMaker<tb::cli::Json>
{
  static std::string convert(const tb::cli::Json& json) { return json.dump(); }
};
