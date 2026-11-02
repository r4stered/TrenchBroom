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

/** The axis-aligned bounds of the node at the given path. */
Json aabbOf(const std::string& map, const std::string& path)
{
  return show(map, path)["aabb"];
}

} // namespace

// maps/simple.map: layer:0 holds brush:0 (-64 -64 -16 to 64 64 0), brush:1 (-64 -64 0
// to -48 64 128), entity:0 (info_player_start at 0 80 24), entity:1 (func_door, 16 -16 0
// to 48 16 96) and entity:2 (light at 100.1234 0 64)

TEST_CASE("translate")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("moves a brush")
  {
    const auto json =
      runJson({"translate", map, "--target", "layer:0/brush:1", "--delta", "16,0,8"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/brush:1"])"));
    CHECK(
      aabbOf(map, "layer:0/brush:1")
      == parseJsonLiteral(R"({"min": [-48, -64, 8], "max": [-32, 64, 136]})"));
  }

  SECTION("keeps the materials aligned unless uv_lock is false")
  {
    // UV offsets are kept modulo the material's size, so this needs materials loaded;
    // the -y face of maps/faces.map's brush:0 has the UV axes +x and -z
    auto faces = EditFixture{"maps/faces.map"};
    setUpMaterials(faces.env);

    const auto uvLock = GENERATE(true, false);
    CAPTURE(uvLock);
    runJson(
      {"translate",
       faces.map,
       "--target",
       "layer:0/brush:0",
       "--delta",
       "8,0,4",
       fmt::format("--uv-lock={}", uvLock)});
    CHECK(
      show(faces.map, "layer:0/brush:0/face:1")["uv"]["offset"]
      == parseJsonLiteral(uvLock ? "[-8, 4]" : "[0, 0]"));
  }

  SECTION("moves point entities and brush entities")
  {
    const auto json = parseOutput(fixture.apply(R"([{
      "op": "translate", "target": ["layer:0/entity:0", "layer:0/entity:1"],
      "delta": [0, 0, 32]
    }])"));
    CHECK(
      json["ops"][0]["modified"]
      == parseJsonLiteral(R"(["layer:0/entity:0", "layer:0/entity:1"])"));
    CHECK(show(map, "layer:0/entity:0")["origin"] == parseJsonLiteral("[0, 80, 56]"));
    CHECK(
      aabbOf(map, "layer:0/entity:1/brush:0")
      == parseJsonLiteral(R"({"min": [16, -16, 32], "max": [48, 16, 128]})"));
  }

  SECTION("a layer stands for its nodes, and a node inside a target moves once")
  {
    const auto json = parseOutput(fixture.apply(R"([{
      "op": "translate", "target": ["layer:0/entity:1/brush:0", "layer:0"],
      "delta": [8, 0, 0]
    }])"));
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"([
        "layer:0/brush:0", "layer:0/brush:1", "layer:0/entity:0", "layer:0/entity:1",
        "layer:0/entity:2"
      ])"));
    CHECK(
      aabbOf(map, "layer:0/entity:1/brush:0")
      == parseJsonLiteral(R"({"min": [24, -16, 0], "max": [56, 16, 96]})"));
  }

  SECTION("a query that matches nothing changes nothing")
  {
    const auto json =
      runJson({"translate", map, "--query", "classname = 'nothing'", "--delta", "8,0,0"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral("[]"));
  }

  SECTION("fails if a brush would leave the world bounds")
  {
    const auto result =
      runCli({"translate", map, "--target", "layer:0/brush:1", "--delta", "100000,0,0"});
    checkError(result, "op_failed", 1, 0);
    CHECK(fixture.unchanged());
  }

  SECTION("refuses the world and faces")
  {
    const auto target = GENERATE("world", "layer:0/brush:0/face:0");
    CAPTURE(target);
    checkError(
      runCli({"translate", map, "--target", target, "--delta", "8,0,0"}),
      "wrong_node_kind",
      1,
      0);
    CHECK(fixture.unchanged());
  }

  SECTION("--help describes the arguments")
  {
    const auto result = runCli({"translate", "--help"});
    CHECK(result.exitCode == 0);
    CHECK_THAT(result.out, ContainsSubstring("--delta <x,y,z>"));
    CHECK_THAT(result.out, ContainsSubstring("--uv-lock [true|false]"));
    CHECK_THAT(
      result.out,
      ContainsSubstring(
        "(accepts: layer, group, point_entity, brush_entity, brush, patch)"));
  }
}

