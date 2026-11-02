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

/** The node at the given path, as show reports it. */
Json showNode(const std::string& map, const std::string& path)
{
  return runJson({"show", map, path})["node"];
}

} // namespace

TEST_CASE("create_point_entity")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("creates a point entity at the origin with the given properties")
  {
    const auto json = runJson(
      {"create_point_entity",
       map,
       "--classname",
       "light",
       "--origin",
       "32,0,64",
       "--properties",
       R"({"light": "200", "wait": "2"})",
       "--as",
       "$lamp"});
    CHECK(json["ops"][0] == parseJsonLiteral(R"({
      "ok": true,
      "created": ["layer:0/entity:3"],
      "modified": [],
      "removed": [],
      "warnings": [],
      "issues_introduced": []
    })"));
    CHECK(json["bindings"] == parseJsonLiteral(R"({"$lamp": "layer:0/entity:3"})"));

    const auto entity = showNode(map, "layer:0/entity:3");
    CHECK(entity["origin"] == parseJsonLiteral("[32, 0, 64]"));
    CHECK(entity["properties"] == parseJsonLiteral(R"({
      "classname": "light", "origin": "32 0 64", "light": "200", "wait": "2"
    })"));
    CHECK(entity["definition"] == Json{"found"});
  }

  SECTION("creates the entity in a group")
  {
    auto groups = EditFixture{"maps/targets.map"};
    const auto json = parseOutput(groups.apply(R"([{
      "op": "create_point_entity", "classname": "info_null", "origin": [0, 0, 0],
      "parent": "layer:0/group:0"
    }])"));
    CHECK(
      json["ops"][0]["created"] == parseJsonLiteral(R"(["layer:0/group:0/entity:0"])"));
  }

  SECTION("refuses a parent that cannot hold entities")
  {
    const auto parent = GENERATE("world", "layer:0/entity:1", "layer:0/brush:0");
    CAPTURE(parent);
    checkError(
      runCli(
        {"create_point_entity",
         map,
         "--classname",
         "light",
         "--origin",
         "0,0,0",
         "--parent",
         parent}),
      "wrong_node_kind",
      1,
      0);
    CHECK(fixture.unchanged());
  }

  SECTION("refuses classes that are not point classes of the FGD")
  {
    using Case = std::tuple<std::string, std::string>;
    const auto [classname, message] = GENERATE(
      Case{"no_such_class", "the FGD does not define \"no_such_class\""},
      Case{"func_door", "\"func_door\" is a brush class, not a point class"});
    CAPTURE(classname);

    const auto args = std::vector<std::string>{
      "create_point_entity", map, "--classname", classname, "--origin", "0,0,0"};
    const auto result = runCli(args);
    checkError(result, "undefined_class", 1, 0);
    CHECK_THAT(
      parseOutput(result)["error"]["message"].asString(), ContainsSubstring(message));
    CHECK(fixture.unchanged());

    SECTION("unless allow_undefined is set")
    {
      auto allowUndefined = args;
      allowUndefined.emplace_back("--allow-undefined");
      CHECK(
        runJson(allowUndefined)["ops"][0]["created"]
        == parseJsonLiteral(R"(["layer:0/entity:3"])"));
      CHECK(showNode(map, "layer:0/entity:3")["classname"] == Json{classname});
    }

    SECTION("--allow-undefined takes an explicit value")
    {
      auto allowUndefined = args;
      allowUndefined.emplace_back("--allow-undefined");
      allowUndefined.emplace_back("false");
      checkError(runCli(allowUndefined), "undefined_class", 1, 0);
    }
  }

  SECTION("refuses malformed arguments")
  {
    const auto [classname, properties] = GENERATE(
      std::tuple{"", "{}"},
      std::tuple{"worldspawn", "{}"},
      std::tuple{"light", R"({"origin": "0 0 0"})"},
      std::tuple{"light", R"({"classname": "light"})"},
      std::tuple{"light", R"({"": "x"})"},
      std::tuple{"light", R"({"light": 200})"},
      std::tuple{"light", "not json"});
    CAPTURE(classname, properties);
    checkError(
      runCli(
        {"create_point_entity",
         map,
         "--classname",
         classname,
         "--origin",
         "0,0,0",
         "--properties",
         properties}),
      "invalid_argument",
      2);
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("create_brush_entity")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("moves world brushes into a new brush entity")
  {
    const auto json = runJson(
      {"create_brush_entity",
       map,
       "--classname",
       "func_wall",
       "--brushes",
       "layer:0/brush:1",
       "--properties",
       R"({"message": "wall"})",
       "--as",
       "$wall"});
    CHECK(json["ops"][0] == parseJsonLiteral(R"({
      "ok": true,
      "created": ["layer:0/entity:3"],
      "modified": ["layer:0/entity:3/brush:0"],
      "removed": [],
      "warnings": [],
      "issues_introduced": []
    })"));
    CHECK(json["bindings"] == parseJsonLiteral(R"({"$wall": "layer:0/entity:3"})"));
    CHECK(showNode(map, "layer:0/entity:3")["properties"] == parseJsonLiteral(R"({
      "classname": "func_wall", "message": "wall"
    })"));
    CHECK(brushCount(map) == 3);
  }

  SECTION("a brush entity stands for its brushes, and is deleted when left empty")
  {
    const auto json = parseOutput(fixture.apply(R"([{
      "op": "create_brush_entity", "classname": "func_wall", "brushes": "layer:0/entity:1"
    }])"));
    CHECK(json["ops"][0]["created"] == parseJsonLiteral(R"(["layer:0/entity:2"])"));
    CHECK(
      json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/entity:2/brush:0"])"));
    CHECK(json["ops"][0]["removed"] == parseJsonLiteral(R"(["layer:0/entity:1"])"));
    // the door's properties are not carried over
    CHECK(
      showNode(map, "layer:0/entity:2")["properties"]
      == parseJsonLiteral(R"({"classname": "func_wall"})"));
  }

  SECTION("an entity that keeps some of its brushes is modified")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "create_box", "min": [16, -16, 96], "max": [48, 16, 112], "material": "a",
       "parent": "layer:0/entity:1", "as": "$box"},
      {"op": "create_brush_entity", "classname": "func_wall", "brushes": "$box"}
    ])"));
    CHECK(
      json["ops"][1]["modified"]
      == parseJsonLiteral(R"(["layer:0/entity:3/brush:0", "layer:0/entity:1"])"));
    CHECK(json["ops"][1]["removed"] == Json::array());
  }

  SECTION("creates the entity in the group of the first brush")
  {
    auto groups = EditFixture{"maps/targets.map"};
    const auto json = parseOutput(groups.apply(R"([{
      "op": "create_brush_entity", "classname": "func_wall",
      "brushes": ["layer:0/group:0/brush:1", "layer:0/brush:0"]
    }])"));
    CHECK(
      json["ops"][0]["created"] == parseJsonLiteral(R"(["layer:0/group:0/entity:0"])"));
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"([
      "layer:0/group:0/entity:0/brush:0", "layer:0/group:0/entity:0/brush:1"
    ])"));
  }

  SECTION("refuses classes that are not brush classes of the FGD")
  {
    const auto classname = GENERATE("light", "no_such_class");
    CAPTURE(classname);

    const auto args = std::vector<std::string>{
      "create_brush_entity",
      map,
      "--classname",
      classname,
      "--brushes",
      "layer:0/brush:0"};
    checkError(runCli(args), "undefined_class", 1, 0);
    CHECK(fixture.unchanged());

    auto allowUndefined = args;
    allowUndefined.emplace_back("--allow-undefined");
    CHECK(
      runJson(allowUndefined)["ops"][0]["created"]
      == parseJsonLiteral(R"(["layer:0/entity:3"])"));
  }

  SECTION("refuses targets without brushes")
  {
    const auto brushes = GENERATE("world", "layer:0/entity:0", "layer:0/brush:0/face:0");
    CAPTURE(brushes);
    checkError(
      runCli(
        {"create_brush_entity", map, "--classname", "func_wall", "--brushes", brushes}),
      "wrong_node_kind",
      1,
      0);

    checkError(
      fixture.apply(R"([{
        "op": "create_brush_entity", "classname": "func_wall",
        "brushes": {"query": "classname is \"no_such_class\""}
      }])"),
      "wrong_node_kind",
      1,
      0);
    CHECK(fixture.unchanged());
  }

  SECTION("refuses a classname in the properties")
  {
    checkError(
      fixture.apply(R"([{
        "op": "create_brush_entity", "classname": "func_wall", "brushes": "layer:0/brush:0",
        "properties": {"classname": "func_door"}
      }])"),
      "invalid_argument",
      2,
      0);
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("move_to_world")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("moves the brushes of a brush entity to the world and deletes it")
  {
    const auto target = GENERATE("layer:0/entity:1", "layer:0/entity:1/brush:0");
    CAPTURE(target);
    const auto json = runJson({"move_to_world", map, "--target", target});
    CHECK(json["ops"][0] == parseJsonLiteral(R"({
      "ok": true,
      "created": [],
      "modified": ["layer:0/brush:2"],
      "removed": ["layer:0/entity:1"],
      "warnings": [],
      "issues_introduced": []
    })"));
    CHECK(showNode(map, "layer:0/brush:2")["entity"] == Json{"world"});
  }

  SECTION("an entity that keeps some of its brushes is modified")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "create_box", "min": [16, -16, 96], "max": [48, 16, 112], "material": "a",
       "parent": "layer:0/entity:1", "as": "$box"},
      {"op": "move_to_world", "target": "$box"}
    ])"));
    CHECK(
      json["ops"][1]["modified"]
      == parseJsonLiteral(R"(["layer:0/brush:2", "layer:0/entity:1"])"));
    CHECK(json["ops"][1]["removed"] == Json::array());
  }

  SECTION("a brush in the world stays where it is")
  {
    const auto json = runJson({"move_to_world", map, "--target", "layer:0/brush:0"});
    CHECK(json["ops"][0]["modified"] == Json::array());
    CHECK(json["ops"][0]["removed"] == Json::array());
  }

  SECTION("moves brushes into the group that holds the entity")
  {
    auto groups = EditFixture{"maps/targets.map"};
    const auto json = parseOutput(groups.apply(R"([
      {"op": "create_brush_entity", "classname": "func_wall",
       "brushes": "layer:0/group:0/brush:0", "as": "$wall"},
      {"op": "move_to_world", "target": "$wall"}
    ])"));
    CHECK(
      json["ops"][1]["modified"] == parseJsonLiteral(R"(["layer:0/group:0/brush:1"])"));
    // the entity was created in the batch, so it has no path in the input rev
    CHECK(json["ops"][1]["removed"] == Json::array());
    CHECK(json["bindings"] == Json::object());
  }

  SECTION("refuses nodes that are not brush entities or brushes")
  {
    const auto target = GENERATE("world", "layer:0", "layer:0/entity:0");
    CAPTURE(target);
    checkError(
      runCli({"move_to_world", map, "--target", target}), "wrong_node_kind", 1, 0);
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("set_property")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("sets a property of entities and the world")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "set_property", "target": ["world", "layer:0/entity:2"],
       "key": "message", "value": "hello"}
    ])"));
    CHECK(
      json["ops"][0]["modified"] == parseJsonLiteral(R"(["world", "layer:0/entity:2"])"));
    CHECK(showNode(map, "world")["properties"]["message"] == Json{"hello"});
    CHECK(showNode(map, "layer:0/entity:2")["properties"]["message"] == Json{"hello"});
  }

  SECTION("replaces a value, and leaves an entity that has the value unmodified")
  {
    CHECK(
      runJson(
        {"set_property",
         map,
         "--target",
         "layer:0/entity:1",
         "--key",
         "targetname",
         "--value",
         "door2"})["ops"][0]["modified"]
      == parseJsonLiteral(R"(["layer:0/entity:1"])"));
    CHECK(
      runJson(
        {"set_property",
         map,
         "--target",
         "layer:0/entity:1",
         "--key",
         "targetname",
         "--value",
         "door2"})["ops"][0]["modified"]
      == Json::array());
  }

  SECTION("reports the issues the new value introduces")
  {
    const auto json = runJson(
      {"set_property",
       map,
       "--target",
       "layer:0/entity:2",
       "--key",
       "targetname",
       "--value",
       "lamp"});
    const auto& issues = json["ops"][0]["issues_introduced"];
    REQUIRE(issues.size() == 1);
    CHECK(issues[0]["type"] == Json{"link_source"});
    CHECK(issues[0]["node"] == Json{"layer:0/entity:2"});
  }

  SECTION("an edit in a linked group reaches its linked copies")
  {
    auto linked = EditFixture{"maps/linked.map"};
    const auto json = runJson(
      {"set_property",
       linked.map,
       "--target",
       "layer:0/group:0/entity:0",
       "--key",
       "light",
       "--value",
       "250"});
    // TODO: ticket 13 adds the linked copies to modified (instances_affected)
    CHECK(
      json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/group:0/entity:0"])"));
    CHECK(
      showNode(linked.map, "layer:0/group:1/entity:0")["properties"]["light"]
      == Json{"250"});
  }

  SECTION("origin moves a point entity")
  {
    runJson(
      {"set_property",
       map,
       "--target",
       "layer:0/entity:2",
       "--key",
       "origin",
       "--value",
       "8 16 32"});
    CHECK(showNode(map, "layer:0/entity:2")["origin"] == parseJsonLiteral("[8, 16, 32]"));
  }

  SECTION("refuses malformed arguments")
  {
    const auto [key, value] = GENERATE(
      std::tuple{"", "x"},
      std::tuple{"classname", "info_null"},
      std::tuple{"origin", "1 2"},
      std::tuple{"origin", "a b c"},
      std::tuple{"angle", "east"});
    CAPTURE(key, value);
    checkError(
      runCli(
        {"set_property",
         map,
         "--target",
         "layer:0/entity:2",
         "--key",
         key,
         "--value",
         value}),
      "invalid_argument",
      2);
    CHECK(fixture.unchanged());
  }

  SECTION("refuses brushes, faces, layers and groups")
  {
    const auto target = GENERATE("layer:0/brush:0", "layer:0/brush:0/face:0", "layer:0");
    CAPTURE(target);
    checkError(
      runCli({"set_property", map, "--target", target, "--key", "a", "--value", "b"}),
      "wrong_node_kind",
      1,
      0);
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("remove_property")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("removes a property")
  {
    const auto json = runJson(
      {"remove_property", map, "--target", "layer:0/entity:1", "--key", "targetname"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/entity:1"])"));
    CHECK(json["ops"][0]["warnings"] == Json::array());
    CHECK(
      showNode(map, "layer:0/entity:1")["properties"]
      == parseJsonLiteral(R"({"classname": "func_door"})"));
  }

  SECTION("warns if a target does not have the property")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "remove_property", "target": ["layer:0/entity:1", "layer:0/entity:2"],
       "key": "targetname"}
    ])"));
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/entity:1"])"));
    CHECK(json["ops"][0]["warnings"] == parseJsonLiteral(R"(["property_not_found"])"));
  }

  SECTION("refuses an empty key, classname and brushes")
  {
    const auto key = GENERATE("", "classname");
    CAPTURE(key);
    checkError(
      runCli({"remove_property", map, "--target", "layer:0/entity:1", "--key", key}),
      "invalid_argument",
      2);
    checkError(
      runCli({"remove_property", map, "--target", "layer:0/brush:0", "--key", "a"}),
      "wrong_node_kind",
      1,
      0);
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("rename_property")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("renames a property, keeping its value and place")
  {
    const auto json = runJson(
      {"rename_property",
       map,
       "--target",
       "layer:0/entity:1",
       "--from",
       "targetname",
       "--to",
       "target"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/entity:1"])"));
    CHECK(json["ops"][0]["warnings"] == Json::array());
    CHECK(
      showNode(map, "layer:0/entity:1")["properties"]
      == parseJsonLiteral(R"({"classname": "func_door", "target": "door1"})"));
  }

  SECTION("warns if a target does not have the property")
  {
    const auto json = runJson(
      {"rename_property",
       map,
       "--target",
       "layer:0/entity:2",
       "--from",
       "targetname",
       "--to",
       "target"});
    CHECK(json["ops"][0]["modified"] == Json::array());
    CHECK(json["ops"][0]["warnings"] == parseJsonLiteral(R"(["property_not_found"])"));
  }

  SECTION("refuses to overwrite a property")
  {
    const auto result = runCli(
      {"rename_property",
       map,
       "--target",
       "world",
       "--from",
       "wad",
       "--to",
       "mapversion"});
    checkError(result, "op_failed", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{"world"});
    CHECK(fixture.unchanged());
  }

  SECTION("refuses malformed keys")
  {
    const auto [from, to] = GENERATE(
      std::tuple{"", "a"},
      std::tuple{"a", ""},
      std::tuple{"a", "a"},
      std::tuple{"classname", "a"},
      std::tuple{"a", "classname"});
    CAPTURE(from, to);
    checkError(
      runCli(
        {"rename_property",
         map,
         "--target",
         "layer:0/entity:1",
         "--from",
         from,
         "--to",
         to}),
      "invalid_argument",
      2);
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("set_spawnflag")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  const auto spawnflagsOf = [&](const std::string& path) {
    return showNode(map, path)["properties"]["spawnflags"];
  };

  SECTION("sets a spawnflag by its name in the FGD, ignoring case")
  {
    const auto json = runJson(
      {"set_spawnflag",
       map,
       "--target",
       "layer:0/entity:1",
       "--flag",
       "starts open",
       "--on"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/entity:1"])"));
    CHECK(spawnflagsOf("layer:0/entity:1") == Json{"1"});
    CHECK(
      showNode(map, "layer:0/entity:1")["spawnflags"]
      == parseJsonLiteral(R"(["Starts Open"])"));
  }

  SECTION("sets and clears a spawnflag by its value")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "set_spawnflag", "target": "layer:0/entity:1", "flag": 32, "on": true},
      {"op": "set_spawnflag", "target": "layer:0/entity:1", "flag": 256, "on": true},
      {"op": "set_spawnflag", "target": "layer:0/entity:1", "flag": 32, "on": false}
    ])"));
    CHECK(json["ops"][2]["modified"] == parseJsonLiteral(R"(["layer:0/entity:1"])"));
    CHECK(spawnflagsOf("layer:0/entity:1") == Json{"256"});
  }

  SECTION("clearing a flag that is not set changes nothing")
  {
    const auto json = runJson(
      {"set_spawnflag",
       map,
       "--target",
       "layer:0/entity:1",
       "--flag",
       "1",
       "--on",
       "false"});
    CHECK(json["ops"][0]["modified"] == Json::array());
    CHECK(spawnflagsOf("layer:0/entity:1").isNull());
  }

  SECTION("refuses a name that the target's class does not define")
  {
    const auto result = runCli(
      {"set_spawnflag",
       map,
       "--target",
       "layer:0/entity:0",
       "--flag",
       "Starts Open",
       "--on"});
    checkError(result, "op_failed", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/entity:0"});
    CHECK(fixture.unchanged());
  }

  SECTION("refuses values that are not a single flag")
  {
    const auto flag = GENERATE("0", "3", "2147483648");
    CAPTURE(flag);
    checkError(
      runCli(
        {"set_spawnflag", map, "--target", "layer:0/entity:1", "--flag", flag, "--on"}),
      "invalid_argument",
      2);
    CHECK(fixture.unchanged());
  }

  SECTION("refuses brushes")
  {
    checkError(
      runCli(
        {"set_spawnflag", map, "--target", "layer:0/brush:0", "--flag", "1", "--on"}),
      "wrong_node_kind",
      1,
      0);
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
