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

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::cli
{
using namespace Catch::Matchers;

TEST_CASE("create_box")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("creates an axis-aligned box in the default layer")
  {
    const auto json = runJson(
      {"create_box",
       map,
       "--min",
       "0,0,0",
       "--max",
       "64 32 16",
       "--material",
       "floor",
       "--as",
       "$box"});
    CHECK(json["schema"] == Json{"tbcli.apply/1"});
    CHECK(json["ops"][0]["created"] == parseJsonLiteral(R"(["layer:0/brush:2"])"));
    CHECK(json["bindings"] == parseJsonLiteral(R"({"$box": "layer:0/brush:2"})"));
    CHECK(json["rev"] == Json{revOf(map)});

    const auto brush = runJson({"show", map, "layer:0/brush:2"})["node"];
    CHECK(
      brush["aabb"] == parseJsonLiteral(R"({"min": [0, 0, 0], "max": [64, 32, 16]})"));
    CHECK(brush["axial_box"] == Json{true});
    CHECK(brush["textures"] == parseJsonLiteral(R"(["floor"])"));
    CHECK(brush["entity"] == Json{"world"});
  }

  SECTION("the subcommand and the batch give the same result")
  {
    const auto subcommand = runJson(
      {"create_box",
       map,
       "--min=0,0,0",
       "--max=64,32,16",
       "--material=floor",
       "--dry-run"});
    const auto batch =
      parseOutput(fixture.apply(fmt::format("[{}]", CreateBoxOp), {"--dry-run"}));
    CHECK(subcommand == batch);
  }

  SECTION("creates the box in a brush entity")
  {
    const auto json = parseOutput(fixture.apply(R"([{
      "op": "create_box", "min": [16, 16, 0], "max": [32, 32, 8], "material": "door",
      "parent": "layer:0/entity:1"
    }])"));
    CHECK(
      json["ops"][0]["created"] == parseJsonLiteral(R"(["layer:0/entity:1/brush:1"])"));
    CHECK(
      runJson({"show", map, "layer:0/entity:1/brush:1"})["node"]["entity"]
      == Json{"layer:0/entity:1"});
  }

  SECTION("refuses a parent that cannot hold brushes")
  {
    const auto parent =
      GENERATE("world", "layer:0/entity:0", "layer:0/brush:0", "layer:0/brush:0/face:0");
    CAPTURE(parent);
    const auto result = fixture.apply(
      fmt::format(
        R"([{{"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": "a", "parent": "{}"}}])",
        parent));
    checkError(result, "wrong_node_kind", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{parent});
    CHECK(fixture.unchanged());
  }

  SECTION("refuses malformed flags")
  {
    checkError(
      runCli({"create_box", map, "--min", "0,0", "--max", "8,8,8", "--material", "a"}),
      "invalid_argument",
      2);
    checkError(
      runCli({"create_box", map, "--max", "8,8,8", "--material", "a"}),
      "invalid_argument",
      2);
    checkError(
      runCli(
        {"create_box",
         map,
         "--min",
         "0,0,0",
         "--max",
         "8,8,8",
         "--material",
         "a",
         "--target",
         "x"}),
      "invalid_argument",
      2);
    checkError(
      runCli(
        {"create_box",
         map,
         "--min",
         "0,0,0",
         "--min",
         "0,0,0",
         "--max",
         "8,8,8",
         "--material",
         "a"}),
      "invalid_argument",
      2);
    CHECK(fixture.unchanged());
  }

  SECTION("--help describes the arguments from the op definition")
  {
    const auto result = runCli({"create_box", "--help"});
    CHECK(result.exitCode == 0);
    CHECK_THAT(result.out, StartsWith("usage: tbcli create_box <map>"));
    CHECK_THAT(result.out, ContainsSubstring("--min <x,y,z>"));
    CHECK_THAT(result.out, ContainsSubstring("--parent <path>"));
    CHECK_THAT(result.out, ContainsSubstring("(accepts: layer, group, brush_entity)"));
    CHECK_THAT(result.out, ContainsSubstring("(default: layer:0)"));
    CHECK_THAT(result.out, ContainsSubstring("--as <$name>"));
    CHECK_THAT(result.out, ContainsSubstring("--dry-run"));
  }
}

TEST_CASE("delete")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("deletes a brush")
  {
    const auto json = runJson({"delete", map, "--target", "layer:0/brush:0"});
    CHECK(json["ops"] == parseJsonLiteral(R"([{
      "ok": true, "created": [], "modified": [], "removed": ["layer:0/brush:0"], "warnings": []
    }])"));
    CHECK(json["rev"] == Json{revOf(map)});
    CHECK(brushCount(map) == 2);
  }

  SECTION("deletes an entity with its brushes")
  {
    const auto json = runJson({"delete", map, "--target", "layer:0/entity:1"});
    CHECK(json["ops"][0]["removed"] == parseJsonLiteral(R"(["layer:0/entity:1"])"));
    CHECK(brushCount(map) == 2);
  }

  SECTION("an entity left without brushes is deleted with its last brush")
  {
    const auto json = runJson({"delete", map, "--target", "layer:0/entity:1/brush:0"});
    CHECK(
      json["ops"][0]["removed"]
      == parseJsonLiteral(R"(["layer:0/entity:1/brush:0", "layer:0/entity:1"])"));
    CHECK(runJson({"describe", map})["counts"]["entities"] == Json{2});
  }

  SECTION("--help describes the target forms")
  {
    const auto result = runCli({"delete", "--help"});
    CHECK_THAT(result.out, ContainsSubstring("--target <path>"));
    CHECK_THAT(result.out, ContainsSubstring("--query <ql>"));
    CHECK_THAT(result.out, ContainsSubstring("--expect <n | any>"));
    CHECK_THAT(result.out, ContainsSubstring("--allow-locked"));
  }

  SECTION("refuses nodes that cannot be deleted")
  {
    const auto target = GENERATE("world", "layer:0", "layer:0/brush:0/face:0");
    CAPTURE(target);
    checkError(runCli({"delete", map, "--target", target}), "wrong_node_kind", 1, 0);
    CHECK(fixture.unchanged());
  }
}

} // namespace tb::cli
