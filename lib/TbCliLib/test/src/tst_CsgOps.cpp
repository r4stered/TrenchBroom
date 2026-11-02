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

#include <fmt/format.h>

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

/** The node at the given path, as show reports it. */
Json show(const std::string& map, const std::string& path)
{
  return runJson({"show", map, path})["node"];
}

/** The paths of the brushes whose bounds overlap the box. */
Json brushesIntersecting(const std::string& map, const std::string& box)
{
  const auto json = runJson({"query", map, "type is brush", "--intersects", box});
  auto result = Json::array();
  for (const auto& item : json["items"].asArray())
  {
    result.push(item["path"]);
  }
  return result;
}

} // namespace

// maps/simple.map: layer:0 holds brush:0 (-64 -64 -16 to 64 64 0), brush:1 (-64 -64 0
// to -48 64 128), entity:0 (info_player_start at 0 80 24), entity:1 (func_door, 16 -16 0
// to 48 16 96) and entity:2 (light at 100.1234 0 64)

TEST_CASE("hollow")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("replaces a brush with walls of the given thickness")
  {
    const auto json = runJson(
      {"hollow", map, "--target", "layer:0/entity:1/brush:0", "--thickness", "8"});
    CHECK(json["ops"][0]["created"] == parseJsonLiteral(R"([
        "layer:0/entity:1/brush:0", "layer:0/entity:1/brush:1",
        "layer:0/entity:1/brush:2", "layer:0/entity:1/brush:3",
        "layer:0/entity:1/brush:4", "layer:0/entity:1/brush:5"
      ])"));
    CHECK(
      json["ops"][0]["removed"] == parseJsonLiteral(R"(["layer:0/entity:1/brush:0"])"));

    // the walls keep the bounds, and the inside is empty
    CHECK(
      show(map, "layer:0/entity:1")["aabb"]
      == parseJsonLiteral(R"({"min": [16, -16, 0], "max": [48, 16, 96]})"));
    CHECK(brushesIntersecting(map, "25,-7,9,39,7,87") == parseJsonLiteral("[]"));
    CHECK(brushesIntersecting(map, "17,-15,1,23,15,7").size() > 0);
  }

  SECTION("the thickness defaults to 16 and does not depend on a grid")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "create_box", "min": [0, 0, 0], "max": [64, 64, 64], "material": "a",
       "as": "$box"},
      {"op": "hollow", "target": "$box"},
      {"op": "create_box", "min": [100, 0, 0], "max": [130, 30, 30], "material": "a",
       "as": "$odd"},
      {"op": "hollow", "target": "$odd", "thickness": 5}
    ])"));
    CHECK(json["ops"][1]["created"].size() == 6);
    CHECK(json["ops"][3]["created"].size() == 6);
    CHECK(brushesIntersecting(map, "17,17,17,47,47,47") == parseJsonLiteral("[]"));
    CHECK(brushesIntersecting(map, "106,6,6,124,24,24") == parseJsonLiteral("[]"));
    CHECK(
      brushesIntersecting(map, "100,0,0,104,30,30")
      == parseJsonLiteral(R"(["layer:0/brush:9"])"));
  }

  SECTION("a layer stands for its brushes")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "hollow", "target": "layer:0", "thickness": 4}
    ])"));
    CHECK(
      json["ops"][0]["removed"]
      == parseJsonLiteral(
        R"(["layer:0/brush:0", "layer:0/brush:1", "layer:0/entity:1/brush:0"])"));
    CHECK(json["ops"][0]["created"].size() == 18);
  }

  SECTION("refuses a brush too thin to be hollowed")
  {
    const auto result = fixture.apply(R"([
      {"op": "hollow", "target": ["layer:0/brush:1", "layer:0/brush:0"]}
    ])");
    checkError(result, "op_failed", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/brush:1"});
    CHECK(fixture.unchanged());
  }

  SECTION("refuses a thickness that is not greater than 0")
  {
    checkError(
      runCli({"hollow", map, "--target", "layer:0/brush:0", "--thickness", "0"}),
      "invalid_argument",
      2);
    CHECK(fixture.unchanged());
  }

  SECTION("refuses nodes that are not brushes or containers")
  {
    checkError(
      runCli({"hollow", map, "--target", "layer:0/entity:0"}), "wrong_node_kind", 1, 0);
  }
}

