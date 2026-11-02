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
#include <tuple>
#include <vector>

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
      "ok": true,
      "created": [],
      "modified": [],
      "removed": ["layer:0/brush:0"],
      "warnings": [],
      "issues_introduced": []
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

namespace
{

/** The nodes of the issues of the given type that verify lists for the given map. */
std::vector<std::string> issueNodes(const std::string& map, const std::string& type)
{
  auto result = std::vector<std::string>{};
  const auto json = runJson({"verify", map, "--type", type});
  for (const auto& issue : json["issues"]["items"].asArray())
  {
    result.push_back(issue["node"].asString());
  }
  return result;
}

} // namespace

TEST_CASE("fix")
{
  SECTION("on maps/verify.map")
  {
    auto fixture = EditFixture{"maps/verify.map"};
    const auto& map = fixture.map;

    SECTION("fixes the issues of a type on the target")
    {
      const auto json = runJson(
        {"fix",
         map,
         "--issue-type",
         "empty_property_value",
         "--fix",
         "delete_property",
         "--target",
         "layer:0/entity:1"});
      // the light keeps its other issues, which are not new
      CHECK(json["ops"] == parseJsonLiteral(R"([{
        "ok": true,
        "created": [],
        "modified": ["layer:0/entity:1"],
        "removed": [],
        "warnings": [],
        "issues_introduced": []
      }])"));
      CHECK(issueNodes(map, "empty_property_value") == std::vector<std::string>{"world"});
      CHECK(
        issueNodes(map, "link_target") == std::vector<std::string>{"layer:0/entity:1"});
    }

    SECTION("without a target, fixes every issue of the type")
    {
      const auto json = parseOutput(fixture.apply(R"([
        {"op": "fix", "issue_type": "empty_property_value", "fix": "delete_property"}
      ])"));
      CHECK(
        json["ops"][0]["modified"]
        == parseJsonLiteral(R"(["world", "layer:0/entity:1"])"));
      CHECK(issueNodes(map, "empty_property_value").empty());
    }

    SECTION("delete_objects removes the nodes with the issues")
    {
      const auto json =
        runJson({"fix", map, "--issue-type", "empty_group", "--fix", "delete_objects"});
      CHECK(json["ops"][0]["modified"] == Json::array());
      CHECK(json["ops"][0]["removed"] == parseJsonLiteral(R"(["layer:0/group:0"])"));
      CHECK(issueNodes(map, "empty_group").empty());
    }

    SECTION("a face target fixes the issue on that face only")
    {
      const auto json = runJson(
        {"fix",
         map,
         "--issue-type",
         "invalid_uv_scale",
         "--fix",
         "reset_uv_scale",
         "--target",
         "layer:0/entity:2/brush:0/face:0"});
      CHECK(
        json["ops"][0]["modified"]
        == parseJsonLiteral(R"(["layer:0/entity:2/brush:0"])"));
      CHECK(issueNodes(map, "invalid_uv_scale").empty());
    }

    SECTION("a target without issues of the type fixes nothing and warns")
    {
      const auto target = GENERATE("layer:0/brush:0", "layer:0/entity:2/brush:0/face:1");
      CAPTURE(target);
      const auto json = runJson(
        {"fix",
         map,
         "--issue-type",
         "invalid_uv_scale",
         "--fix",
         "reset_uv_scale",
         "--target",
         target});
      CHECK(json["ops"][0]["modified"] == Json::array());
      CHECK(json["ops"][0]["warnings"] == parseJsonLiteral(R"(["nothing_to_fix"])"));
      CHECK(issueNodes(map, "invalid_uv_scale").size() == 1);
    }

    SECTION("snap_vertices snaps the vertices of the brushes with the issue")
    {
      const auto json = runJson(
        {"fix", map, "--issue-type", "non_integer_vertices", "--fix", "snap_vertices"});
      CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/brush:1"])"));
      CHECK(issueNodes(map, "non_integer_vertices").empty());
    }

    SECTION("an unknown issue type or fix is refused before anything runs")
    {
      using Case = std::tuple<std::string, std::string, std::string>;
      const auto [type, fix, message] = GENERATE(
        Case{"no_such_type", "delete_objects", "unknown issue type \"no_such_type\""},
        Case{
          "long_property_value",
          "delete_objects",
          "unknown fix \"delete_objects\" for issue type \"long_property_value\", which "
          "has \"delete_property\", \"truncate_property_value\""},
        Case{
          "mixed_brush_contents",
          "delete_objects",
          "issue type \"mixed_brush_contents\" has no quick-fixes"});
      CAPTURE(type, fix);

      const auto subcommand = runCli({"fix", map, "--issue-type", type, "--fix", fix});
      checkError(subcommand, "invalid_argument", 2);
      CHECK_THAT(
        parseOutput(subcommand)["error"]["message"].asString(),
        ContainsSubstring(message));

      const auto batch = fixture.apply(
        fmt::format(
          R"([{{"op": "delete", "target": "layer:0/brush:0"}},
            {{"op": "fix", "issue_type": "{}", "fix": "{}"}}])",
          type,
          fix));
      checkError(batch, "invalid_argument", 2, 1);
      CHECK(fixture.unchanged());
    }
  }

  SECTION("on maps/fix.map")
  {
    auto fixture = EditFixture{"maps/fix.map"};
    const auto& map = fixture.map;

    SECTION("move_brushes_to_world reports the brushes it moved as modified")
    {
      const auto json = runJson(
        {"fix",
         map,
         "--issue-type",
         "point_entity_with_brushes",
         "--fix",
         "move_brushes_to_world"});
      CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/brush:1"])"));
      // the light is left without brushes, which removes it
      CHECK(json["ops"][0]["removed"] == parseJsonLiteral(R"(["layer:0/entity:1"])"));
      CHECK(issueNodes(map, "point_entity_with_brushes").empty());
    }

    SECTION("issues on locked nodes are left out unless --allow-locked")
    {
      const auto args = std::vector<std::string>{
        "fix", map, "--issue-type", "empty_property_value", "--fix", "delete_property"};

      const auto json = runJson(args);
      CHECK(json["ops"][0]["warnings"] == parseJsonLiteral(R"(["nothing_to_fix"])"));
      CHECK(issueNodes(map, "empty_property_value").size() == 1);

      auto allowLocked = args;
      allowLocked.emplace_back("--allow-locked");
      CHECK(
        runJson(allowLocked)["ops"][0]["modified"]
        == parseJsonLiteral(R"(["layer:1/entity:0"])"));
      CHECK(issueNodes(map, "empty_property_value").empty());
    }

    SECTION("a locked target is refused")
    {
      checkError(
        runCli(
          {"fix",
           map,
           "--issue-type",
           "empty_property_value",
           "--fix",
           "delete_property",
           "--target",
           "layer:1/entity:0"}),
        "locked",
        1,
        0);
      CHECK(fixture.unchanged());
    }
  }

  SECTION("--help describes the arguments")
  {
    const auto result = runCli({"fix", "--help"});
    CHECK_THAT(result.out, ContainsSubstring("--issue-type <string>"));
    CHECK_THAT(result.out, ContainsSubstring("--fix <string>"));
    CHECK_THAT(result.out, ContainsSubstring("--target <path>"));
  }
}

} // namespace tb::cli
