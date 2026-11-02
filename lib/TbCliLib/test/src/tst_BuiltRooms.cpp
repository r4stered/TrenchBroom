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

  /**
   * Checks that the map is the recorded fixture map, and that verify --compile,
   * replaying the fixture's qbsp recording, reports no leak.
   */
  void checkCompilesSealed(const std::string& fixtureName)
  {
    const auto generated = env.readFile("rooms.map");
    const auto recorded =
      *readFile(fixturePath(fmt::format("compiled/{}.map", fixtureName)));
    CHECK(withoutVolatileLines(generated) == withoutVolatileLines(recorded));

    auto compilers = Json::object();
    compilers.set(
      std::string{QbspTool}, arg(setUpFakeCompiler(env, QbspTool, fixtureName)));
    auto config = quakeConfig();
    config.set("compilers", std::move(compilers));
    env.writeConfig({}, config);

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

constexpr auto RoomA = std::string_view{
  R"({"op": "add_room", "name": "A", "min": [0, 0, 0], "size": [256, 256, 128]})"};

/** A room as large as A, east of it at a gap of 128. */
constexpr auto RoomBEastOfA = std::string_view{
  R"({"op": "add_room", "name": "B", "size": [256, 256, 128],
      "place": {"east_of": "A", "gap": 128}})"};

/** The textures of each brush of the entity at the given path. */
std::vector<Json> brushTextures(const std::string& map, const std::string& entityPath)
{
  auto result = std::vector<Json>{};
  const auto entity = showNode(map, entityPath);
  for (const auto& brush : entity["brushes"]["items"].asArray())
  {
    result.push_back(showNode(map, brush.asString())["textures"]);
  }
  return result;
}

/** The last n elements. */
std::vector<Json> lastOf(const std::vector<Json>& values, const size_t n)
{
  return std::vector<Json>(values.end() - static_cast<std::ptrdiff_t>(n), values.end());
}

/** The value of a key of the carrier at the given path. */
Json carrierKey(const std::string& map, const std::string& carrierPath, const char* key)
{
  return showNode(map, carrierPath)["properties"][key];
}

/** The statuses of the built rooms, in file order. */
std::vector<Json> statuses(const RoomFixture& fixture)
{
  const auto builtRooms = fixture.builtRooms();
  auto result = std::vector<Json>{};
  for (const auto& room : builtRooms["items"].asArray())
  {
    result.push_back(room["status"]);
  }
  return result;
}

/** A room north of B at a gap of 64. */
constexpr auto RoomCNorthOfB = std::string_view{
  R"({"op": "add_room", "name": "C", "size": [128, 128, 64],
      "place": {"north_of": "B", "gap": 64}})"};

/** The batch that compiled/built_rooms.map was made by, followed by the given ops. */
std::string builtRoomsBatch(const std::string_view moreOps = {})
{
  return fmt::format(
    R"([{},
      {{"op": "add_room", "name": "B", "size": [192, 320, 96],
       "place": {{"east_of": "A", "gap": 128}}}},
      {{"op": "create_point_entity", "classname": "info_player_start",
       "origin": [128, 128, 24]}},
      {{"op": "create_point_entity", "classname": "light",
       "origin": [480, 128, 64]}}{}])",
    RoomA,
    moreOps);
}

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
    checkError(result, "ambiguous_room_name", 1, 0);
    CHECK(
      parseOutput(result)["error"]["candidates"]
      == parseJsonLiteral(R"(["layer:0/group:0/entity:0", "layer:0/group:1/entity:0"])"));
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

  SECTION("refuses rooms whose walls would overlap a connection")
  {
    fixture.applyJson(
      fmt::format(
        R"([{}, {}, {{"op": "connect", "from": "A", "to": "B"}}])", RoomA, RoomBEastOfA));
    const auto result = fixture.apply(
      R"([{"op": "add_room", "name": "C", "min": [288, 100, 0], "size": [64, 64, 64]}])");
    checkError(result, "layout_overlap", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/group:2/entity:0"});
  }

  SECTION("a generated room compiles sealed")
  {
    // compiled/built_rooms.map was made by this batch; its qbsp recording has no leak
    fixture.applyJson(builtRoomsBatch());
    fixture.checkCompilesSealed("built_rooms");
  }
}

