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
#include "cli/BuiltRooms.h"
#include "cli/Compile.h"
#include "cli/FileUtils.h"

#include "kd/string_utils.h"

#include <fmt/format.h>

#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::cli
{
using namespace Catch::Matchers;

namespace
{

constexpr auto EmptyMap = std::string_view{R"(// Game: Quake
// Format: Valve
// entity 0
{
"mapversion" "220"
"classname" "worldspawn"
"wad" ""
}
)"};

/** An empty map in a temp dir with a tbcli.json. */
struct RoomFixture
{
  CliTestEnvironment env;
  std::string map;
  std::string original;

  RoomFixture()
  {
    env.writeConfig({}, quakeConfig());
    map = arg(env.writeFile("rooms.map", EmptyMap));
    original = env.readFile("rooms.map");
  }

  RunResult apply(const std::string_view batch) const
  {
    return runCli({"apply", map, "-"}, batch);
  }

  Json applyJson(const std::string_view batch) const
  {
    return runJson({"apply", map, "-"}, batch);
  }

  bool unchanged() const { return env.readFile("rooms.map") == original; }

  Json builtRooms() const { return runJson({"describe", map})["built_rooms"]; }
};

/** The aabb of the node at the given path. */
Json aabbOf(const std::string& map, const std::string& path)
{
  return showNode(map, path)["aabb"];
}

/** The aabbs of the brushes of the entity at the given path. */
std::vector<Json> brushAabbs(const std::string& map, const std::string& entityPath)
{
  auto result = std::vector<Json>{};
  const auto entity = showNode(map, entityPath);
  for (const auto& brush : entity["brushes"]["items"].asArray())
  {
    result.push_back(aabbOf(map, brush.asString()));
  }
  return result;
}

/** The map text without the lines that differ between saves of the same map. */
std::string withoutVolatileLines(const std::string& text)
{
  auto lines = std::vector<std::string>{};
  for (const auto& line : kdl::str_split(text, "\n"))
  {
    if (
      !line.starts_with("// Generator:") && !line.starts_with("\"_tb_linked_group_id\""))
    {
      lines.push_back(line);
    }
  }
  return kdl::str_join(lines, "\n");
}

constexpr auto RoomA = std::string_view{
  R"({"op": "add_room", "name": "A", "min": [0, 0, 0], "size": [256, 256, 128]})"};

} // namespace

TEST_CASE("BuiltRooms")
{
  SECTION("relationNames")
  {
    CHECK(
      std::vector<std::string_view>(relationNames().begin(), relationNames().end())
      == std::vector<std::string_view>{"east_of", "west_of", "north_of", "south_of"});
  }

  SECTION("isPlacement")
  {
    CHECK(isPlacement(parseJsonLiteral(R"({"east_of": "A", "gap": 64})")));
    CHECK(isPlacement(parseJsonLiteral(R"({"gap": -8.5, "south_of": "B"})")));
    CHECK_FALSE(isPlacement(parseJsonLiteral(R"({"east_of": "A"})")));
    CHECK_FALSE(isPlacement(parseJsonLiteral(R"({"east_of": "A", "gap": "64"})")));
    CHECK_FALSE(isPlacement(parseJsonLiteral(R"({"east_of": 1, "gap": 64})")));
    CHECK_FALSE(isPlacement(parseJsonLiteral(R"({"above": "A", "gap": 64})")));
    CHECK_FALSE(
      isPlacement(parseJsonLiteral(R"({"east_of": "A", "west_of": "B", "gap": 64})")));
    CHECK_FALSE(isPlacement(parseJsonLiteral(R"(["east_of", "A", 64])")));
  }
}

TEST_CASE("add_room")
{
  auto fixture = RoomFixture{};
  const auto& map = fixture.map;

  SECTION("creates a sealed room of ordinary brushes in a carrier in a group")
  {
    const auto json = fixture.applyJson(fmt::format("[{}]", RoomA));
    CHECK(json["ops"][0]["created"] == parseJsonLiteral(R"(["layer:0/group:0"])"));

    const auto group = showNode(map, "layer:0/group:0");
    CHECK(group["name"] == Json{"built_room:A"});
    CHECK(
      group["children"]["items"] == parseJsonLiteral(R"(["layer:0/group:0/entity:0"])"));

    const auto carrier = showNode(map, "layer:0/group:0/entity:0");
    CHECK(carrier["classname"] == Json{"func_group"});
    CHECK(carrier["properties"]["_tbcli_kind"] == Json{"room"});
    CHECK(carrier["properties"]["_tbcli_name"] == Json{"A"});
    CHECK(carrier["properties"]["_tbcli_size"] == Json{"256 256 128"});
    CHECK(carrier["properties"]["_tbcli_wall"] == Json{"16"});
    CHECK(carrier["properties"]["_tbcli_fp"].asString().size() == 16);
    // an absolutely placed room has no placement, and no position is stored
    CHECK(carrier["properties"].find("_tbcli_place") == nullptr);
    CHECK(carrier["properties"].find("_tb_type") == nullptr);

    // floor, ceiling, west, east, south and north, as in the reference lowering
    CHECK(
      brushAabbs(map, "layer:0/group:0/entity:0")
      == std::vector<Json>{
        parseJsonLiteral(R"({"min": [-16, -16, -16], "max": [272, 272, 0]})"),
        parseJsonLiteral(R"({"min": [-16, -16, 128], "max": [272, 272, 144]})"),
        parseJsonLiteral(R"({"min": [-16, -16, 0], "max": [0, 272, 128]})"),
        parseJsonLiteral(R"({"min": [256, -16, 0], "max": [272, 272, 128]})"),
        parseJsonLiteral(R"({"min": [0, -16, 0], "max": [256, 0, 128]})"),
        parseJsonLiteral(R"({"min": [0, 256, 0], "max": [256, 272, 128]})"),
      });
    CHECK(showNode(map, "layer:0/group:0/entity:0/brush:0")["axial_box"] == Json{true});
  }

  SECTION("the wall thickness can be given")
  {
    fixture.applyJson(R"([{"op": "add_room", "name": "A", "min": [0, 0, 0],
      "size": [64, 64, 64], "wall": 8}])");
    CHECK(
      aabbOf(map, "layer:0/group:0/entity:0")
      == parseJsonLiteral(R"({"min": [-8, -8, -8], "max": [72, 72, 72]})"));
    CHECK(
      showNode(map, "layer:0/group:0/entity:0")["properties"]["_tbcli_wall"]
      == Json{"8"});
  }

  SECTION("places a room next to another, centred on the other axis, on the same floor")
  {
    using T = std::tuple<std::string, std::string>;
    const auto [relation, expected] = GENERATE(
      T{"east_of", R"({"min": [384, 64, 0], "max": [512, 192, 64]})"},
      T{"west_of", R"({"min": [-256, 64, 0], "max": [-128, 192, 64]})"},
      T{"north_of", R"({"min": [64, 384, 0], "max": [192, 512, 64]})"},
      T{"south_of", R"({"min": [64, -256, 0], "max": [192, -128, 64]})"});
    CAPTURE(relation);

    const auto json = fixture.applyJson(
      fmt::format(
        R"([{}, {{"op": "add_room", "name": "B", "size": [128, 128, 64],
          "place": {{"{}": "A", "gap": 128}}}}])",
        RoomA,
        relation));
    CHECK(json["ops"][1]["created"] == parseJsonLiteral(R"(["layer:0/group:1"])"));

    // the interior is the carrier's bounds less the walls
    const auto interior = parseJsonLiteral(expected);
    auto shell = Json::object();
    for (const auto* corner : {"min", "max"})
    {
      const auto sign = std::string_view{corner} == "min" ? -16 : 16;
      auto point = Json::array();
      for (const auto& value : interior[corner].asArray())
      {
        point.push(value.asNumber() + sign);
      }
      shell.set(corner, std::move(point));
    }
    CHECK(aabbOf(map, "layer:0/group:1/entity:0") == shell);
    CHECK(
      showNode(map, "layer:0/group:1/entity:0")["properties"]["_tbcli_place"]
      == Json{relation + " A 128"});
  }

  SECTION("centring rounds half a unit up")
  {
    fixture.applyJson(
      fmt::format(
        R"([{}, {{"op": "add_room", "name": "B", "size": [64, 101, 64],
          "place": {{"east_of": "A", "gap": 64}}}}])",
        RoomA));
    // (256 - 101) / 2 = 77.5
    CHECK(
      aabbOf(map, "layer:0/group:1/entity:0")
      == parseJsonLiteral(R"({"min": [304, 62, -16], "max": [400, 195, 80]})"));
  }

  SECTION("places relative to where the other room's brushes are now")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(R"([
      {"op": "translate", "target": "layer:0/group:0", "delta": [1000, 0, 32]},
      {"op": "add_room", "name": "B", "size": [256, 256, 128],
       "place": {"east_of": "A", "gap": 64}}])");
    CHECK(
      aabbOf(map, "layer:0/group:1/entity:0")
      == parseJsonLiteral(R"({"min": [1304, -16, 16], "max": [1592, 272, 176]})"));
  }

  SECTION("as binds the room's group; the subcommand takes place as JSON")
  {
    const auto json = fixture.applyJson(
      R"([{"op": "add_room", "name": "A", "min": [0, 0, 0], "size": [64, 64, 64],
          "as": "$a"}])");
    CHECK(json["bindings"] == parseJsonLiteral(R"({"$a": "layer:0/group:0"})"));

    const auto flags = runJson(
      {"add_room",
       map,
       "--name",
       "B",
       "--size",
       "64,64,64",
       "--place",
       R"({"north_of": "A", "gap": 96})"});
    CHECK(flags["ops"][0]["created"] == parseJsonLiteral(R"(["layer:0/group:1"])"));
    CHECK(
      aabbOf(map, "layer:0/group:1/entity:0")
      == parseJsonLiteral(R"({"min": [-16, 144, -16], "max": [80, 240, 80]})"));
  }

  SECTION("refuses rooms whose walls would overlap with layout_overlap")
  {
    const auto second = GENERATE(
      // walls 16 thick need a gap of at least 32
      R"({"op": "add_room", "name": "B", "size": [64, 64, 64],
          "place": {"east_of": "A", "gap": 31}})",
      R"({"op": "add_room", "name": "B", "min": [100, 100, 100], "size": [64, 64, 64]})");
    CAPTURE(second);

    const auto result = fixture.apply(fmt::format("[{}, {}]", RoomA, second));
    checkError(result, "layout_overlap", 1, 1);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/group:0/entity:0"});
    CHECK(fixture.unchanged());
  }

  SECTION("walls that only touch do not overlap")
  {
    fixture.applyJson(
      fmt::format(
        R"([{}, {{"op": "add_room", "name": "B", "size": [64, 64, 64],
          "place": {{"east_of": "A", "gap": 32}}}}])",
        RoomA));
    CHECK(fixture.builtRooms()["total"] == Json{2});
  }

  SECTION("refuses to place against a room that does not exist")
  {
    const auto result = fixture.apply(
      R"([{"op": "add_room", "name": "B", "size": [64, 64, 64],
          "place": {"east_of": "A", "gap": 64}}])");
    checkError(result, "node_not_found", 1, 0);
    CHECK(fixture.unchanged());
  }

  SECTION("refuses to place against a name that several rooms have")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(
      R"([{"op": "duplicate", "target": "layer:0/group:0", "offset": [0, 1000, 0]}])");
    const auto before = fixture.env.readFile("rooms.map");
    const auto result = fixture.apply(R"([{"op": "add_room", "name": "B",
      "size": [64, 64, 64], "place": {"east_of": "A", "gap": 64}}])");
    checkError(result, "op_failed", 1, 0);
    CHECK_THAT(
      parseOutput(result)["error"]["message"].asString(),
      ContainsSubstring("layer:0/group:0/entity:0, layer:0/group:1/entity:0"));
    CHECK(fixture.env.readFile("rooms.map") == before);
  }

  SECTION("a carrier without brushes cannot be placed against and overlaps nothing")
  {
    fixture.env.writeFile("rooms.map", std::string{EmptyMap} + R"(// entity 1
{
"classname" "func_group"
"_tbcli_kind" "room"
"_tbcli_name" "A"
"_tbcli_size" "64 64 64"
"_tbcli_wall" "16"
}
)");
    checkError(
      fixture.apply(R"([{"op": "add_room", "name": "B", "size": [64, 64, 64],
        "place": {"east_of": "A", "gap": 64}}])"),
      "op_failed",
      1,
      0);
    fixture.applyJson(
      R"([{"op": "add_room", "name": "B", "min": [-8, -8, -8], "size": [64, 64, 64]}])");
    CHECK(fixture.builtRooms()["total"] == Json{2});
  }

  SECTION("refuses a name that a room has already")
  {
    const auto result = fixture.apply(
      fmt::format(
        R"([{}, {{"op": "add_room", "name": "A", "min": [1000, 0, 0],
          "size": [64, 64, 64]}}])",
        RoomA));
    checkError(result, "op_failed", 1, 1);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/group:0/entity:0"});
  }

  SECTION("refuses to build in a locked default layer")
  {
    const auto result =
      fixture.apply(fmt::format(R"([{{"op": "lock", "target": "layer:0"}}, {}])", RoomA));
    checkError(result, "locked", 1, 1);
  }

  SECTION("refuses malformed arguments")
  {
    const auto batch = GENERATE(
      R"({"op": "add_room", "name": "a b", "min": [0, 0, 0], "size": [64, 64, 64]})",
      R"({"op": "add_room", "name": "", "min": [0, 0, 0], "size": [64, 64, 64]})",
      R"({"op": "add_room", "name": "A", "min": [0, 0, 0], "size": [64, 0, 64]})",
      R"({"op": "add_room", "name": "A", "min": [0, 0, 0], "size": [64, 64, 64], "wall": 0})",
      R"({"op": "add_room", "name": "A", "size": [64, 64, 64]})",
      R"({"op": "add_room", "name": "A", "min": [0, 0, 0], "size": [64, 64, 64],
          "place": {"east_of": "B", "gap": 64}})",
      R"({"op": "add_room", "name": "A", "size": [64, 64, 64],
          "place": {"east_of": "B"}})",
      R"({"op": "add_room", "name": "A", "size": [64, 64, 64],
          "place": {"east_of": "B", "west_of": "C"}})",
      R"({"op": "add_room", "name": "A", "size": [64, 64, 64],
          "place": {"above": "B", "gap": 64}})");
    CAPTURE(batch);

    checkError(fixture.apply(fmt::format("[{}]", batch)), "invalid_argument", 2, 0);
    CHECK(fixture.unchanged());
  }

  SECTION("a generated room compiles sealed")
  {
    // compiled/built_rooms.map was made by this batch; its qbsp recording has no leak
    fixture.applyJson(
      fmt::format(
        R"([{},
          {{"op": "add_room", "name": "B", "size": [192, 320, 96],
           "place": {{"east_of": "A", "gap": 128}}}},
          {{"op": "create_point_entity", "classname": "info_player_start",
           "origin": [128, 128, 24]}},
          {{"op": "create_point_entity", "classname": "light",
           "origin": [480, 128, 64]}}])",
        RoomA));
    const auto generated = fixture.env.readFile("rooms.map");
    const auto recorded = *readFile(fixturePath("compiled/built_rooms.map"));
    CHECK(withoutVolatileLines(generated) == withoutVolatileLines(recorded));

    auto compilers = Json::object();
    compilers.set(
      std::string{QbspTool},
      arg(setUpFakeCompiler(fixture.env, QbspTool, "built_rooms")));
    auto config = quakeConfig();
    config.set("compilers", std::move(compilers));
    fixture.env.writeConfig({}, config);

    const auto json = runJson({"verify", map, "--compile"});
    for (const auto& issue : json["issues"]["items"].asArray())
    {
      CHECK_FALSE(issue["type"] == Json{"leak"});
    }
    const auto& stages = json["stages"].asArray();
    REQUIRE(stages.size() == 4);
    CHECK(stages[3]["name"] == Json{"qbsp"});
    CHECK_FALSE(stages[3]["status"] == Json{"failed"});
  }
}

