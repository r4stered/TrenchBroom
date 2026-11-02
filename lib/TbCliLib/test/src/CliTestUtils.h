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
#include "cli/Run.h"
#include "fs/TestEnvironment.h"

#include <filesystem>
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

/** Calls tbcli in-process. Paths are passed as UTF-8 strings. */
RunResult runCli(const std::vector<std::string>& args, std::string_view in = {});

/** The argument string for a path. */
std::string arg(const std::filesystem::path& path);

/** Parses stdout as JSON, failing the test if it is not valid JSON. */
Json parseOutput(const RunResult& result);

} // namespace tb::cli

template <>
struct Catch::StringMaker<tb::cli::Json>
{
  static std::string convert(const tb::cli::Json& json) { return json.dump(); }
};