TEST_CASE("rotate")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("rotates about the center of the targets' bounds by default")
  {
    runJson(
      {"rotate", map, "--target", "layer:0/brush:1", "--axis", "z", "--degrees", "90"});
    CHECK(
      aabbOf(map, "layer:0/brush:1")
      == parseJsonLiteral(R"({"min": [-120, -8, 0], "max": [8, 8, 128]})"));
  }

  SECTION("rotates counterclockwise about an axis through the given center")
  {
    runJson(
      {"rotate",
       map,
       "--target",
       "layer:0/brush:1",
       "--axis",
       "z",
       "--degrees",
       "90",
       "--center",
       "0,0,0"});
    CHECK(
      aabbOf(map, "layer:0/brush:1")
      == parseJsonLiteral(R"({"min": [-64, -64, 0], "max": [64, -48, 128]})"));
  }

  SECTION("updates the angle of point entities")
  {
    runJson(
      {"rotate", map, "--target", "layer:0/entity:0", "--axis", "z", "--degrees", "90"});
    const auto entity = show(map, "layer:0/entity:0");
    CHECK(entity["properties"]["angle"] == Json{"90"});
    CHECK(entity["origin"] == parseJsonLiteral("[0, 80, 24]"));
  }

  SECTION("an entity whose brushes all move is modified with them")
  {
    const auto json = runJson(
      {"rotate",
       map,
       "--target",
       "layer:0/entity:1/brush:0",
       "--axis",
       "z",
       "--degrees",
       "90"});
    CHECK(
      json["ops"][0]["modified"]
      == parseJsonLiteral(R"(["layer:0/entity:1/brush:0", "layer:0/entity:1"])"));
    CHECK(show(map, "layer:0/entity:1")["properties"]["angle"] == Json{"90"});
  }

  SECTION("refuses a malformed axis")
  {
    checkError(
      runCli(
        {"rotate", map, "--target", "layer:0/brush:1", "--axis", "w", "--degrees", "90"}),
      "invalid_argument",
      2);
    checkError(
      fixture.apply(
        R"([{"op": "rotate", "target": "layer:0/brush:1", "axis": [0, 0, 1], "degrees": 90}])"),
      "invalid_argument",
      2,
      0);
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("scale")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("scales the targets to new bounds")
  {
    const auto json = parseOutput(fixture.apply(R"([{
      "op": "scale", "target": "layer:0/brush:1",
      "to": {"min": [0, 0, 0], "max": [32, 64, 64]}
    }])"));
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/brush:1"])"));
    CHECK(
      aabbOf(map, "layer:0/brush:1")
      == parseJsonLiteral(R"({"min": [0, 0, 0], "max": [32, 64, 64]})"));
  }

  SECTION("the subcommand takes the new bounds as six numbers")
  {
    runJson({"scale", map, "--target", "layer:0/brush:1", "--to", "0,0,0,32,64,64"});
    CHECK(
      aabbOf(map, "layer:0/brush:1")
      == parseJsonLiteral(R"({"min": [0, 0, 0], "max": [32, 64, 64]})"));
  }

  SECTION("scales by factors about the center of the targets' bounds by default")
  {
    runJson({"scale", map, "--target", "layer:0/brush:1", "--factors", "2,1,0.5"});
    CHECK(
      aabbOf(map, "layer:0/brush:1")
      == parseJsonLiteral(R"({"min": [-72, -64, 32], "max": [-40, 64, 96]})"));
  }

  SECTION("scales by factors about the given center")
  {
    runJson(
      {"scale",
       map,
       "--target",
       "layer:0/brush:1",
       "--factors",
       "2,1,1",
       "--center",
       "-64,0,0"});
    CHECK(
      aabbOf(map, "layer:0/brush:1")
      == parseJsonLiteral(R"({"min": [-64, -64, 0], "max": [-32, 64, 128]})"));
  }

  SECTION("refuses malformed arguments")
  {
    const auto args = GENERATE(
      R"("factors": [2, 1, 1], "to": {"min": [0, 0, 0], "max": [8, 8, 8]})",
      R"("center": [0, 0, 0])",
      R"("to": {"min": [0, 0, 0], "max": [8, 8, 8]}, "center": [0, 0, 0])",
      R"("to": {"min": [0, 0, 0], "max": [0, 8, 8]})",
      R"("to": {"min": [0, 0, 0]})",
      R"("factors": [2, 0, 1])");
    CAPTURE(args);
    checkError(
      fixture.apply(
        fmt::format(R"([{{"op": "scale", "target": "layer:0/brush:1", {}}}])", args)),
      "invalid_argument",
      2,
      0);
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("flip")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("mirrors along an axis through the center of the targets' bounds by default")
  {
    // the door and the light: bounds from 16 -16 0 to 108.1234 16 96 with the light's
    // definition box, whose center is at x 62.0617
    const auto json = parseOutput(fixture.apply(R"([{
      "op": "flip", "target": ["layer:0/entity:1", "layer:0/entity:2"], "axis": "x"
    }])"));
    CHECK(
      json["ops"][0]["modified"]
      == parseJsonLiteral(R"(["layer:0/entity:1", "layer:0/entity:2"])"));
    CHECK(show(map, "layer:0/entity:2")["origin"] == parseJsonLiteral("[24, 0, 64]"));
    CHECK(
      aabbOf(map, "layer:0/entity:1/brush:0")
      == parseJsonLiteral(R"({"min": [76.123, -16, 0], "max": [108.123, 16, 96]})"));
  }

  SECTION("mirrors along an axis through the given center")
  {
    runJson(
      {"flip", map, "--target", "layer:0/brush:1", "--axis", "x", "--center", "0,0,0"});
    CHECK(
      aabbOf(map, "layer:0/brush:1")
      == parseJsonLiteral(R"({"min": [48, -64, 0], "max": [64, 64, 128]})"));
  }
}

} // namespace tb::cli
