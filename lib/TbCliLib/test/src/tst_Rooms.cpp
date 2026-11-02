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
#include "cli/Compile.h"
#include "cli/Rooms.h"

#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::cli
{
using namespace Catch::Matchers;

namespace
{

/**
 * A map of fixture/compiled copied into a temp dir, with a tbcli.json whose qbsp is the
 * fake compiler replaying the map's recording.
 */
struct RoomsFixture
{
  CliTestEnvironment env;
  std::string map;

  explicit RoomsFixture(const std::string& recording = "rooms_prt1")
  {
    auto compilers = Json::object();
    compilers.set(
      std::string{QbspTool}, arg(setUpFakeCompiler(env, QbspTool, recording)));
    auto config = quakeConfig();
    config.set("compilers", std::move(compilers));
    env.writeConfig({}, config);

    const auto fixture = std::filesystem::path{"compiled"} / (recording + ".map");
    map = arg(env.copyFixture(fixture, fixture.filename()));
  }

  RunResult run(const std::vector<std::string>& options = {}) const
  {
    auto args = std::vector<std::string>{"rooms", map};
    args.insert(args.end(), options.begin(), options.end());
    return runCli(args);
  }

  Json json(const std::vector<std::string>& options = {}) const
  {
    const auto result = run(options);
    INFO(result.out);
    INFO(result.err);
    REQUIRE(result.exitCode == 0);
    return parseOutput(result);
  }

  /** Runs `verify --compile --artifacts <dir>` and returns the dir. */
  std::string keepArtifacts() const
  {
    const auto artifacts = arg(env.path("artifacts"));
    const auto result = runCli({"verify", map, "--compile", "--artifacts", artifacts});
    REQUIRE(result.exitCode == 0);
    return artifacts;
  }
};

/** The ids of the rooms or cells of a `rooms` result. */
std::vector<size_t> idsOf(const Json& list)
{
  auto result = std::vector<size_t>{};
  for (const auto& item : list["items"].asArray())
  {
    result.push_back(size_t(item["id"].asNumber()));
  }
  return result;
}

} // namespace

TEST_CASE("rooms")
{
  SECTION("runs qbsp only, with -nodetail -forceprt1, on the map's bytes")
  {
    const auto fixture = RoomsFixture{};
    const auto json = fixture.json();

    CHECK(
      fixture.env.readFile("tools/qbsp.args")
      == "-nodetail\n-forceprt1\nrooms_prt1.map\n");
    CHECK(
      fixture.env.readFile("tools/qbsp.input") == fixture.env.readFile("rooms_prt1.map"));
    // the temporary directory is deleted
    CHECK_FALSE(std::filesystem::exists(fixture.env.readFile("tools/qbsp.cwd")));

    CHECK(json["schema"] == Json{"tbcli.rooms/1"});
    CHECK(json["rev"] == Json{revOf(fixture.map)});
    CHECK(json["heuristic"] == Json{true});
    CHECK(
      json["method"]
      == parseJsonLiteral(R"({"name": "constriction", "split_ratio": 0.5})"));
  }

  SECTION("cuts two rooms at the doorway between them")
  {
    // room B, east of the doorway, has the lowest leaf; the doorway's own leaf joins it
    CHECK(RoomsFixture{}.json()["rooms"] == parseJsonLiteral(R"({
        "items": [
          {
            "id": 0,
            "bounds": {"min": [0, -128, 0], "max": [272, 128, 128]},
            "volume": 8486912,
            "leaves": 4,
            "contents": "empty",
            "entities": ["layer:0/entity:1"],
            "key_entities": [],
            "openings": []
          },
          {
            "id": 1,
            "bounds": {"min": [-256, -128, 0], "max": [0, 128, 128]},
            "volume": 8388608,
            "leaves": 3,
            "contents": "empty",
            "entities": ["layer:0/entity:0"],
            "key_entities": ["layer:0/entity:0"],
            "openings": []
          }
        ],
        "total": 2,
        "truncated": false
      })"));
  }

  SECTION("cuts at a liquid's surface, but not around a pillar")
  {
    const auto rooms = RoomsFixture{"rooms_prt2_water"}.json()["rooms"];
    REQUIRE(rooms["total"] == Json{3});

    const auto& roomB = rooms["items"][0];
    CHECK(roomB["contents"] == Json{"empty"});
    CHECK(roomB["leaves"] == Json{6});
    CHECK(
      roomB["bounds"]
      == parseJsonLiteral(R"({"min": [0, -128, 0], "max": [272, 128, 128]})"));
    CHECK(roomB["entities"] == parseJsonLiteral(R"(["layer:0/entity:2"])"));

    const auto& water = rooms["items"][1];
    CHECK(water["contents"] == Json{"water"});
    CHECK(water["leaves"] == Json{1});
    CHECK(
      water["bounds"]
      == parseJsonLiteral(R"({"min": [80, -96, 0], "max": [208, 96, 48]})"));
    CHECK(water["entities"] == Json::array());

    // the func_detail pillar is structural for rooms, which splits room A into four
    // leafs that merge again
    const auto& roomA = rooms["items"][2];
    CHECK(roomA["leaves"] == Json{4});
    CHECK(
      roomA["bounds"]
      == parseJsonLiteral(R"({"min": [-256, -128, 0], "max": [0, 128, 128]})"));
    CHECK(roomA["entities"] == parseJsonLiteral(R"(["layer:0/entity:1"])"));
    CHECK(roomA["key_entities"] == parseJsonLiteral(R"(["layer:0/entity:1"])"));
  }

  SECTION("leaves out sky leafs")
  {
    const auto fixture = RoomsFixture{"rooms_sky"};
    CHECK(fixture.json()["rooms"]["total"] == Json{2});

    // vis leaf 0 is the sky above both rooms
    const auto json = fixture.json({"--cells"});
    CHECK(json["cells"]["total"] == Json{7});
    CHECK(json["cells"]["items"][0]["id"] == Json{1});
    for (const auto& portal : json["portals"].asArray())
    {
      CHECK(portal["cells"][0] != Json{0});
      CHECK(portal["cells"][1] != Json{0});
    }
  }

  SECTION("joins a space too small for a player to the room it opens into")
  {
    // an 8 unit wide slot, 80 units deep, in room A's west wall
    const auto json = RoomsFixture{"rooms_slot"}.json();
    CHECK(json["rooms"]["total"] == Json{2});
    const auto& roomA = json["rooms"]["items"][1];
    CHECK(roomA["leaves"] == Json{4});
    CHECK(
      roomA["bounds"]
      == parseJsonLiteral(R"({"min": [-336, -128, 0], "max": [0, 128, 128]})"));
  }

  SECTION("--split-ratio")
  {
    const auto fixture = RoomsFixture{"rooms_prt2_water"};

    // no constriction is narrow enough to cut, but the liquid's surface still is
    const auto json = fixture.json({"--split-ratio", "0"});
    CHECK(json["method"]["split_ratio"] == Json{0});
    CHECK(json["rooms"]["total"] == Json{2});

    CHECK(fixture.json({"--split-ratio=0.5"})["rooms"]["total"] == Json{3});

    // every leaf is narrower than the rooms it lies in
    CHECK(fixture.json({"--split-ratio", "1"})["rooms"]["total"].asNumber() > 3);

    checkError(fixture.run({"--split-ratio", "1.5"}), "usage", 2);
    checkError(fixture.run({"--split-ratio", "x"}), "usage", 2);
  }

  SECTION("--cells")
  {
    SECTION("lists the leafs with their rooms, and the portals with their cuts")
    {
      const auto json = RoomsFixture{}.json({"--cells"});
      CHECK(json.find("rooms") == nullptr);

      const auto& cells = json["cells"];
      CHECK(cells["total"] == Json{7});
      CHECK(cells["items"][3] == parseJsonLiteral(R"({
          "id": 3,
          "bounds": {"min": [0, -32, 0], "max": [16, 32, 96]},
          "volume": 98304,
          "contents": "empty",
          "room": 0,
          "entities": []
        })"));
      CHECK(cells["items"][4]["entities"] == parseJsonLiteral(R"(["layer:0/entity:0"])"));

      const auto& portals = json["portals"].asArray();
      REQUIRE(portals.size() == 6);
      auto cuts = std::vector<Json>{};
      for (const auto& portal : portals)
      {
        if (!portal["cut"].isNull())
        {
          cuts.push_back(portal);
        }
      }
      // the west side of the doorway, between its leaf and room A
      CHECK(cuts == std::vector<Json>{parseJsonLiteral(R"({
          "cells": [3, 4],
          "center": [0, 0, 48],
          "normal": [1, 0, 0],
          "area": 6144,
          "cut": "constriction"
        })")});
    }

    SECTION("marks where the contents change as a liquid surface")
    {
      const auto json = RoomsFixture{"rooms_prt2_water"}.json({"--cells", "--room", "1"});
      CHECK(idsOf(json["cells"]) == std::vector<size_t>{3});
      // every portal of the water leaf
      CHECK(json["portals"].size() == 5);
      for (const auto& portal : json["portals"].asArray())
      {
        CHECK(portal["cut"] == Json{"liquid_surface"});
      }
    }
  }

  SECTION("paging and filters")
  {
    const auto fixture = RoomsFixture{"rooms_prt2_water"};

    auto rooms = fixture.json({"--limit", "1"})["rooms"];
    CHECK(idsOf(rooms) == std::vector<size_t>{0});
    CHECK(rooms["total"] == Json{3});
    CHECK(rooms["truncated"] == Json{true});

    rooms = fixture.json({"--offset", "1"})["rooms"];
    CHECK(idsOf(rooms) == std::vector<size_t>{1, 2});
    CHECK(rooms["truncated"] == Json{false});

    rooms = fixture.json({"--room", "2"})["rooms"];
    CHECK(idsOf(rooms) == std::vector<size_t>{2});
    CHECK(rooms["total"] == Json{1});
    CHECK(fixture.json({"--room", "7"})["rooms"]["total"] == Json{0});

    // room A, within 0.01 of the box, and the water
    rooms = fixture.json({"--within", "-256.005,-128,0,0,128,128"})["rooms"];
    CHECK(idsOf(rooms) == std::vector<size_t>{2});
    rooms = fixture.json({"--within", "64,-128,0,256,128,64"})["rooms"];
    CHECK(idsOf(rooms) == std::vector<size_t>{1});

    const auto cells = fixture.json({"--cells", "--room", "2", "--limit", "2"})["cells"];
    CHECK(idsOf(cells) == std::vector<size_t>{7, 8});
    CHECK(cells["total"] == Json{4});
    CHECK(cells["truncated"] == Json{true});
  }

  SECTION("a leaking map fails with the leak issue that verify reports")
  {
    const auto fixture = RoomsFixture{"compile_leak"};
    const auto result = fixture.run();
    checkError(result, "map_leaks", 1);
    // the leak check runs qbsp as verify does, since -nodetail would seal more
    CHECK(fixture.env.readFile("tools/qbsp.args") == "compile_leak.map\n");

    const auto verify = runJson({"verify", fixture.map, "--compile", "--type", "leak"});
    REQUIRE(verify["issues"]["total"] == Json{1});
    CHECK(parseOutput(result)["error"]["issue"] == verify["issues"]["items"][0]);
  }

  SECTION("a failed qbsp fails with compile_failed")
  {
    const auto result = RoomsFixture{"compile_fatal"}.run();
    checkError(result, "compile_failed", 1);
    CHECK_THAT(
      parseOutput(result)["error"]["message"].asString(),
      StartsWith("qbsp exited with code 1: "));
  }

  SECTION("a missing qbsp fails with compiler_missing")
  {
    auto env = CliTestEnvironment{};
    env.writeConfig({}, quakeConfig());
    const auto map = arg(env.copyFixture("compiled/rooms_prt1.map", "rooms_prt1.map"));
    checkError(runCli({"rooms", map}), "compiler_missing", 1);
  }

  SECTION("--from-artifacts")
  {
    auto fixture = RoomsFixture{};
    const auto artifacts = fixture.keepArtifacts();
    std::filesystem::remove(fixture.env.path("tools/qbsp.args"));

    SECTION("reuses the artifacts of verify --compile without running qbsp")
    {
      CHECK(fixture.json({"--from-artifacts", artifacts}) == fixture.json());
      std::filesystem::remove(fixture.env.path("tools/qbsp.args"));
      fixture.json({"--from-artifacts", artifacts});
      CHECK_FALSE(std::filesystem::exists(fixture.env.path("tools/qbsp.args")));
    }

    SECTION("refuses missing files")
    {
      const auto file = GENERATE(
        std::string{"rooms_prt1.bsp"},
        std::string{"rooms_prt1.prt"},
        std::string{"rooms_prt1.bsp.tbcli.json"});
      std::filesystem::remove(fixture.env.path("artifacts") / file);
      checkError(fixture.run({"--from-artifacts", artifacts}), "stale_artifact", 1);
    }

    SECTION("refuses the artifacts of another rev")
    {
      fixture.env.writeFile(
        "rooms_prt1.map", fixture.env.readFile("rooms_prt1.map") + "\n");
      checkError(fixture.run({"--from-artifacts", artifacts}), "stale_artifact", 1);
    }

    SECTION("refuses a portal file of clusters")
    {
      fixture.env.writeFile(
        "artifacts/rooms_prt1.prt",
        "PRT2\n7\n2\n1\n4 0 1 (0 32 0) (0 -32 0) (0 -32 96) (0 32 96)\n0 1 2 -1\n3 4 5 6 "
        "-1\n");
      const auto result = fixture.run({"--from-artifacts", artifacts});
      checkError(result, "stale_artifact", 1);
      CHECK_THAT(
        parseOutput(result)["error"]["message"].asString(),
        ContainsSubstring("clusters"));
    }

    SECTION("--timeout is a usage error")
    {
      checkError(
        fixture.run({"--from-artifacts", artifacts, "--timeout", "1"}), "usage", 2);
    }
  }

  SECTION("refuses the clustered portal file verify keeps for a map with liquids")
  {
    const auto fixture = RoomsFixture{"rooms_prt2_water_keepprt"};
    const auto result = fixture.run({"--from-artifacts", fixture.keepArtifacts()});
    checkError(result, "stale_artifact", 1);
    CHECK_THAT(
      parseOutput(result)["error"]["message"].asString(),
      ContainsSubstring("run rooms without --from-artifacts"));
  }

  SECTION("refuses the artifacts of a leaking compile")
  {
    const auto fixture = RoomsFixture{"compile_leak"};
    const auto artifacts = arg(fixture.env.path("artifacts"));
    runCli({"verify", fixture.map, "--compile", "--artifacts", artifacts});
    checkError(fixture.run({"--from-artifacts", artifacts}), "stale_artifact", 1);
  }

  SECTION("other commands do not accept the rooms options")
  {
    const auto fixture = RoomsFixture{};
    for (const auto& option : std::vector<std::vector<std::string>>{
           {"--cells"},
           {"--room", "0"},
           {"--split-ratio", "0.5"},
           {"--from-artifacts", "x"}})
    {
      auto args = std::vector<std::string>{"verify", fixture.map};
      args.insert(args.end(), option.begin(), option.end());
      checkError(runCli(args), "usage", 2);
    }
  }
}

} // namespace tb::cli
