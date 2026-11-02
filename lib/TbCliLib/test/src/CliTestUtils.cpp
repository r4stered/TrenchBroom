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

#include "CliTestUtils.h"

#include "TestEnvironment.h"
#include "cli/Config.h"

#include <fstream>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{

std::filesystem::path fixturePath(const std::filesystem::path& relativePath)
{
  return getFixtureRoot() / "test" / relativePath;
}

std::filesystem::path quakeGameConfigPath()
{
  return getFixtureRoot() / "games" / "Quake" / "GameConfig.cfg";
}

std::filesystem::path quakeFgdPath()
{
  return getFixtureRoot() / "games" / "Quake" / "Quake.fgd";
}

std::filesystem::path texturesFixturePath()
{
  return getFixtureRoot() / "textures";
}

CliTestEnvironment::CliTestEnvironment() = default;

std::filesystem::path CliTestEnvironment::path(
  const std::filesystem::path& relativePath) const
{
  return m_env.dir() / relativePath;
}

std::filesystem::path CliTestEnvironment::copyFile(
  const std::filesystem::path& source, const std::filesystem::path& relativePath)
{
  const auto target = path(relativePath);
  std::filesystem::create_directories(target.parent_path());
  std::filesystem::copy_file(
    source, target, std::filesystem::copy_options::overwrite_existing);
  return target;
}

std::filesystem::path CliTestEnvironment::copyFixture(
  const std::filesystem::path& fixtureRelativePath,
  const std::filesystem::path& relativePath)
{
  return copyFile(fixturePath(fixtureRelativePath), relativePath);
}

std::filesystem::path CliTestEnvironment::writeFile(
  const std::filesystem::path& relativePath, const std::string_view contents)
{
  const auto target = path(relativePath);
  std::filesystem::create_directories(target.parent_path());
  auto stream = std::ofstream{target, std::ios::binary};
  stream << contents;
  REQUIRE(stream.good());
  return target;
}

std::filesystem::path CliTestEnvironment::writeConfig(
  const std::filesystem::path& relativeDir, const Json& config)
{
  return writeFile(relativeDir / "tbcli.json", config.dump());
}

std::string CliTestEnvironment::readFile(const std::filesystem::path& relativePath) const
{
  return m_env.loadFile(relativePath);
}

Json quakeConfig()
{
  auto config = Json::object();
  config.set("format", "valve");
  config.set("game_config", arg(quakeGameConfigPath()));
  config.set("fgd", arg(quakeFgdPath()));
  return config;
}

void setUpMaterials(CliTestEnvironment& env)
{
  env.copyFile(texturesFixturePath() / "cr8_a_excerpt.wad", "textures.wad");
  env.copyFile(texturesFixturePath() / "palette.lmp", "quake/id1/gfx/palette.lmp");

  auto config = quakeConfig();
  config.set("quake_dir", arg(env.path("quake")));
  env.writeConfig({}, config);
}

LoadedMap loadFixtureMap(const std::filesystem::path& fixture, Logger& logger)
{
  auto config = Config{};
  config.gameConfig = quakeGameConfigPath();
  config.fgd = {quakeFgdPath()};
  return loadMap(fixturePath(fixture), config, logger);
}

RunResult runCli(const std::vector<std::string>& args, const std::string_view in)
{
  auto result = run(args, in);
  UNSCOPED_INFO("exit code: " << result.exitCode);
  UNSCOPED_INFO("stdout: " << result.out);
  UNSCOPED_INFO("stderr: " << result.err);
  return result;
}

std::string arg(const std::filesystem::path& path)
{
  return path.string();
}

Json parseOutput(const RunResult& result)
{
  auto json = parseJson(result.out);
  REQUIRE(json.is_success());
  return std::move(json).value();
}

Json parseJsonLiteral(const std::string_view str)
{
  auto json = parseJson(str);
  REQUIRE(json.is_success());
  return std::move(json).value();
}

Json runJson(const std::vector<std::string>& args, const std::string_view in)
{
  const auto result = runCli(args, in);
  REQUIRE(result.exitCode == 0);
  return parseOutput(result);
}

std::string revOf(const std::string& map)
{
  return runJson({"describe", map})["rev"].asString();
}

size_t brushCount(const std::string& map)
{
  return size_t(runJson({"describe", map})["counts"]["brushes"].asNumber());
}

void checkError(
  const RunResult& result,
  const std::string& code,
  const int exitCode,
  const std::optional<size_t> opIndex)
{
  CHECK(result.exitCode == exitCode);
  const auto json = parseOutput(result);
  CHECK(json["error"]["code"] == Json{code});
  CHECK(!json["error"]["message"].asString().empty());
  if (opIndex)
  {
    CHECK(json["error"]["op_index"] == Json{*opIndex});
  }
  else
  {
    CHECK(json["error"].find("op_index") == nullptr);
  }
}

EditFixture::EditFixture(const std::filesystem::path& fixture)
  : fileName{fixture.filename()}
{
  env.writeConfig({}, quakeConfig());
  map = arg(env.copyFixture(fixture, fileName));
  original = env.readFile(fileName);
}

bool EditFixture::unchanged() const
{
  return env.readFile(fileName) == original;
}

RunResult EditFixture::apply(
  const std::string_view batch, const std::vector<std::string>& options)
{
  auto args = std::vector<std::string>{"apply", map, "-"};
  args.insert(args.end(), options.begin(), options.end());
  return runCli(args, batch);
}

} // namespace tb::cli
