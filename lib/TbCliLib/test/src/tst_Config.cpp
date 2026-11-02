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
#include "cli/CliError.h"
#include "cli/Config.h"

#include <map>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

namespace tb::cli
{

TEST_CASE("Config")
{
  auto env = CliTestEnvironment{};
  const auto mapPath = env.path("project/maps/sub/test.map");

  SECTION("findConfigFile")
  {
    CHECK(findConfigFile(mapPath) == std::nullopt);

    const auto outer = env.writeConfig("project", Json::object());
    CHECK(findConfigFile(mapPath) == outer);

    const auto inner = env.writeConfig("project/maps", Json::object());
    CHECK(findConfigFile(mapPath) == inner);
  }

  SECTION("loadConfig")
  {
    SECTION("without tbcli.json")
    {
      const auto config = loadConfig(mapPath, {});
      CHECK(config.file == std::nullopt);
      CHECK(config.mapFormat == "valve");
      CHECK(config.gameConfig == std::nullopt);
      CHECK(config.fgd.empty());
      CHECK(config.quakeDir == std::nullopt);
    }

    SECTION("relative paths are resolved against the directory of tbcli.json")
    {
      auto json = Json::object();
      json.set("format", "valve");
      json.set("game_config", "games/Quake/GameConfig.cfg");
      json.set("fgd", "../defs/a.fgd");
      json.set("quake_dir", arg(env.path("elsewhere/quake")));
      const auto file = env.writeConfig("project", json);

      const auto config = loadConfig(mapPath, {});
      CHECK(config.file == file);
      CHECK(config.gameConfig == env.path("project/games/Quake/GameConfig.cfg"));
      CHECK(
        config.fgd
        == std::vector<std::filesystem::path>{env.path("defs/a.fgd").lexically_normal()});
      // absolute paths are kept
      CHECK(config.quakeDir == env.path("elsewhere/quake"));
    }

    SECTION("fgd may be a list")
    {
      auto json = Json::object();
      json.set("fgd", Json{Json::Array{"a.fgd", "b.fgd"}});
      env.writeConfig("project", json);

      CHECK(
        loadConfig(mapPath, {}).fgd
        == std::vector<std::filesystem::path>{
          env.path("project/a.fgd"), env.path("project/b.fgd")});
    }

    SECTION("describe.key_entities")
    {
      auto describe = Json::object();
      describe.set("key_entities", Json{Json::Array{"light", "monster_army"}});
      auto json = Json::object();
      json.set("describe", describe);
      env.writeConfig("project", json);

      CHECK(
        loadConfig(mapPath, {}).keyEntities
        == std::vector<std::string>{"light", "monster_army"});
      CHECK(
        loadConfig(
          mapPath, ConfigOverrides{.keyEntities = std::vector<std::string>{"info_null"}})
          .keyEntities
        == std::vector<std::string>{"info_null"});
    }

    SECTION("verify")
    {
      env.writeFile("project/tbcli.json", R"({"verify": {
        "severity": {"empty_group": "error", "missing_mod": "info"},
        "ignore": [
          {"type": "link_target"},
          {"type": "empty_group", "key": "0123456789ab"},
          {"type": "missing_definition", "query": "classname is \"x\""}
        ]
      }})");

      const auto config = loadConfig(mapPath, {});
      CHECK(
        config.verify.severity
        == std::map<const IssueTypeDef*, Severity>{
          {findIssueType("empty_group"), Severity::Error},
          {findIssueType("missing_mod"), Severity::Info}});
      REQUIRE(config.verify.ignore.size() == 3);
      CHECK(config.verify.ignore[0].type == findIssueType("link_target"));
      CHECK(config.verify.ignore[0].key == std::nullopt);
      CHECK(config.verify.ignore[0].query == std::nullopt);
      CHECK(config.verify.ignore[1].key == "0123456789ab");
      CHECK(config.verify.ignore[2].query == R"(classname is "x")");
    }

    SECTION("overrides replace keys and resolve against the working directory")
    {
      auto json = Json::object();
      json.set("format", "standard");
      json.set("game_config", "a.cfg");
      json.set("fgd", Json{Json::Array{"a.fgd", "b.fgd"}});
      json.set("quake_dir", "quake");
      env.writeConfig("project", json);

      const auto cwd = std::filesystem::current_path();
      const auto config = loadConfig(
        mapPath,
        ConfigOverrides{
          .mapFormat = "valve",
          .gameConfig = "b.cfg",
          .fgd = {"c.fgd"},
          .quakeDir = "other",
        });
      CHECK(config.mapFormat == "valve");
      CHECK(config.gameConfig == cwd / "b.cfg");
      CHECK(config.fgd == std::vector<std::filesystem::path>{cwd / "c.fgd"});
      CHECK(config.quakeDir == cwd / "other");
    }

    SECTION("errors")
    {
      const auto errorCode = [&]() -> std::optional<ErrorCode> {
        try
        {
          loadConfig(mapPath, {});
          return std::nullopt;
        }
        catch (const CliError& e)
        {
          return e.code();
        }
      };

      SECTION("malformed JSON")
      {
        env.writeFile("project/tbcli.json", "{ oops }");
        CHECK(errorCode() == ErrorCode::ConfigError);
      }

      SECTION("not an object")
      {
        env.writeFile("project/tbcli.json", "[]");
        CHECK(errorCode() == ErrorCode::ConfigError);
      }

      SECTION("mistyped key")
      {
        auto json = Json::object();
        json.set("game_config", 42);
        env.writeConfig("project", json);
        CHECK(errorCode() == ErrorCode::ConfigError);
      }

      SECTION("mistyped fgd list element")
      {
        auto json = Json::object();
        json.set("fgd", Json{Json::Array{"a.fgd", true}});
        env.writeConfig("project", json);
        CHECK(errorCode() == ErrorCode::ConfigError);
      }

      SECTION("describe is not an object")
      {
        auto json = Json::object();
        json.set("describe", Json{Json::Array{"light"}});
        env.writeConfig("project", json);
        CHECK(errorCode() == ErrorCode::ConfigError);
      }

      SECTION("describe.key_entities is not a list of strings")
      {
        const auto keyEntities = GENERATE(Json{"light"}, Json{Json::Array{"light", 1}});

        auto describe = Json::object();
        describe.set("key_entities", keyEntities);
        auto json = Json::object();
        json.set("describe", describe);
        env.writeConfig("project", json);
        CHECK(errorCode() == ErrorCode::ConfigError);
      }

      SECTION("malformed verify section")
      {
        const auto verify = GENERATE(
          R"([])",
          R"({"severity": []})",
          R"({"severity": {"no_such_type": "error"}})",
          R"({"severity": {"empty_group": "fatal"}})",
          R"({"ignore": {}})",
          R"({"ignore": [{"key": "0123456789ab"}]})",
          R"({"ignore": [{"type": "no_such_type"}]})",
          R"({"ignore": [{"type": "empty_group", "key": 1}]})",
          R"({"ignore": [{"type": "empty_group", "nodes": "x"}]})",
          R"({"ignore": [{"type": "empty_group", "key": "a", "query": "b"}]})");
        CAPTURE(verify);

        env.writeFile("project/tbcli.json", std::string{R"({"verify": )"} + verify + "}");
        CHECK(errorCode() == ErrorCode::ConfigError);
      }

      SECTION("map format other than valve")
      {
        auto json = Json::object();
        json.set("format", "standard");
        env.writeConfig("project", json);
        CHECK(errorCode() == ErrorCode::NotValve220);
      }
    }
  }
}

} // namespace tb::cli