TEST_CASE("subtract")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  // a doorway through the wall brush:1, which also touches the floor brush:0
  constexpr auto DoorwayCutter =
    R"({"op": "create_box", "min": [-80, -16, 0], "max": [-32, 16, 96], "material": "a", "as": "$door"})";

  SECTION("cuts a doorway and deletes the cutter")
  {
    const auto json = parseOutput(fixture.apply(
      fmt::format(
        R"([{}, {{"op": "subtract", "cutters": "$door", "from": "layer:0"}}])",
        DoorwayCutter)));
    const auto& op = json["ops"][1];
    // the floor that the cutter only touches is left as it is
    CHECK(op["removed"] == parseJsonLiteral(R"(["layer:0/brush:1"])"));
    CHECK(op["created"].size() == 3);
    CHECK(op["modified"] == parseJsonLiteral("[]"));
    CHECK(json["bindings"] == parseJsonLiteral("{}"));

    CHECK(brushesIntersecting(map, "-70,-15,1,-40,15,95") == parseJsonLiteral("[]"));
    CHECK(
      show(map, "layer:0/brush:0")["aabb"]
      == parseJsonLiteral(R"({"min": [-64, -64, -16], "max": [64, 64, 0]})"));
    CHECK(brushCount(map) == 5);
  }

  SECTION("keeps the cutters if asked to")
  {
    const auto json = parseOutput(fixture.apply(
      fmt::format(
        R"([{}, {{"op": "subtract", "cutters": "$door", "from": "layer:0/brush:1", "keep_cutters": true}}])",
        DoorwayCutter)));
    CHECK(json["ops"][1]["removed"] == parseJsonLiteral(R"(["layer:0/brush:1"])"));
    CHECK(json["bindings"]["$door"].isString());
    CHECK(brushCount(map) == 6);
  }

  SECTION("an existing cutter is reported as removed")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "translate", "target": "layer:0/entity:1", "delta": [-80, 0, 0]},
      {"op": "subtract", "cutters": "layer:0/entity:1", "from": "layer:0/brush:1"}
    ])"));
    CHECK(
      json["ops"][1]["removed"]
      == parseJsonLiteral(
        R"(["layer:0/brush:1", "layer:0/entity:1/brush:0", "layer:0/entity:1"])"));
  }

  SECTION("warns and changes nothing if no cutter overlaps a brush of from")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "create_box", "min": [500, 0, 0], "max": [516, 16, 16], "material": "a",
       "as": "$far"},
      {"op": "subtract", "cutters": "$far", "from": "layer:0"}
    ])"));
    CHECK(json["ops"][1]["warnings"] == parseJsonLiteral(R"(["nothing_cut"])"));
    CHECK(json["ops"][1]["removed"] == parseJsonLiteral("[]"));
    CHECK(json["bindings"]["$far"].isString());
  }

  SECTION("a cutter does not cut itself")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "subtract", "cutters": "layer:0/brush:1", "from": "layer:0/brush:1"}
    ])"));
    CHECK(json["ops"][0]["warnings"] == parseJsonLiteral(R"(["nothing_cut"])"));
    CHECK(brushCount(map) == 3);
  }

  SECTION("from is required")
  {
    checkError(
      runCli({"subtract", map, "--cutters", "layer:0/brush:1"}), "invalid_argument", 2);
  }
}