TEST_CASE("connect")
{
  auto fixture = RoomFixture{};
  const auto& map = fixture.map;
  const auto rooms = fmt::format("[{}, {}]", RoomA, RoomBEastOfA);

  SECTION("builds a corridor and cuts both rooms' walls around the opening")
  {
    fixture.applyJson(rooms);
    const auto json =
      fixture.applyJson(R"([{"op": "connect", "from": "A", "to": "B", "height": 96}])");
    const auto& op = json["ops"][0];
    CHECK(op["created"] == parseJsonLiteral(R"(["layer:0/group:2"])"));
    CHECK(
      op["modified"]
      == parseJsonLiteral(R"(["layer:0/group:0/entity:0", "layer:0/group:1/entity:0"])"));
    // the east wall of A and the west wall of B
    CHECK(
      op["removed"]
      == parseJsonLiteral(
        R"(["layer:0/group:0/entity:0/brush:3", "layer:0/group:1/entity:0/brush:2"])"));

    CHECK(showNode(map, "layer:0/group:2")["name"] == Json{"connection:A-B"});
    const auto carrier = showNode(map, "layer:0/group:2/entity:0");
    CHECK(carrier["classname"] == Json{"func_group"});
    const auto& properties = carrier["properties"];
    CHECK(properties["_tbcli_kind"] == Json{"connection"});
    CHECK(properties["_tbcli_from"] == Json{"A"});
    CHECK(properties["_tbcli_to"] == Json{"B"});
    CHECK(properties["_tbcli_width"] == Json{"128"});
    CHECK(properties["_tbcli_height"] == Json{"96"});
    CHECK(properties["_tbcli_offset"] == Json{"0"});
    CHECK(properties["_tbcli_fp"].asString().size() == 16);
    CHECK(properties.find("_tb_type") == nullptr);

    // floor, ceiling, south and north wall between the rooms' outer wall faces, around
    // an interior centred on the shared wall
    CHECK(
      brushAabbs(map, "layer:0/group:2/entity:0")
      == std::vector<Json>{
        parseJsonLiteral(R"({"min": [272, 48, -16], "max": [368, 208, 0]})"),
        parseJsonLiteral(R"({"min": [272, 48, 96], "max": [368, 208, 112]})"),
        parseJsonLiteral(R"({"min": [272, 48, 0], "max": [368, 64, 96]})"),
        parseJsonLiteral(R"({"min": [272, 192, 0], "max": [368, 208, 96]})"),
      });

    // the cut walls are split beside and above the opening
    CHECK(
      brushAabbs(map, "layer:0/group:0/entity:0")
      == std::vector<Json>{
        parseJsonLiteral(R"({"min": [-16, -16, -16], "max": [272, 272, 0]})"),
        parseJsonLiteral(R"({"min": [-16, -16, 128], "max": [272, 272, 144]})"),
        parseJsonLiteral(R"({"min": [-16, -16, 0], "max": [0, 272, 128]})"),
        parseJsonLiteral(R"({"min": [0, -16, 0], "max": [256, 0, 128]})"),
        parseJsonLiteral(R"({"min": [0, 256, 0], "max": [256, 272, 128]})"),
        parseJsonLiteral(R"({"min": [256, -16, 0], "max": [272, 64, 128]})"),
        parseJsonLiteral(R"({"min": [256, 192, 0], "max": [272, 272, 128]})"),
        parseJsonLiteral(R"({"min": [256, 64, 96], "max": [272, 192, 128]})"),
      });
    CHECK(
      lastOf(brushAabbs(map, "layer:0/group:1/entity:0"), 3)
      == std::vector<Json>{
        parseJsonLiteral(R"({"min": [368, -16, 0], "max": [384, 64, 128]})"),
        parseJsonLiteral(R"({"min": [368, 192, 0], "max": [384, 272, 128]})"),
        parseJsonLiteral(R"({"min": [368, 64, 96], "max": [384, 192, 128]})"),
      });
  }

  SECTION("the cut rooms stay rebuildable with the fingerprints of their cut walls")
  {
    fixture.applyJson(rooms);
    const auto fingerprintOfA = [&]() {
      return showNode(map, "layer:0/group:0/entity:0")["properties"]["_tbcli_fp"];
    };
    const auto before = fingerprintOfA();
    fixture.applyJson(R"([{"op": "connect", "from": "A", "to": "B"}])");
    CHECK(fingerprintOfA() != before);
    CHECK(
      statuses(fixture) == std::vector<Json>{Json{"rebuildable"}, Json{"rebuildable"}});
  }

  SECTION("joins rooms apart along either axis, in either order")
  {
    using T = std::tuple<std::string, std::string>;
    const auto [relation, expected] = GENERATE(
      T{"west_of", R"({"min": [-112, 48, -16], "max": [-16, 208, 144]})"},
      T{"north_of", R"({"min": [48, 272, -16], "max": [208, 368, 144]})"});
    CAPTURE(relation);

    fixture.applyJson(
      fmt::format(
        R"([{}, {{"op": "add_room", "name": "B", "size": [256, 256, 128],
          "place": {{"{}": "A", "gap": 128}}}},
          {{"op": "connect", "from": "B", "to": "A"}}])",
        RoomA,
        relation));
    CHECK(showNode(map, "layer:0/group:2")["name"] == Json{"connection:B-A"});
    CHECK(aabbOf(map, "layer:0/group:2/entity:0") == parseJsonLiteral(expected));
  }

  SECTION("centres the corridor on the shared wall at fractional coordinates")
  {
    // rounded relative to the start of the shared wall, so moving both rooms does not
    // move the corridor within them
    fixture.applyJson(R"([
      {"op": "add_room", "name": "A", "min": [0, 0.5, 0], "size": [256, 256, 128]},
      {"op": "add_room", "name": "B", "size": [256, 256, 128],
       "place": {"east_of": "A", "gap": 128}},
      {"op": "connect", "from": "A", "to": "B"}])");
    CHECK(
      aabbOf(map, "layer:0/group:2/entity:0")
      == parseJsonLiteral(R"({"min": [272, 48.5, -16], "max": [368, 208.5, 144]})"));
  }

  SECTION("width and offset place the opening along the shared wall")
  {
    fixture.applyJson(rooms);
    fixture.applyJson(
      R"([{"op": "connect", "from": "A", "to": "B", "width": 64, "offset": 32}])");
    CHECK(
      aabbOf(map, "layer:0/group:2/entity:0")
      == parseJsonLiteral(R"({"min": [272, 112, -16], "max": [368, 208, 144]})"));
    CHECK(
      showNode(map, "layer:0/group:2/entity:0")["properties"]["_tbcli_offset"]
      == Json{"32"});
  }

  SECTION("the cut walls keep their materials")
  {
    fixture.applyJson(rooms);
    fixture.applyJson(R"([
      {"op": "set_material", "target": "layer:0/group:0/entity:0/brush:3",
       "material": "outside"},
      {"op": "set_material", "target": "layer:0/group:0/entity:0/brush:3",
       "faces": "-x", "material": "inside"},
      {"op": "connect", "from": "A", "to": "B", "height": 96}])");
    const auto cut = parseJsonLiteral(R"(["inside", "outside"])");
    CHECK(
      lastOf(brushTextures(map, "layer:0/group:0/entity:0"), 3)
      == std::vector<Json>{cut, cut, cut});
  }

  SECTION("a room whose walls are cut into other boxes stays rebuildable")
  {
    // the order in which openings are cut changes how the walls are cut into boxes
    fixture.applyJson(rooms);
    fixture.applyJson(R"([
      {"op": "create_box", "min": [-16, -16, 0], "max": [0, 128, 128], "material": "a",
       "as": "$south"},
      {"op": "create_box", "min": [-16, 128, 0], "max": [0, 272, 128], "material": "a",
       "as": "$north"},
      {"op": "delete", "target": "layer:0/group:0/entity:0/brush:2"},
      {"op": "move_to_entity", "target": ["$south", "$north"],
       "entity": "layer:0/group:0/entity:0"}])");
    REQUIRE(brushAabbs(map, "layer:0/group:0/entity:0").size() == 7);
    CHECK(fixture.builtRooms()["items"][0]["status"] == Json{"rebuildable"});
  }

  SECTION("as binds the connection's group; the subcommand takes flags")
  {
    fixture.applyJson(rooms);
    const auto json = fixture.applyJson(
      R"([{"op": "connect", "from": "A", "to": "B", "width": 64, "as": "$c"}])");
    CHECK(json["bindings"] == parseJsonLiteral(R"({"$c": "layer:0/group:2"})"));

    fixture.env.writeFile("rooms.map", EmptyMap);
    fixture.applyJson(rooms);
    const auto flags = runJson(
      {"connect", map, "--from", "A", "--to", "B", "--width", "64", "--offset", "-96"});
    CHECK(flags["ops"][0]["created"] == parseJsonLiteral(R"(["layer:0/group:2"])"));
    CHECK(
      aabbOf(map, "layer:0/group:2/entity:0")
      == parseJsonLiteral(R"({"min": [272, -16, -16], "max": [368, 80, 144]})"));
  }

  SECTION("refuses corridors that do not fit the shared wall")
  {
    const auto batch = GENERATE(
      R"({"op": "connect", "from": "A", "to": "B", "width": 257})",
      R"({"op": "connect", "from": "A", "to": "B", "offset": 65})",
      R"({"op": "connect", "from": "A", "to": "B", "offset": -65})");
    CAPTURE(batch);

    fixture.applyJson(rooms);
    const auto before = fixture.env.readFile("rooms.map");
    checkError(
      fixture.apply(fmt::format("[{}]", batch)), "layout_corridor_does_not_fit", 1, 0);
    CHECK(fixture.env.readFile("rooms.map") == before);
  }

  SECTION("refuses rooms that do not stand side by side")
  {
    const auto other = GENERATE(
      // diagonal
      R"({"op": "add_room", "name": "B", "min": [400, 400, 0], "size": [64, 64, 64]})",
      // above
      R"({"op": "add_room", "name": "B", "min": [0, 0, 200], "size": [64, 64, 64]})");
    CAPTURE(other);

    checkError(
      fixture.apply(
        fmt::format(
          R"([{}, {}, {{"op": "connect", "from": "A", "to": "B"}}])", RoomA, other)),
      "layout_corridor_does_not_fit",
      1,
      2);
  }

  SECTION("refuses rooms whose floors are at different heights")
  {
    checkError(
      fixture.apply(
        fmt::format(
          R"([{}, {{"op": "add_room", "name": "B", "min": [384, 0, 16],
            "size": [256, 256, 128]}}, {{"op": "connect", "from": "A", "to": "B"}}])",
          RoomA)),
      "layout_floor_mismatch",
      1,
      2);
  }

  SECTION("refuses corridors taller than a room")
  {
    checkError(
      fixture.apply(
        fmt::format(
          R"([{}, {{"op": "add_room", "name": "B", "size": [256, 256, 64],
            "place": {{"east_of": "A", "gap": 128}}}},
            {{"op": "connect", "from": "A", "to": "B", "height": 65}}])",
          RoomA)),
      "layout_corridor_too_tall",
      1,
      2);
  }

  SECTION("refuses corridors that would overlap another room or connection")
  {
    using T = std::tuple<std::string, std::string, size_t>;
    const auto [batch, node, opIndex] = GENERATE(
      // room C stands between A and B
      T{R"([{"op": "add_room", "name": "C", "min": [288, 100, 0], "size": [64, 64, 64]},
           {"op": "connect", "from": "A", "to": "B"}])",
        "layer:0/group:2/entity:0",
        1},
      // the corridor between C and D would cross the one between A and B
      T{R"([{"op": "add_room", "name": "C", "min": [288, 300, 0], "size": [64, 64, 64]},
           {"op": "add_room", "name": "D", "min": [288, -200, 0], "size": [64, 64, 64]},
           {"op": "connect", "from": "A", "to": "B"},
           {"op": "connect", "from": "C", "to": "D", "width": 64, "height": 64}])",
        "layer:0/group:4/entity:0",
        3});
    CAPTURE(batch);

    fixture.applyJson(rooms);
    const auto result = fixture.apply(batch);
    checkError(result, "layout_overlap", 1, opIndex);
    CHECK(parseOutput(result)["error"]["node"] == Json{node});
  }

  SECTION("refuses rooms whose walls touch, which leave no room for a corridor")
  {
    checkError(
      fixture.apply(
        fmt::format(
          R"([{}, {{"op": "add_room", "name": "B", "size": [256, 256, 128],
            "place": {{"east_of": "A", "gap": 32}}}},
            {{"op": "connect", "from": "A", "to": "B"}}])",
          RoomA)),
      "layout_corridor_does_not_fit",
      1,
      2);
  }

  SECTION("refuses rooms that are joined already, in either direction")
  {
    const auto second = GENERATE(
      R"({"op": "connect", "from": "A", "to": "B", "width": 32, "offset": 96})",
      R"({"op": "connect", "from": "B", "to": "A", "width": 32, "offset": 96})");
    CAPTURE(second);

    fixture.applyJson(rooms);
    const auto result = fixture.apply(
      fmt::format(
        R"([{{"op": "connect", "from": "A", "to": "B", "width": 32}}, {}])", second));
    checkError(result, "op_failed", 1, 1);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/group:2/entity:0"});
  }

  SECTION("refuses a room inside a linked group")
  {
    fixture.applyJson(rooms);
    fixture.applyJson(R"([{"op": "linked_duplicate", "target": "layer:0/group:1",
      "offset": [0, 2000, 0]}])");
    // until linked copies count as one room, rename the copy so that the name is unique
    auto text = fixture.env.readFile("rooms.map");
    const auto copy = text.rfind(R"("_tbcli_name" "B")");
    REQUIRE(copy != std::string::npos);
    text.replace(copy, 17, R"("_tbcli_name" "C")");
    fixture.env.writeFile("rooms.map", text);

    const auto result = fixture.apply(R"([{"op": "connect", "from": "A", "to": "B"}])");
    checkError(result, "connect_cuts_linked_room", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/group:1/entity:0"});
  }

  SECTION("refuses a room that does not exist")
  {
    fixture.applyJson(rooms);
    checkError(
      fixture.apply(R"([{"op": "connect", "from": "A", "to": "C"}])"),
      "node_not_found",
      1,
      0);
  }

  SECTION("refuses a locked room")
  {
    fixture.applyJson(rooms);
    fixture.applyJson(R"([
      {"op": "create_layer", "name": "L", "as": "$l"},
      {"op": "move_to_layer", "target": "layer:0/group:1", "layer": "$l"},
      {"op": "lock", "target": "$l"}])");
    const auto result = fixture.apply(R"([{"op": "connect", "from": "A", "to": "B"}])");
    checkError(result, "locked", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:1/group:0/entity:0"});
  }

  SECTION("refuses a hand edited room, whose walls it cannot cut safely")
  {
    fixture.applyJson(rooms);
    const auto result = fixture.apply(R"([
      {"op": "delete", "target": "layer:0/group:1/entity:0/brush:0"},
      {"op": "connect", "from": "A", "to": "B"}])");
    checkError(result, "hand_edited", 1, 1);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/group:1/entity:0"});
  }

  SECTION("refuses malformed arguments")
  {
    const auto batch = GENERATE(
      R"({"op": "connect", "from": "A", "to": "A"})",
      R"({"op": "connect", "from": "A b", "to": "B"})",
      R"({"op": "connect", "from": "A", "to": "B", "width": 0})",
      R"({"op": "connect", "from": "A", "to": "B", "height": -8})",
      R"({"op": "connect", "from": "A"})");
    CAPTURE(batch);

    checkError(fixture.apply(fmt::format("[{}]", batch)), "invalid_argument", 2, 0);
    CHECK(fixture.unchanged());
  }

  SECTION("two rooms and a connection compile sealed")
  {
    // compiled/connected_rooms.map was made by this batch; its qbsp recording has no
    // leak
    fixture.applyJson(
      builtRoomsBatch(R"(, {"op": "connect", "from": "A", "to": "B", "height": 96})"));
    fixture.checkCompilesSealed("connected_rooms");
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

  SECTION("a parameter key edited by hand makes a room hand_edited")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(R"([{"op": "set_property", "target": "layer:0/group:0/entity:0",
      "key": "_tbcli_size", "value": "512 256 128"}])");
    const auto room = fixture.builtRooms()["items"][0];
    CHECK(room["params"]["size"] == parseJsonLiteral("[512, 256, 128]"));
    CHECK(room["status"] == Json{"hand_edited"});
  }

  SECTION("moving one of two joined rooms alone keeps both rebuildable")
  {
    fixture.applyJson(
      fmt::format(
        R"([{}, {}, {{"op": "connect", "from": "A", "to": "B"}}])", RoomA, RoomBEastOfA));
    fixture.applyJson(
      R"([{"op": "translate", "target": "layer:0/group:1", "delta": [0, 64, 0]}])");
    const auto rebuildable = std::vector<Json>{Json{"rebuildable"}, Json{"rebuildable"}};
    CHECK(statuses(fixture) == rebuildable);

    // a rebuild moves the corridor to the middle of the wall that they share now
    fixture.applyJson(R"([{"op": "edit_room", "room": "A", "wall": 16}])");
    CHECK(
      aabbOf(map, "layer:0/group:2/entity:0")
      == parseJsonLiteral(R"({"min": [272, 80, -16], "max": [368, 240, 144]})"));
    CHECK(statuses(fixture) == rebuildable);
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

  SECTION("rooms that the editor touched read as expected")
  {
    // maps from the GUI carrier check: fresh.map is the room as tbcli writes it,
    // pasted.map was moved, UV edited and pasted into another map, and
    // ungrouped_and_made_structural.map holds one copy that was ungrouped after a
    // linked edit moved its ceiling, and one whose brushes were made structural
    using T = std::tuple<std::string, std::string>;
    const auto [fixtureName, expected] = GENERATE(
      T{"fresh.map", R"([{
        "name": "A", "kind": "room", "params": {"size": [256, 256, 128], "wall": 16},
        "status": "rebuildable", "instances": ["layer:0/group:0"],
        "group": "layer:0/group:0"
      }])"},
      T{"pasted.map", R"([{
        "name": "A", "kind": "room", "params": {"size": [256, 256, 128], "wall": 16},
        "status": "rebuildable", "instances": ["layer:0/group:0"],
        "group": "layer:0/group:0"
      }])"},
      T{"ungrouped_and_made_structural.map", R"([{
        "name": "A", "kind": "room", "params": {"size": [256, 256, 128], "wall": 16},
        "status": "hand_edited", "instances": ["layer:0/entity:2"], "group": null
      }])"});
    CAPTURE(fixtureName);

    auto editFixture = EditFixture{"maps/carrier/" + fixtureName};
    CHECK(
      runJson({"describe", editFixture.map})["built_rooms"]["items"]
      == parseJsonLiteral(expected));
  }

  SECTION("rooms that the editor moved and UV edited can be rebuilt")
  {
    auto editFixture = EditFixture{"maps/carrier/pasted.map"};
    runJson(
      {"apply", editFixture.map, "-"},
      R"([{"op": "edit_room", "room": "A", "size": [256, 256, 192]}])");
    // 64 units taller where the editor moved it
    CHECK(
      aabbOf(editFixture.map, "layer:0/group:0/entity:0")
      == parseJsonLiteral(R"({"min": [-112, -256, -112], "max": [176, 32, 112]})"));
    CHECK(
      runJson({"describe", editFixture.map})["built_rooms"]["items"][0]["status"]
      == Json{"rebuildable"});
  }

  SECTION("a room duplicated in place by the editor is ambiguous")
  {
    auto editFixture = EditFixture{"maps/carrier/fresh.map"};
    runJson(
      {"apply", editFixture.map, "-"},
      R"([{"op": "duplicate", "target": "layer:0/group:0", "offset": [0, 0, 0]}])");
    CHECK(runJson({"describe", editFixture.map})["built_rooms"]["total"] == Json{2});

    const auto result =
      editFixture.apply(R"([{"op": "edit_room", "room": "A", "wall": 8}])");
    checkError(result, "ambiguous_room_name", 1, 0);
    CHECK(
      parseOutput(result)["error"]["candidates"]
      == parseJsonLiteral(R"(["layer:0/group:0/entity:0", "layer:0/group:1/entity:0"])"));
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

TEST_CASE("edit_room")
{
  auto fixture = RoomFixture{};
  const auto& map = fixture.map;
  const auto rooms = fmt::format("[{}, {}]", RoomA, RoomBEastOfA);
  const auto carrierA = std::string{"layer:0/group:0/entity:0"};
  const auto carrierB = std::string{"layer:0/group:1/entity:0"};
  const auto rebuildable = std::vector<Json>{Json{"rebuildable"}, Json{"rebuildable"}};

  SECTION("changes the size of a room where it stands")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    const auto json =
      fixture.applyJson(R"([{"op": "edit_room", "room": "A", "size": [512, 256, 128]}])");
    const auto& op = json["ops"][0];
    CHECK(op["modified"] == parseJsonLiteral(R"(["layer:0/group:0/entity:0"])"));
    CHECK(op["removed"].size() == 6);
    CHECK(op["warnings"] == Json::array());
    CHECK(op.find("stale_placements") == nullptr);

    CHECK(
      aabbOf(map, carrierA)
      == parseJsonLiteral(R"({"min": [-16, -16, -16], "max": [528, 272, 144]})"));
    CHECK(brushAabbs(map, carrierA).size() == 6);
    CHECK(carrierKey(map, carrierA, "_tbcli_size") == Json{"512 256 128"});
    CHECK(statuses(fixture) == std::vector<Json>{Json{"rebuildable"}});
  }

  SECTION("changes the wall thickness around the same interior")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(R"([{"op": "edit_room", "room": "A", "wall": 8}])");
    CHECK(
      aabbOf(map, carrierA)
      == parseJsonLiteral(R"({"min": [-8, -8, -8], "max": [264, 264, 136]})"));
    CHECK(carrierKey(map, carrierA, "_tbcli_wall") == Json{"8"});
  }

  SECTION("lengthening a room moves the rooms placed against it, and theirs")
  {
    fixture.applyJson(fmt::format("[{}, {}, {}]", RoomA, RoomBEastOfA, RoomCNorthOfB));
    const auto json =
      fixture.applyJson(R"([{"op": "edit_room", "room": "A", "size": [512, 256, 128]}])");
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"([
        "layer:0/group:0/entity:0", "layer:0/group:1/entity:0", "layer:0/group:2/entity:0"
      ])"));
    CHECK(
      aabbOf(map, carrierB)
      == parseJsonLiteral(R"({"min": [624, -16, -16], "max": [912, 272, 144]})"));
    CHECK(
      aabbOf(map, "layer:0/group:2/entity:0")
      == parseJsonLiteral(R"({"min": [688, 304, -16], "max": [848, 464, 80]})"));
    CHECK(
      statuses(fixture)
      == std::vector<Json>{
        Json{"rebuildable"}, Json{"rebuildable"}, Json{"rebuildable"}});
  }

  SECTION("a resized room is placed against its room again")
  {
    fixture.applyJson(
      fmt::format(
        R"([{}, {{"op": "add_room", "name": "B", "size": [256, 256, 128],
          "place": {{"west_of": "A", "gap": 128}}}}])",
        RoomA));
    fixture.applyJson(R"([{"op": "edit_room", "room": "B", "size": [384, 128, 128]}])");
    // the gap to A stays 128, and B is centred on A again
    CHECK(
      aabbOf(map, carrierB)
      == parseJsonLiteral(R"({"min": [-528, 48, -16], "max": [-112, 208, 144]})"));
  }

  SECTION("keeps how far a room was moved along the room it is placed against")
  {
    fixture.applyJson(rooms);
    fixture.applyJson(R"([
      {"op": "translate", "target": "layer:0/group:1", "delta": [0, 32, 0]},
      {"op": "edit_room", "room": "A", "size": [512, 256, 128]}])");
    CHECK(
      aabbOf(map, carrierB)
      == parseJsonLiteral(R"({"min": [624, 16, -16], "max": [912, 304, 144]})"));
  }

  SECTION("a room that no longer stands at its placement stays where it is")
  {
    fixture.applyJson(rooms);
    fixture.applyJson(
      R"([{"op": "translate", "target": "layer:0/group:1", "delta": [0, 0, 16]}])");
    const auto before = aabbOf(map, carrierB);
    const auto json =
      fixture.applyJson(R"([{"op": "edit_room", "room": "A", "size": [256, 512, 128]}])");
    const auto& op = json["ops"][0];
    CHECK(op["modified"] == parseJsonLiteral(R"(["layer:0/group:0/entity:0"])"));
    CHECK(op["stale_placements"] == parseJsonLiteral(R"(["layer:0/group:1/entity:0"])"));
    CHECK(op["warnings"] == parseJsonLiteral(R"(["stale_placement"])"));
    CHECK(aabbOf(map, carrierB) == before);
  }

  SECTION("a room whose room to place against is gone keeps its minimum")
  {
    fixture.applyJson(rooms);
    fixture.applyJson(R"([{"op": "delete", "target": "layer:0/group:0"}])");
    const auto json =
      fixture.applyJson(R"([{"op": "edit_room", "room": "B", "size": [128, 128, 128]}])");
    CHECK(
      json["ops"][0]["stale_placements"]
      == parseJsonLiteral(R"(["layer:0/group:0/entity:0"])"));
    CHECK(
      aabbOf(map, "layer:0/group:0/entity:0")
      == parseJsonLiteral(R"({"min": [368, -16, -16], "max": [528, 144, 144]})"));
  }

  SECTION("place places a room against another anew")
  {
    fixture.applyJson(
      fmt::format(
        R"([{}, {}, {{"op": "add_room", "name": "C", "min": [0, 1000, 0],
          "size": [128, 128, 128]}}])",
        RoomA,
        RoomBEastOfA));
    fixture.applyJson(R"([{"op": "edit_room", "room": "B",
      "place": {"north_of": "C", "gap": 64}}])");
    CHECK(
      aabbOf(map, carrierB)
      == parseJsonLiteral(R"({"min": [-80, 1176, -16], "max": [208, 1464, 144]})"));
    CHECK(carrierKey(map, carrierB, "_tbcli_place") == Json{"north_of C 64"});
  }

  SECTION("refuses to place a room against itself or a room placed against it")
  {
    const auto edit = GENERATE(
      R"({"op": "edit_room", "room": "B", "place": {"east_of": "B", "gap": 64}})",
      R"({"op": "edit_room", "room": "A", "place": {"east_of": "B", "gap": 64}})");
    CAPTURE(edit);

    fixture.applyJson(rooms);
    const auto before = fixture.env.readFile("rooms.map");
    checkError(fixture.apply(fmt::format("[{}]", edit)), "op_failed", 1, 0);
    CHECK(fixture.env.readFile("rooms.map") == before);
  }

  SECTION("rebuilds the connections of the rooms that change")
  {
    fixture.applyJson(rooms);
    const auto json = fixture.applyJson(R"([
      {"op": "connect", "from": "A", "to": "B"},
      {"op": "edit_room", "room": "A", "size": [384, 256, 128]}])");
    CHECK(json["ops"][1]["modified"] == parseJsonLiteral(R"([
        "layer:0/group:0/entity:0", "layer:0/group:1/entity:0", "layer:0/group:2/entity:0"
      ])"));
    CHECK(
      aabbOf(map, "layer:0/group:2/entity:0")
      == parseJsonLiteral(R"({"min": [400, 48, -16], "max": [496, 208, 144]})"));
    CHECK(statuses(fixture) == rebuildable);

    // the same brushes as building the rooms and the connection that way
    auto direct = RoomFixture{};
    direct.applyJson(R"([
      {"op": "add_room", "name": "A", "min": [0, 0, 0], "size": [384, 256, 128]},
      {"op": "add_room", "name": "B", "size": [256, 256, 128],
       "place": {"east_of": "A", "gap": 128}},
      {"op": "connect", "from": "A", "to": "B"}])");
    for (const auto& path : {carrierA, carrierB, std::string{"layer:0/group:2/entity:0"}})
    {
      CAPTURE(path);
      CHECK(
        carrierKey(map, path, "_tbcli_fp") == carrierKey(direct.map, path, "_tbcli_fp"));
    }
  }

  SECTION("a wall change rebuilds the room's connections")
  {
    fixture.applyJson(rooms);
    fixture.applyJson(R"([
      {"op": "connect", "from": "A", "to": "B"},
      {"op": "edit_room", "room": "A", "wall": 8}])");
    CHECK(
      aabbOf(map, "layer:0/group:2/entity:0")
      == parseJsonLiteral(R"({"min": [264, 48, -16], "max": [368, 208, 144]})"));
    CHECK(statuses(fixture) == rebuildable);
  }

  SECTION("rebuilt brushes keep the materials of the old brushes on their side")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(R"([
      {"op": "set_material", "target": "layer:0/group:0/entity:0", "material": "wall"},
      {"op": "set_material", "target": "layer:0/group:0/entity:0/brush:0",
       "material": "floor"},
      {"op": "set_material", "target": "layer:0/group:0/entity:0/brush:3",
       "material": "east"},
      {"op": "edit_room", "room": "A", "size": [512, 256, 128]}])");
    // floor, ceiling, west, east, south and north
    CHECK(
      brushTextures(map, carrierA)
      == std::vector<Json>{
        parseJsonLiteral(R"(["floor"])"),
        parseJsonLiteral(R"(["wall"])"),
        parseJsonLiteral(R"(["wall"])"),
        parseJsonLiteral(R"(["east"])"),
        parseJsonLiteral(R"(["wall"])"),
        parseJsonLiteral(R"(["wall"])"),
      });
  }

  SECTION("refuses a hand edited room unless force is given")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(
      R"([{"op": "delete", "target": "layer:0/group:0/entity:0/brush:5"}])");
    const auto before = fixture.env.readFile("rooms.map");
    const auto result =
      fixture.apply(R"([{"op": "edit_room", "room": "A", "size": [512, 256, 128]}])");
    checkError(result, "hand_edited", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{carrierA});
    CHECK(fixture.env.readFile("rooms.map") == before);

    fixture.applyJson(
      R"([{"op": "edit_room", "room": "A", "size": [512, 256, 128], "force": true}])");
    CHECK(brushAabbs(map, carrierA).size() == 6);
    CHECK(statuses(fixture) == std::vector<Json>{Json{"rebuildable"}});
  }

  SECTION("refuses a room whose parameter keys were edited by hand")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(R"([{"op": "set_property", "target": "layer:0/group:0/entity:0",
      "key": "_tbcli_size", "value": "512 256 128"}])");
    checkError(
      fixture.apply(R"([{"op": "edit_room", "room": "A", "wall": 8}])"),
      "hand_edited",
      1,
      0);
  }

  SECTION("refuses to move a room placed against it that was edited by hand")
  {
    fixture.applyJson(rooms);
    fixture.applyJson(
      R"([{"op": "delete", "target": "layer:0/group:1/entity:0/brush:5"}])");
    const auto result =
      fixture.apply(R"([{"op": "edit_room", "room": "A", "size": [512, 256, 128]}])");
    checkError(result, "hand_edited", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{carrierB});
  }

  SECTION("refuses layouts that would overlap")
  {
    fixture.applyJson(
      fmt::format(
        R"([{}, {{"op": "add_room", "name": "C", "min": [400, 0, 0],
          "size": [64, 64, 64]}}])",
        RoomA));
    const auto result =
      fixture.apply(R"([{"op": "edit_room", "room": "A", "size": [512, 256, 128]}])");
    checkError(result, "layout_overlap", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{carrierB});
  }

  SECTION("refuses a connection that no longer fits")
  {
    fixture.applyJson(rooms);
    fixture.applyJson(R"([{"op": "connect", "from": "A", "to": "B"}])");
    checkError(
      fixture.apply(R"([{"op": "edit_room", "room": "B", "size": [256, 64, 128]}])"),
      "layout_corridor_does_not_fit",
      1,
      0);
  }

  SECTION("refuses an ambiguous name, listing every candidate")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(
      R"([{"op": "duplicate", "target": "layer:0/group:0", "offset": [0, 1000, 0]}])");
    const auto result = fixture.apply(R"([{"op": "edit_room", "room": "A", "wall": 8}])");
    checkError(result, "ambiguous_room_name", 1, 0);
    CHECK(
      parseOutput(result)["error"]["candidates"]
      == parseJsonLiteral(R"(["layer:0/group:0/entity:0", "layer:0/group:1/entity:0"])"));
  }

  SECTION("refuses to move a locked room")
  {
    fixture.applyJson(rooms);
    fixture.applyJson(R"([
      {"op": "create_layer", "name": "L", "as": "$l"},
      {"op": "move_to_layer", "target": "layer:0/group:1", "layer": "$l"},
      {"op": "lock", "target": "$l"}])");
    const auto result =
      fixture.apply(R"([{"op": "edit_room", "room": "A", "size": [512, 256, 128]}])");
    checkError(result, "locked", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:1/group:0/entity:0"});
  }

  SECTION("refuses a room that does not exist")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    checkError(
      fixture.apply(R"([{"op": "edit_room", "room": "B", "wall": 8}])"),
      "node_not_found",
      1,
      0);
  }

  SECTION("refuses malformed arguments")
  {
    const auto batch = GENERATE(
      R"({"op": "edit_room", "room": "a b", "wall": 8})",
      R"({"op": "edit_room", "room": "A", "size": [64, 0, 64]})",
      R"({"op": "edit_room", "room": "A", "wall": 0})",
      R"({"op": "edit_room", "room": "A", "place": {"east_of": "B"}})",
      R"({"op": "edit_room", "wall": 8})");
    CAPTURE(batch);

    checkError(fixture.apply(fmt::format("[{}]", batch)), "invalid_argument", 2, 0);
    CHECK(fixture.unchanged());
  }

  SECTION("rooms and connections moved together stay rebuildable")
  {
    fixture.applyJson(rooms);
    fixture.applyJson(R"([{"op": "connect", "from": "A", "to": "B"}])");
    fixture.applyJson(R"([{"op": "translate", "target": ["layer:0/group:0",
      "layer:0/group:1", "layer:0/group:2"], "delta": [12.5, -40, 8]}])");
    CHECK(statuses(fixture) == rebuildable);

    fixture.applyJson(R"([{"op": "edit_room", "room": "A", "size": [384, 256, 128]}])");
    CHECK(
      aabbOf(map, "layer:0/group:2/entity:0")
      == parseJsonLiteral(R"({"min": [412.5, 8, -8], "max": [508.5, 168, 152]})"));
    CHECK(statuses(fixture) == rebuildable);
  }
}

