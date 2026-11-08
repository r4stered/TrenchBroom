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
#include "base/Logger.h"
#include "cli/LoadMap.h"
#include "cli/MapRef.h"
#include "cli/Show.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::cli
{
using namespace Catch::Matchers;

TEST_CASE("show")
{
  auto env = CliTestEnvironment{};
  env.writeConfig({}, quakeConfig());
  const auto map = arg(env.copyFixture("maps/query.map", "query.map"));

  const auto showNode = [&](const std::string& path) {
    const auto result = runCli({"show", map, path});
    REQUIRE(result.exitCode == 0);
    const auto json = parseOutput(result);
    CHECK(json["schema"] == Json{"tbcli.show/1"});
    CHECK(json["rev"].isString());
    return json["node"];
  };

  SECTION("a brush in full")
  {
    const auto brush = showNode("layer:0/brush:0");
    CHECK(brush["path"] == Json{"layer:0/brush:0"});
    CHECK(
      brush["aabb"] == parseJsonLiteral(R"({"min": [0, 0, 0], "max": [64, 64, 16]})"));
    CHECK(brush["axial_box"] == Json{true});
    CHECK(brush["entity"] == Json{"world"});
    CHECK(brush["vertices"].size() == 8);
    REQUIRE(brush["faces"].size() == 6);

    // the faces are in file order
    CHECK(brush["faces"][3] == parseJsonLiteral(R"({
      "path": "layer:0/brush:0/face:3",
      "normal": [0, 0, 1],
      "texture": "floor_top",
      "uv": {
        "u_axis": [1, 0, 0], "v_axis": [0, -1, 0],
        "offset": [8, 4], "scale": [0.5, 2], "rotation": 15
      },
      "vertices": [[64, 64, 16], [0, 64, 16], [0, 0, 16], [64, 0, 16]]
    })"));
    CHECK(brush["faces"][0]["normal"] == parseJsonLiteral("[-1, 0, 0]"));
    CHECK(brush["faces"][0].find("points") == nullptr);
  }

  SECTION("--raw adds the plane points")
  {
    const auto result = runCli({"show", map, "layer:0/brush:0", "--raw"});
    REQUIRE(result.exitCode == 0);
    CHECK(
      parseOutput(result)["node"]["faces"][3]["points"]
      == parseJsonLiteral("[[64, 64, 16], [64, 65, 16], [65, 64, 16]]"));
  }

  SECTION("a face")
  {
    const auto face = showNode("layer:0/brush:3/face:5");
    CHECK(face["path"] == Json{"layer:0/brush:3/face:5"});
    CHECK(face["texture"] == Json{"ramp"});
    // the slope's normal, (2, 0, 1) normalized
    CHECK(face["normal"] == parseJsonLiteral("[0.894, 0, 0.447]"));
  }

  SECTION("a brush entity lists its brushes")
  {
    const auto door = showNode("layer:0/entity:1");
    CHECK(door["classname"] == Json{"func_door"});
    CHECK(door["properties"] == parseJsonLiteral(R"({
      "classname": "func_door", "targetname": "door"
    })"));
    CHECK(
      door["aabb"] == parseJsonLiteral(R"({"min": [300, 0, 0], "max": [332, 32, 64]})"));
    CHECK(door["brushes"] == parseJsonLiteral(R"({
      "items": ["layer:0/entity:1/brush:0"], "total": 1, "truncated": false
    })"));
  }

  SECTION("the world is worldspawn and lists its layers")
  {
    const auto world = showNode("world");
    CHECK(world["path"] == Json{"world"});
    CHECK(world["classname"] == Json{"worldspawn"});
    CHECK(world["layers"] == parseJsonLiteral(R"({
      "items": ["layer:0", "layer:1"], "total": 2, "truncated": false
    })"));
  }

  SECTION("a layer lists its children")
  {
    CHECK(showNode("layer:1") == parseJsonLiteral(R"({
      "path": "layer:1", "id": 1, "name": "Upper",
      "bounds": {"min": [0, 0, 256], "max": [64, 64, 332]},
      "counts": {"brushes": 1, "entities": 1, "faces": 6},
      "hidden": false, "locked": false, "omit_from_export": false,
      "children": {"items": ["layer:1/group:0"], "total": 1, "truncated": false}
    })"));
  }

  SECTION("layers and groups can be addressed by id")
  {
    CHECK(showNode("layer:id:1")["path"] == Json{"layer:1"});
    CHECK(showNode("layer:id:1/group:id:2")["path"] == Json{"layer:1/group:0"});
    CHECK(showNode("group:id:2")["path"] == Json{"layer:1/group:0"});
    CHECK(showNode("group:id:2/entity:0")["classname"] == Json{"info_player_start"});
    CHECK(
      showNode("group:id:2")["children"]["items"]
      == parseJsonLiteral(R"(["layer:1/group:0/brush:0", "layer:1/group:0/entity:0"])"));
  }

  SECTION("a linked group lists its siblings")
  {
    env.copyFixture("maps/describe.map", "describe.map");
    const auto result =
      runCli({"show", arg(env.path("describe.map")), "layer:0/group:0"});
    REQUIRE(result.exitCode == 0);
    const auto group = parseOutput(result)["node"];
    CHECK(group["link_id"] == Json{"pillar-link"});
    CHECK(group["siblings"] == parseJsonLiteral(R"(["layer:0/group:1"])"));
  }

  SECTION("a malformed path is invalid_path")
  {
    const auto path = GENERATE(
      std::string{""},
      std::string{"layer"},
      std::string{"layer:x"},
      std::string{"layer:-1"},
      std::string{"layer:0/"},
      std::string{"brush:0"},
      std::string{"world/layer:0"},
      std::string{"layer:0/face:0"},
      std::string{"layer:0/layer:0"},
      std::string{"layer:0/brush:0/face:0/face:1"},
      std::string{"layer:0/entity:id:1"},
      std::string{"thing:0"});

    CAPTURE(path);
    const auto result = runCli({"show", map, path});
    CHECK(result.exitCode == 1);
    const auto error = parseOutput(result)["error"];
    CHECK(error["code"] == Json{"invalid_path"});
    CHECK(error["node"] == Json{path});
  }

  SECTION("a path to nothing is node_not_found")
  {
    const auto path = GENERATE(
      std::string{"layer:2"},
      std::string{"layer:0/brush:4"},
      std::string{"layer:0/entity:1/brush:1"},
      std::string{"layer:0/brush:0/face:6"},
      std::string{"layer:id:99"},
      // 1 is the id of a layer, not of a group
      std::string{"group:id:1"},
      std::string{"layer:0/group:id:2"});

    CAPTURE(path);
    const auto result = runCli({"show", map, path});
    CHECK(result.exitCode == 1);
    const auto error = parseOutput(result)["error"];
    CHECK(error["code"] == Json{"node_not_found"});
    CHECK(error["node"] == Json{path});
  }

  SECTION("--format text prints an outline")
  {
    const auto result = runCli({"show", map, "layer:0/entity:1", "--format", "text"});
    REQUIRE(result.exitCode == 0);
    CHECK_THAT(result.out, ContainsSubstring("schema: tbcli.show/1"));
    CHECK_THAT(result.out, ContainsSubstring("classname: func_door"));
  }

  SECTION("a borrowed map is shown at the rev it is lent at")
  {
    auto logger = NullLogger{};
    const auto loadedMap = loadFixtureMap("maps/query.map", logger);
    const auto noLoadProblems = std::vector<LoadProblem>{};
    const auto mapRef = MapRef{*loadedMap.map, "borrowed", noLoadProblems};

    const auto json = show(mapRef, "layer:0/entity:1", false);
    CHECK(json["rev"] == Json{"borrowed"});
    CHECK(json["node"]["classname"] == Json{"func_door"});
  }
}

} // namespace tb::cli