TEST_CASE("merge")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("replaces brushes with their convex hull")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "merge", "target": ["layer:0/brush:1", "layer:0/brush:0"]}
    ])"));
    CHECK(json["ops"][0]["created"] == parseJsonLiteral(R"(["layer:0/brush:0"])"));
    CHECK(
      json["ops"][0]["removed"]
      == parseJsonLiteral(R"(["layer:0/brush:1", "layer:0/brush:0"])"));
    const auto merged = show(map, "layer:0/brush:0");
    CHECK(
      merged["aabb"]
      == parseJsonLiteral(R"({"min": [-64, -64, -16], "max": [64, 64, 128]})"));
    CHECK(brushCount(map) == 2);
  }

  SECTION("the hull goes in the entity of the first brush")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "merge", "target": ["layer:0/entity:1/brush:0", "layer:0/brush:1"]}
    ])"));
    CHECK(
      json["ops"][0]["created"] == parseJsonLiteral(R"(["layer:0/entity:1/brush:0"])"));
  }

  SECTION("refuses a single brush")
  {
    const auto result = runCli({"merge", map, "--target", "layer:0/brush:0"});
    checkError(result, "op_failed", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/brush:0"});
    CHECK(fixture.unchanged());
  }

  SECTION("an empty query does nothing")
  {
    const auto json =
      runJson({"merge", map, "--query", R"(material == "none")", "--dry-run"});
    CHECK(json["ops"][0]["created"] == parseJsonLiteral("[]"));
  }
}

TEST_CASE("intersect")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("replaces brushes with the space they share")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "create_box", "min": [-80, -8, 64], "max": [0, 8, 80], "material": "a",
       "as": "$beam"},
      {"op": "intersect", "target": ["layer:0/brush:1", "$beam"]}
    ])"));
    CHECK(json["ops"][1]["created"] == parseJsonLiteral(R"(["layer:0/brush:1"])"));
    CHECK(json["ops"][1]["removed"] == parseJsonLiteral(R"(["layer:0/brush:1"])"));
    CHECK(
      show(map, "layer:0/brush:1")["aabb"]
      == parseJsonLiteral(R"({"min": [-64, -8, 64], "max": [-48, 8, 80]})"));
    CHECK(brushCount(map) == 3);
  }

  SECTION("refuses brushes that share no volume")
  {
    const auto result = fixture.apply(R"([
      {"op": "intersect", "target": ["layer:0/brush:0", "layer:0/brush:1"]}
    ])");
    checkError(result, "op_failed", 1, 0);
    CHECK(fixture.unchanged());
  }

  SECTION("refuses a single brush")
  {
    checkError(
      runCli({"intersect", map, "--target", "layer:0/brush:0"}), "op_failed", 1, 0);
  }
}

TEST_CASE("snap_vertices")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("snaps vertices to the grid, and modifies only brushes that change")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "create_brush", "material": "a", "as": "$wedge",
       "vertices": [[0, 0, 0], [32.4, 0, 0], [0, 31.7, 0], [0, 0, 16.2]]},
      {"op": "snap_vertices", "target": ["$wedge", "layer:0/brush:0"], "grid": 8}
    ])"));
    CHECK(json["ops"][1]["modified"] == json["ops"][0]["created"]);

    const auto wedge = show(map, json["bindings"]["$wedge"].asString());
    CHECK(
      wedge["aabb"] == parseJsonLiteral(R"({"min": [0, 0, 0], "max": [32, 32, 16]})"));
  }

  SECTION("refuses a grid that is not greater than 0")
  {
    checkError(
      runCli({"snap_vertices", map, "--target", "layer:0/brush:0", "--grid", "-1"}),
      "invalid_argument",
      2);
  }

  SECTION("refuses a brush that would collapse")
  {
    const auto result = fixture.apply(R"([
      {"op": "create_box", "min": [0, 0, 0], "max": [64, 64, 3], "material": "a",
       "as": "$thin"},
      {"op": "snap_vertices", "target": "$thin", "grid": 16}
    ])");
    checkError(result, "op_failed", 1, 1);
    CHECK(fixture.unchanged());
  }
}

} // namespace tb::cli