TEST_CASE("built rooms")
{
  auto fixture = RoomFixture{};
  const auto& map = fixture.map;

  SECTION("describe lists each built room with its parameters")
  {
    fixture.applyJson(
      fmt::format(
        R"([{}, {{"op": "add_room", "name": "B", "size": [128, 128, 64], "wall": 8,
          "place": {{"south_of": "A", "gap": 64}}}}])",
        RoomA));
    CHECK(fixture.builtRooms() == parseJsonLiteral(R"({
      "items": [
        {
          "name": "A",
          "kind": "room",
          "params": {"size": [256, 256, 128], "wall": 16},
          "status": "rebuildable",
          "instances": ["layer:0/group:0"],
          "group": "layer:0/group:0"
        },
        {
          "name": "B",
          "kind": "room",
          "params": {
            "size": [128, 128, 64], "wall": 8, "place": {"south_of": "A", "gap": 64}
          },
          "status": "rebuildable",
          "instances": ["layer:0/group:1"],
          "group": "layer:0/group:1"
        }
      ],
      "total": 2,
      "truncated": false
    })"));
  }

  SECTION("a map without built rooms lists none")
  {
    CHECK(
      fixture.builtRooms()
      == parseJsonLiteral(R"({"items": [], "total": 0, "truncated": false})"));
  }

  SECTION("moves and retexturing keep a room rebuildable")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(R"([
      {"op": "translate", "target": "layer:0/group:0", "delta": [12.5, -40, 8]},
      {"op": "set_material", "target": "layer:0/group:0/entity:0", "material": "wall"}
    ])");
    CHECK(fixture.builtRooms()["items"][0]["status"] == Json{"rebuildable"});
  }

  SECTION("a removed brush makes a room hand_edited")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(
      R"([{"op": "delete", "target": "layer:0/group:0/entity:0/brush:5"}])");
    CHECK(fixture.builtRooms()["items"][0]["status"] == Json{"hand_edited"});
  }

  SECTION("moving one brush makes a room hand_edited")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(R"([{"op": "translate",
      "target": "layer:0/group:0/entity:0/brush:1", "delta": [0, 0, 16]}])");
    CHECK(fixture.builtRooms()["items"][0]["status"] == Json{"hand_edited"});
  }

  SECTION("malformed parameters are reported as null")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(R"([{"op": "set_property", "target": "layer:0/group:0/entity:0",
      "key": "_tbcli_wall", "value": "thick"}])");
    const auto room = fixture.builtRooms()["items"][0];
    CHECK(room["params"] == Json{});
    CHECK(room["status"] == Json{"hand_edited"});
  }

  SECTION("an ungrouped carrier is still a built room, with no group")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(R"([{"op": "ungroup", "target": "layer:0/group:0"}])");
    const auto room = fixture.builtRooms()["items"][0];
    CHECK(room["group"] == Json{});
    CHECK(room["instances"] == parseJsonLiteral(R"(["layer:0/entity:0"])"));
    CHECK(room["status"] == Json{"rebuildable"});
  }

  SECTION("brushes moved out of the carrier are no built room")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(
      R"([{"op": "move_to_world", "target": "layer:0/group:0/entity:0"}])");
    CHECK(fixture.builtRooms()["total"] == Json{0});
  }

  SECTION("show on the carrier or its group reports the room")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    for (const auto& path : {"layer:0/group:0", "layer:0/group:0/entity:0"})
    {
      CAPTURE(path);
      const auto node = showNode(map, path);
      CHECK(node["instance_of"] == Json{"A"});
      CHECK(node["instances"] == Json{1});
      CHECK(
        node["params"] == parseJsonLiteral(R"({"size": [256, 256, 128], "wall": 16})"));
      CHECK(node["status"] == Json{"rebuildable"});
    }
    CHECK(
      showNode(map, "layer:0/group:0/entity:0/brush:0").find("instance_of") == nullptr);
  }
}

} // namespace tb::cli
