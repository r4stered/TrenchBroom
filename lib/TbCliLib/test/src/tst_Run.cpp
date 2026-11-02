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

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::cli
{
using namespace Catch::Matchers;

namespace
{

void checkErrorEnvelope(
  const RunResult& result, const std::string& code, const int exitCode)
{
  CHECK(result.exitCode == exitCode);

  const auto json = parseOutput(result);
  REQUIRE(json.isObject());
  CHECK(json.size() == 1);

  const auto& error = json["error"];
  REQUIRE(error.isObject());
  CHECK(error["code"] == Json{code});
  CHECK(error["message"].isString());
  CHECK(!error["message"].asString().empty());
}

double maxY(const RunResult& result)
{
  return parseOutput(result)["bounds"]["max"][1].asNumber();
}

} // namespace

TEST_CASE("run")
{
  auto env = CliTestEnvironment{};

  SECTION("command line")
  {
    SECTION("no arguments is a usage error")
    {
      const auto result = runCli({});
      checkErrorEnvelope(result, "usage", 2);
      CHECK_THAT(result.err, ContainsSubstring("usage: tbcli"));
    }

    SECTION("unknown command is a usage error")
    {
      checkErrorEnvelope(runCli({"frobnicate", "x.map"}), "usage", 2);
    }

    SECTION("unknown option is a usage error")
    {
      checkErrorEnvelope(runCli({"describe", "x.map", "--frobnicate"}), "usage", 2);
    }

    SECTION("option without its value is a usage error")
    {
      checkErrorEnvelope(runCli({"describe", "x.map", "--fgd"}), "usage", 2);
    }

    SECTION("unknown output format is a usage error")
    {
      checkErrorEnvelope(runCli({"describe", "x.map", "--format", "xml"}), "usage", 2);
    }

    SECTION("single dash option is a usage error")
    {
      checkErrorEnvelope(runCli({"describe", "x.map", "-x"}), "usage", 2);
    }

    SECTION("describe without a map is a usage error")
    {
      checkErrorEnvelope(runCli({"describe"}), "usage", 2);
    }

    SECTION("describe with two maps is a usage error")
    {
      checkErrorEnvelope(runCli({"describe", "a.map", "b.map"}), "usage", 2);
    }

    SECTION("show without a node path is a usage error")
    {
      checkErrorEnvelope(runCli({"show", "x.map"}), "usage", 2);
    }

    SECTION("fgd without a classname is a usage error")
    {
      checkErrorEnvelope(runCli({"fgd", "x.map"}), "usage", 2);
    }

    SECTION("a malformed count is a usage error")
    {
      const auto count = GENERATE("many", "-1", "+1", "1.5", "");
      CAPTURE(count);
      checkErrorEnvelope(runCli({"query", "x.map", "--limit", count}), "usage", 2);
      checkErrorEnvelope(runCli({"query", "x.map", "--offset", count}), "usage", 2);
    }

    SECTION("a malformed box is a usage error")
    {
      const auto box = GENERATE(
        "1,2,3", "1,2,3,4,5,6,7", "0,0,0,1,1,x", "0,0,0,1,1,1e", "10,0,0,0,10,10");
      CAPTURE(box);
      checkErrorEnvelope(runCli({"query", "x.map", "--within", box}), "usage", 2);
      checkErrorEnvelope(runCli({"query", "x.map", "--intersects", box}), "usage", 2);
    }

    SECTION("options of other commands are refused")
    {
      checkErrorEnvelope(runCli({"query", "x.map", "--raw"}), "usage", 2);
      checkErrorEnvelope(runCli({"describe", "x.map", "--limit", "1"}), "usage", 2);
      checkErrorEnvelope(
        runCli({"show", "x.map", "world", "--key-entities", "light"}), "usage", 2);
      checkErrorEnvelope(runCli({"fgd", "x.map", "light", "--count-only"}), "usage", 2);
      checkErrorEnvelope(runCli({"describe", "x.map", "--fail-on", "error"}), "usage", 2);
      checkErrorEnvelope(
        runCli({"query", "x.map", "--min-severity", "error"}), "usage", 2);
    }

    SECTION("an option that several commands accept is accepted by each of them")
    {
      // accepted, so the command gets as far as loading the map
      checkErrorEnvelope(runCli({"query", "x.map", "--limit", "1"}), "file_not_found", 1);
      checkErrorEnvelope(
        runCli({"verify", "x.map", "--limit", "1"}), "file_not_found", 1);
    }

    SECTION("an op's --target is the op's flag, not verify's option")
    {
      checkErrorEnvelope(
        runCli({"delete", "x.map", "--target", "layer:0/brush:0"}), "file_not_found", 1);
    }

    SECTION("--help and -h print usage")
    {
      const auto option = GENERATE("--help", "-h");
      const auto result = runCli({option});
      CHECK(result.exitCode == 0);
      CHECK_THAT(result.out, ContainsSubstring("usage: tbcli"));
    }

    SECTION("--version prints the version")
    {
      const auto result = runCli({"--version"});
      CHECK(result.exitCode == 0);
      CHECK_THAT(result.out, StartsWith("tbcli "));
    }
  }

  SECTION("map loading")
  {
    env.writeConfig({}, quakeConfig());

    SECTION("missing map fails")
    {
      checkErrorEnvelope(
        runCli({"describe", arg(env.path("missing.map"))}), "file_not_found", 1);
    }

    SECTION("logger output goes to stderr")
    {
      const auto map = env.copyFixture("maps/simple.map", "simple.map");
      const auto result = runCli({"describe", arg(map)});

      CHECK(result.exitCode == 0);
      CHECK_THAT(result.err, ContainsSubstring("Loading document from"));
      CHECK_THAT(result.err, ContainsSubstring("Loaded entity definition file"));
      CHECK(parseOutput(result).isObject());
    }
  }

  SECTION("exit codes")
  {
    SECTION("a command can exit with its own code and still print its result")
    {
      env.writeConfig({}, quakeConfig());
      const auto map = env.copyFixture("maps/simple.map", "simple.map");

      // maps/simple.map has warnings
      const auto result = runCli({"verify", arg(map), "--fail-on", "warning"});
      CHECK(result.exitCode == 4);
      CHECK(parseOutput(result)["schema"] == Json{"tbcli.verify/1"});
    }
  }

  SECTION("configuration")
  {
    const auto map = env.copyFixture("maps/simple.map", "project/maps/sub/simple.map");

    SECTION("tbcli.json is found by walking up from the map")
    {
      env.writeConfig("project", quakeConfig());

      const auto result = runCli({"describe", arg(map)});
      CHECK(result.exitCode == 0);
      // the Quake FGD makes the player start 16 units wide on each side
      CHECK(maxY(result) == 96.0);
    }

    SECTION("without a game config the load fails")
    {
      checkErrorEnvelope(runCli({"describe", arg(map)}), "config_error", 1);
    }

    SECTION("relative paths are resolved against the directory of tbcli.json")
    {
      env.copyFixture("fgd/big_start.fgd", "project/defs/big_start.fgd");
      auto config = quakeConfig();
      config.set("fgd", "defs/big_start.fgd");
      env.writeConfig("project", config);

      const auto result = runCli({"describe", arg(map)});
      CHECK(result.exitCode == 0);
      CHECK(maxY(result) == 112.0);
    }

    SECTION("only the first of several FGDs is loaded")
    {
      auto config = quakeConfig();
      config.set(
        "fgd",
        Json{Json::Array{arg(fixturePath("fgd/big_start.fgd")), arg(quakeFgdPath())}});
      env.writeConfig("project", config);

      const auto result = runCli({"describe", arg(map)});
      CHECK(result.exitCode == 0);
      CHECK(maxY(result) == 112.0);
      CHECK_THAT(result.err, ContainsSubstring("Only the first FGD is loaded"));
    }

    SECTION("a missing FGD fails")
    {
      auto config = quakeConfig();
      config.set("fgd", "missing.fgd");
      env.writeConfig("project", config);

      checkErrorEnvelope(runCli({"describe", arg(map)}), "config_error", 1);
    }

    SECTION("a malformed tbcli.json fails with its line")
    {
      env.writeFile("project/tbcli.json", "{\n  \"format\": \"valve\",\n  oops\n}");

      const auto result = runCli({"describe", arg(map)});
      checkErrorEnvelope(result, "config_error", 1);
      CHECK(parseOutput(result)["error"]["line"] == Json{3});
    }

    SECTION("a non-Valve format is refused")
    {
      SECTION("in tbcli.json")
      {
        auto config = quakeConfig();
        config.set("format", "standard");
        env.writeConfig("project", config);

        checkErrorEnvelope(runCli({"describe", arg(map)}), "not_valve_220", 1);
      }

      SECTION("as a flag")
      {
        env.writeConfig("project", quakeConfig());

        checkErrorEnvelope(
          runCli({"describe", arg(map), "--map-format", "standard"}), "not_valve_220", 1);
      }
    }

    SECTION("a missing Quake directory is reported and ignored")
    {
      auto config = quakeConfig();
      config.set("quake_dir", "missing");
      env.writeConfig("project", config);

      const auto result = runCli({"describe", arg(map)});
      CHECK(result.exitCode == 0);
      CHECK_THAT(result.err, ContainsSubstring("Quake directory"));
      CHECK_THAT(result.err, ContainsSubstring("not found"));
    }

    SECTION("flags override tbcli.json")
    {
      auto config = quakeConfig();
      config.set("format", "standard");
      config.set("game_config", "missing.cfg");
      config.set("fgd", "missing.fgd");
      config.set("quake_dir", "missing");
      env.writeConfig("project", config);
      std::filesystem::create_directories(env.path("quake"));

      const auto result = runCli(
        {"describe",
         arg(map),
         "--map-format",
         "valve",
         "--game-config",
         arg(quakeGameConfigPath()),
         "--fgd=" + arg(fixturePath("fgd/big_start.fgd")),
         "--quake-dir",
         arg(env.path("quake"))});
      CHECK(result.exitCode == 0);
      CHECK(maxY(result) == 112.0);
    }

    SECTION("--key-entities overrides describe.key_entities")
    {
      auto describe = Json::object();
      describe.set("key_entities", Json{Json::Array{"func_door"}});
      auto config = quakeConfig();
      config.set("describe", describe);
      env.writeConfig("project", config);

      // empty elements are ignored
      const auto result = runCli({"describe", arg(map), "--key-entities", ",light,"});
      CHECK(result.exitCode == 0);

      const auto output = parseOutput(result);
      const auto& items = output["key_entities"]["items"];
      CHECK(items.size() == 3);
      CHECK(items[1]["classname"] == Json{"light"});
      // listed because it has a targetname, not because of its class
      CHECK(items[2]["classname"] == Json{"func_door"});
    }

    SECTION("flags work without tbcli.json")
    {
      const auto result = runCli(
        {"describe",
         arg(map),
         "--game-config",
         arg(quakeGameConfigPath()),
         "--fgd",
         arg(quakeFgdPath())});
      CHECK(result.exitCode == 0);
      CHECK(maxY(result) == 96.0);
    }
  }
}

TEST_CASE("readsStdin")
{
  CHECK(readsStdin({"apply", "x.map", "-"}));
  CHECK(readsStdin({"apply", "--dry-run", "x.map", "-", "-o", "out.map"}));
  CHECK(!readsStdin({"apply", "x.map", "batch.json"}));
  CHECK(!readsStdin({"apply", "x.map", "batch.json", "-o", "-"}));
  CHECK(!readsStdin({"create_box", "x.map", "--material", "-"}));
  CHECK(!readsStdin({"apply", "x.map", "-", "--help"}));
  CHECK(!readsStdin({"apply", "x.map", "-", "--frobnicate"}));
  CHECK(!readsStdin({}));
}

} // namespace tb::cli