TEST_CASE("edit_connection")
{
  auto fixture = RoomFixture{};
  const auto& map = fixture.map;
  const auto connected = fmt::format(
    R"([{}, {}, {{"op": "connect", "from": "A", "to": "B"}}])", RoomA, RoomBEastOfA);
  const auto carrier = std::string{"layer:0/group:2/entity:0"};
  const auto rebuildable = std::vector<Json>{Json{"rebuildable"}, Json{"rebuildable"}};

  SECTION("rebuilds the corridor and cuts the rooms' walls again")
  {
    fixture.applyJson(connected);
    const auto json = fixture.applyJson(R"([{"op": "edit_connection",
      "connection": "A-B", "width": 64, "offset": 32}])");
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"([
        "layer:0/group:0/entity:0", "layer:0/group:1/entity:0", "layer:0/group:2/entity:0"
      ])"));
    CHECK(
      aabbOf(map, carrier)
      == parseJsonLiteral(R"({"min": [272, 112, -16], "max": [368, 208, 144]})"));
    CHECK(carrierKey(map, carrier, "_tbcli_width") == Json{"64"});
    CHECK(carrierKey(map, carrier, "_tbcli_height") == Json{"128"});
    CHECK(carrierKey(map, carrier, "_tbcli_offset") == Json{"32"});
    CHECK(statuses(fixture) == rebuildable);

    // the same brushes as connecting the rooms that way
    auto direct = RoomFixture{};
    direct.applyJson(
      fmt::format(
        R"([{}, {}, {{"op": "connect", "from": "A", "to": "B", "width": 64,
          "offset": 32}}])",
        RoomA,
        RoomBEastOfA));
    for (const auto& path :
         {std::string{"layer:0/group:0/entity:0"},
          std::string{"layer:0/group:1/entity:0"},
          carrier})
    {
      CAPTURE(path);
      CHECK(
        carrierKey(map, path, "_tbcli_fp") == carrierKey(direct.map, path, "_tbcli_fp"));
    }
  }

  SECTION("the rooms can be named in either order")
  {
    fixture.applyJson(connected);
    fixture.applyJson(
      R"([{"op": "edit_connection", "connection": "B-A", "height": 64}])");
    CHECK(carrierKey(map, carrier, "_tbcli_height") == Json{"64"});
    CHECK(statuses(fixture) == rebuildable);
  }

  SECTION("refuses a hand edited connection unless force is given")
  {
    fixture.applyJson(connected);
    fixture.applyJson(
      R"([{"op": "delete", "target": "layer:0/group:2/entity:0/brush:0"}])");
    const auto result =
      fixture.apply(R"([{"op": "edit_connection", "connection": "A-B", "width": 64}])");
    checkError(result, "hand_edited", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{carrier});

    fixture.applyJson(R"([{"op": "edit_connection", "connection": "A-B",
      "width": 64, "force": true}])");
    CHECK(brushAabbs(map, carrier).size() == 4);
  }

  SECTION("refuses a corridor taller than a room")
  {
    fixture.applyJson(connected);
    checkError(
      fixture.apply(R"([{"op": "edit_connection", "connection": "A-B", "height": 200}])"),
      "layout_corridor_too_tall",
      1,
      0);
  }

  SECTION("refuses a connection that does not exist")
  {
    fixture.applyJson(connected);
    checkError(
      fixture.apply(R"([{"op": "edit_connection", "connection": "A-C", "width": 64}])"),
      "node_not_found",
      1,
      0);
  }

  SECTION("refuses malformed arguments")
  {
    const auto batch = GENERATE(
      R"({"op": "edit_connection", "connection": "A"})",
      R"({"op": "edit_connection", "connection": "A-A"})",
      R"({"op": "edit_connection", "connection": "A-B-C"})",
      R"({"op": "edit_connection", "connection": "A-b c"})",
      R"({"op": "edit_connection", "connection": "A-B", "width": 0})",
      R"({"op": "edit_connection", "connection": "A-B", "height": -1})");
    CAPTURE(batch);

    checkError(fixture.apply(fmt::format("[{}]", batch)), "invalid_argument", 2, 0);
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("detach")
{
  auto fixture = RoomFixture{};
  const auto& map = fixture.map;

  SECTION("strips a room's keys and leaves its brushes")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    const auto json = fixture.applyJson(R"([{"op": "detach", "room": "A"}])");
    CHECK(
      json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/group:0/entity:0"])"));

    const auto carrier = showNode(map, "layer:0/group:0/entity:0");
    CHECK(carrier["properties"] == parseJsonLiteral(R"({"classname": "func_group"})"));
    CHECK(brushAabbs(map, "layer:0/group:0/entity:0").size() == 6);
    CHECK(showNode(map, "layer:0/group:0")["name"] == Json{"built_room:A"});
    CHECK(fixture.builtRooms()["total"] == Json{0});
  }

  SECTION("strips a connection's keys; the rooms it joined stay rebuildable")
  {
    fixture.applyJson(
      fmt::format(
        R"([{}, {}, {{"op": "connect", "from": "A", "to": "B"}}])", RoomA, RoomBEastOfA));
    fixture.applyJson(R"([{"op": "detach", "connection": "B-A"}])");
    CHECK(
      showNode(map, "layer:0/group:2/entity:0")["properties"]
      == parseJsonLiteral(R"({"classname": "func_group"})"));
    CHECK(
      statuses(fixture) == std::vector<Json>{Json{"rebuildable"}, Json{"rebuildable"}});

    // rebuilding a room closes the opening of the detached connection
    fixture.applyJson(R"([{"op": "edit_room", "room": "A", "wall": 16}])");
    CHECK(brushAabbs(map, "layer:0/group:0/entity:0").size() == 6);
  }

  SECTION("a room joined to a detached room cannot be rebuilt")
  {
    fixture.applyJson(
      fmt::format(
        R"([{}, {}, {{"op": "connect", "from": "A", "to": "B"}}])", RoomA, RoomBEastOfA));
    fixture.applyJson(R"([{"op": "detach", "room": "A"}])");
    CHECK(statuses(fixture) == std::vector<Json>{Json{"rebuildable"}});
    checkError(
      fixture.apply(R"([{"op": "edit_room", "room": "B", "wall": 16}])"),
      "node_not_found",
      1,
      0);
  }

  SECTION("detaches a room whose keys are malformed")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(R"([
      {"op": "set_property", "target": "layer:0/group:0/entity:0", "key": "_tbcli_wall",
       "value": "thick"},
      {"op": "detach", "room": "A"}])");
    CHECK(fixture.builtRooms()["total"] == Json{0});
  }

  SECTION("refuses an ambiguous name, listing every candidate")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    fixture.applyJson(
      R"([{"op": "duplicate", "target": "layer:0/group:0", "offset": [0, 1000, 0]}])");
    const auto result = fixture.apply(R"([{"op": "detach", "room": "A"}])");
    checkError(result, "ambiguous_room_name", 1, 0);
    CHECK(parseOutput(result)["error"]["candidates"].size() == 2);
  }

  SECTION("refuses a room or connection that does not exist")
  {
    const auto batch = GENERATE(
      R"({"op": "detach", "room": "B"})", R"({"op": "detach", "connection": "A-B"})");
    CAPTURE(batch);

    fixture.applyJson(fmt::format("[{}]", RoomA));
    checkError(fixture.apply(fmt::format("[{}]", batch)), "node_not_found", 1, 0);
  }

  SECTION("refuses a locked carrier")
  {
    fixture.applyJson(fmt::format("[{}]", RoomA));
    checkError(
      fixture.apply(R"([{"op": "lock", "target": "layer:0"},
        {"op": "detach", "room": "A"}])"),
      "locked",
      1,
      1);
  }

  SECTION("refuses malformed arguments")
  {
    const auto batch = GENERATE(
      R"({"op": "detach"})",
      R"({"op": "detach", "room": "A", "connection": "A-B"})",
      R"({"op": "detach", "room": "a b"})",
      R"({"op": "detach", "connection": "AB"})");
    CAPTURE(batch);

    checkError(fixture.apply(fmt::format("[{}]", batch)), "invalid_argument", 2, 0);
  }
}

} // namespace tb::cli
