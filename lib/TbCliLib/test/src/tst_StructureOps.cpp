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

/*
 * maps/structure.map:
 * - layer:0/brush:0 (floor) and layer:0/brush:1 (wall, 128 0 0 to 160 32 32)
 * - layer:0/entity:0, a func_door with one brush, and layer:0/entity:1, a light
 * - layer:0/group:0 "Plain" with a brush
 * - layer:0/group:1 and layer:0/group:2 "Box", linked, each with a brush and a light;
 *   group:2 is group:1 moved by 256 on x
 * - layer:1 "Props" with a brush
 */

TEST_CASE("duplicate")
{
  auto fixture = EditFixture{"maps/structure.map"};
  const auto& map = fixture.map;

  SECTION("copies nodes and moves the copies by the offset")
  {
    const auto json = parseOutput(fixture.apply(R"([{
      "op": "duplicate", "target": ["layer:0/brush:1", "layer:0/group:0"],
      "offset": [0, 0, 256], "as": "$copies"
    }])"));
    CHECK(
      json["ops"][0]["created"]
      == parseJsonLiteral(R"(["layer:0/brush:2", "layer:0/group:3"])"));
    CHECK(
      json["bindings"]
      == parseJsonLiteral(R"({"$copies": ["layer:0/brush:2", "layer:0/group:3"]})"));

    CHECK(
      showNode(map, "layer:0/brush:2")["aabb"]
      == parseJsonLiteral(R"({"min": [128, 0, 256], "max": [160, 32, 288]})"));
    CHECK(
      showNode(map, "layer:0/brush:1")["aabb"]
      == parseJsonLiteral(R"({"min": [128, 0, 0], "max": [160, 32, 32]})"));
    CHECK(showNode(map, "layer:0/group:3")["name"] == Json{"Plain"});
  }

  SECTION("copies a brush of a brush entity with a new entity")
  {
    const auto json = runJson(
      {"duplicate", map, "--target", "layer:0/entity:0/brush:0", "--offset", "64,0,0"});
    CHECK(json["ops"][0]["created"] == parseJsonLiteral(R"(["layer:0/entity:2"])"));
    CHECK(showNode(map, "layer:0/entity:2")["classname"] == Json{"func_door"});
  }

  SECTION("a copy inside a linked group reaches its linked copies")
  {
    const auto json = runJson(
      {"duplicate", map, "--target", "layer:0/group:1/brush:0", "--offset", "0,0,64"});
    const auto& op = json["ops"][0];
    CHECK(op["created"] == parseJsonLiteral(R"(["layer:0/group:1/brush:1"])"));
    CHECK(op["modified"] == parseJsonLiteral(R"(["layer:0/group:2/brush:1"])"));
    CHECK(
      op["instances_affected"]
      == parseJsonLiteral(R"(["layer:0/group:1", "layer:0/group:2"])"));
    CHECK(
      showNode(map, "layer:0/group:2/brush:1")["aabb"]
      == parseJsonLiteral(R"({"min": [256, 200, 64], "max": [288, 232, 96]})"));
  }

  SECTION("refuses a layer")
  {
    checkError(
      fixture.apply(R"([{"op": "duplicate", "target": "layer:1", "offset": [0, 0, 0]}])"),
      "wrong_node_kind",
      1,
      0);
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("group")
{
  auto fixture = EditFixture{"maps/structure.map"};
  const auto& map = fixture.map;

  SECTION("groups nodes in the layer of the first")
  {
    const auto json = parseOutput(fixture.apply(R"([{
      "op": "group", "target": ["layer:0/brush:1", "layer:0/entity:1"], "name": "Lamp",
      "as": "$lamp"
    }])"));
    const auto& op = json["ops"][0];
    CHECK(op["created"] == parseJsonLiteral(R"(["layer:0/group:3"])"));
    CHECK(
      op["modified"]
      == parseJsonLiteral(R"(["layer:0/group:3/brush:0", "layer:0/group:3/entity:0"])"));
    CHECK(json["bindings"] == parseJsonLiteral(R"({"$lamp": "layer:0/group:3"})"));
    CHECK(showNode(map, "layer:0/group:3")["name"] == Json{"Lamp"});
  }

  SECTION("a brush of a brush entity brings its entity along")
  {
    const auto json =
      runJson({"group", map, "--target", "layer:0/entity:0/brush:0", "--name", "Door"});
    CHECK(
      json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/group:3/entity:0"])"));
  }

  SECTION("refuses an empty name")
  {
    checkError(
      runCli({"group", map, "--target", "layer:0/brush:1", "--name", ""}),
      "invalid_argument",
      2);
  }
}

TEST_CASE("ungroup")
{
  auto fixture = EditFixture{"maps/structure.map"};
  const auto& map = fixture.map;

  SECTION("moves a group's contents into its parent")
  {
    const auto json = runJson({"ungroup", map, "--target", "layer:0/group:0"});
    const auto& op = json["ops"][0];
    CHECK(op["removed"] == parseJsonLiteral(R"(["layer:0/group:0"])"));
    CHECK(op["modified"] == parseJsonLiteral(R"(["layer:0/brush:2"])"));
    CHECK(
      showNode(map, "layer:0/brush:2")["textures"] == parseJsonLiteral(R"(["crate"])"));
  }

  SECTION("a linked group is separated first")
  {
    const auto json = runJson({"ungroup", map, "--target", "layer:0/group:1"});
    const auto& op = json["ops"][0];
    CHECK(op["removed"] == parseJsonLiteral(R"(["layer:0/group:1"])"));
    CHECK(
      op["modified"] == parseJsonLiteral(R"(["layer:0/brush:2", "layer:0/entity:2"])"));
    CHECK(op.find("instances_affected") == nullptr);
    CHECK(showNode(map, "layer:0/group:1").find("link_id") == nullptr);
  }

  SECTION("refuses a brush")
  {
    checkError(
      runCli({"ungroup", map, "--target", "layer:0/brush:0"}), "wrong_node_kind", 1, 0);
  }
}

TEST_CASE("rename_group")
{
  auto fixture = EditFixture{"maps/structure.map"};
  const auto& map = fixture.map;

  SECTION("renames groups")
  {
    const auto json =
      runJson({"rename_group", map, "--target", "layer:0/group:0", "--name", "Crates"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/group:0"])"));
    CHECK(showNode(map, "layer:0/group:0")["name"] == Json{"Crates"});
  }

  SECTION("a group that has the name already is not modified")
  {
    const auto json =
      runJson({"rename_group", map, "--target", "layer:0/group:0", "--name", "Plain"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral("[]"));
  }
}

TEST_CASE("create_layer")
{
  auto fixture = EditFixture{"maps/structure.map"};
  const auto& map = fixture.map;

  SECTION("creates a layer below the others that later ops can fill")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "create_layer", "name": "Detail", "as": "$detail"},
      {"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": "trim",
       "parent": "$detail"}
    ])"));
    CHECK(json["ops"][0]["created"] == parseJsonLiteral(R"(["layer:2"])"));
    CHECK(json["ops"][1]["created"] == parseJsonLiteral(R"(["layer:2/brush:0"])"));
    CHECK(json["bindings"] == parseJsonLiteral(R"({"$detail": "layer:2"})"));

    const auto layers = runJson({"describe", map})["layers"]["items"];
    REQUIRE(layers.size() == 3);
    CHECK(layers[2]["name"] == Json{"Detail"});
  }
}

TEST_CASE("rename_layer")
{
  auto fixture = EditFixture{"maps/structure.map"};
  const auto& map = fixture.map;

  SECTION("renames a layer")
  {
    const auto json =
      runJson({"rename_layer", map, "--target", "layer:1", "--name", "Furniture"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:1"])"));
    CHECK(showNode(map, "layer:1")["name"] == Json{"Furniture"});
  }

  SECTION("refuses the default layer")
  {
    const auto result =
      runCli({"rename_layer", map, "--target", "layer:0", "--name", "Main"});
    checkError(result, "op_failed", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0"});
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("move_to_layer")
{
  auto fixture = EditFixture{"maps/structure.map"};
  const auto& map = fixture.map;

  SECTION("moves nodes to a layer")
  {
    const auto json = parseOutput(fixture.apply(R"([{
      "op": "move_to_layer", "target": ["layer:0/brush:1", "layer:0/group:0"],
      "layer": "layer:1"
    }])"));
    CHECK(
      json["ops"][0]["modified"]
      == parseJsonLiteral(R"(["layer:1/brush:1", "layer:1/group:0"])"));
  }

  SECTION("a brush of a brush entity moves with its entity")
  {
    const auto json = runJson(
      {"move_to_layer",
       map,
       "--target",
       "layer:0/entity:0/brush:0",
       "--layer",
       "layer:1"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:1/entity:0"])"));
  }

  SECTION("nodes in the layer already stay where they are")
  {
    const auto json = runJson(
      {"move_to_layer", map, "--target", "layer:1/brush:0", "--layer", "layer:1"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral("[]"));
  }

  SECTION("refuses a node inside a group")
  {
    const auto result = runCli(
      {"move_to_layer",
       map,
       "--target",
       "layer:0/group:0/brush:0",
       "--layer",
       "layer:1"});
    checkError(result, "op_failed", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/group:0/brush:0"});
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("set_layer_omit_from_export")
{
  auto fixture = EditFixture{"maps/structure.map"};
  const auto& map = fixture.map;

  SECTION("sets whether layers are exported")
  {
    const auto json = runJson(
      {"set_layer_omit_from_export", map, "--target", "layer:1", "--omit", "true"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:1"])"));
    CHECK(showNode(map, "layer:1")["omit_from_export"] == Json{true});

    const auto again = runJson(
      {"set_layer_omit_from_export", map, "--target", "layer:1", "--omit", "true"});
    CHECK(again["ops"][0]["modified"] == parseJsonLiteral("[]"));
  }

  SECTION("the default layer's setting is saved too")
  {
    runJson({"set_layer_omit_from_export", map, "--target", "layer:0", "--omit"});
    CHECK(showNode(map, "layer:0")["omit_from_export"] == Json{true});
  }
}

TEST_CASE("move_to_entity")
{
  auto fixture = EditFixture{"maps/structure.map"};
  const auto& map = fixture.map;

  SECTION("moves brushes into a brush entity")
  {
    const auto json = runJson(
      {"move_to_entity",
       map,
       "--target",
       "layer:0/brush:1",
       "--entity",
       "layer:0/entity:0"});
    CHECK(
      json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/entity:0/brush:1"])"));
    CHECK(
      showNode(map, "layer:0/entity:0/brush:1")["textures"]
      == parseJsonLiteral(R"(["wall"])"));
  }

  SECTION("an entity left without brushes is removed")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "create_brush_entity", "classname": "func_wall", "brushes": "layer:1/brush:0",
       "as": "$wall"},
      {"op": "move_to_entity", "target": "layer:0/entity:0/brush:0", "entity": "$wall"}
    ])"));
    const auto& op = json["ops"][1];
    CHECK(op["modified"] == parseJsonLiteral(R"(["layer:1/entity:0/brush:1"])"));
    CHECK(op["removed"] == parseJsonLiteral(R"(["layer:0/entity:0"])"));
  }

  SECTION("refuses a point entity")
  {
    checkError(
      runCli(
        {"move_to_entity",
         map,
         "--target",
         "layer:0/brush:1",
         "--entity",
         "layer:0/entity:1"}),
      "wrong_node_kind",
      1,
      0);
  }
}

TEST_CASE("lock")
{
  auto fixture = EditFixture{"maps/structure.map"};
  const auto& map = fixture.map;

  SECTION("a layer that is locked already stays as it is")
  {
    runJson({"lock", map, "--target", "layer:1"});
    const auto json = runJson({"lock", map, "--target", "layer:1"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral("[]"));
  }

  SECTION("locks layers, which later ops then refuse")
  {
    const auto result = fixture.apply(R"([
      {"op": "lock", "target": "layer:1"},
      {"op": "delete", "target": "layer:1/brush:0"}
    ])");
    checkError(result, "locked", 1, 1);

    const auto json = runJson({"lock", map, "--target", "layer:1"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:1"])"));
    CHECK(showNode(map, "layer:1")["locked"] == Json{true});
    checkError(runCli({"delete", map, "--target", "layer:1/brush:0"}), "locked", 1, 0);
  }

  SECTION("refuses a group")
  {
    checkError(
      runCli({"lock", map, "--target", "layer:0/group:0"}), "wrong_node_kind", 1, 0);
  }
}

TEST_CASE("linked_duplicate")
{
  auto fixture = EditFixture{"maps/structure.map"};
  const auto& map = fixture.map;

  SECTION("creates a linked copy of a group")
  {
    const auto json = runJson(
      {"linked_duplicate",
       map,
       "--target",
       "layer:0/group:0",
       "--offset",
       "0,128,0",
       "--as",
       "$copy"});
    CHECK(json["ops"][0]["created"] == parseJsonLiteral(R"(["layer:0/group:3"])"));
    CHECK(json["bindings"] == parseJsonLiteral(R"({"$copy": "layer:0/group:3"})"));

    const auto copy = showNode(map, "layer:0/group:3");
    CHECK(copy["siblings"] == parseJsonLiteral(R"(["layer:0/group:0"])"));
    CHECK(copy["link_id"] == showNode(map, "layer:0/group:0")["link_id"]);
    CHECK(
      showNode(map, "layer:0/group:3/brush:0")["aabb"]
      == parseJsonLiteral(R"({"min": [-48, 80, 0], "max": [-32, 96, 16]})"));
  }

  SECTION("an edit in the copy reaches the original")
  {
    runJson(
      {"linked_duplicate", map, "--target", "layer:0/group:1", "--offset", "0,512,0"});
    const auto json = runJson(
      {"set_property",
       map,
       "--target",
       "layer:0/group:3/entity:0",
       "--key",
       "light",
       "--value",
       "50"});
    CHECK(
      json["ops"][0]["instances_affected"]
      == parseJsonLiteral(
        R"(["layer:0/group:1", "layer:0/group:2", "layer:0/group:3"])"));
    CHECK(showNode(map, "layer:0/group:1/entity:0")["properties"]["light"] == Json{"50"});
  }

  SECTION("refuses a brush")
  {
    checkError(
      runCli(
        {"linked_duplicate", map, "--target", "layer:0/brush:0", "--offset", "0,0,0"}),
      "wrong_node_kind",
      1,
      0);
  }
}

TEST_CASE("separate_linked")
{
  auto fixture = EditFixture{"maps/structure.map"};
  const auto& map = fixture.map;

  SECTION("separates a linked group, so that edits no longer reach it")
  {
    const auto json = runJson({"separate_linked", map, "--target", "layer:0/group:2"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/group:2"])"));
    CHECK(showNode(map, "layer:0/group:2").find("link_id") == nullptr);

    const auto edit = runJson(
      {"set_property",
       map,
       "--target",
       "layer:0/group:1/entity:0",
       "--key",
       "light",
       "--value",
       "50"});
    CHECK(
      edit["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/group:1/entity:0"])"));
    CHECK(edit["ops"][0].find("instances_affected") == nullptr);
    CHECK(
      showNode(map, "layer:0/group:2/entity:0")["properties"].find("light") == nullptr);
  }

  SECTION("refuses a group that is linked to no group outside the targets")
  {
    const auto target = GENERATE(
      std::string{R"("layer:0/group:0")"},
      std::string{R"(["layer:0/group:1", "layer:0/group:2"])"});
    CAPTURE(target);
    checkError(
      fixture.apply(
        fmt::format(R"([{{"op": "separate_linked", "target": {}}}])", target)),
      "op_failed",
      1,
      0);
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("import")
{
  auto fixture = EditFixture{"maps/structure.map"};
  const auto& map = fixture.map;

  const auto lights = std::string{R"(// entity 0
{
"classname" "info_null"
"origin" "0 0 0"
}
// entity 1
{
"classname" "light"
"origin" "8 0 0"
})"};

  SECTION("pastes the top-level nodes of a fragment and binds them as a list")
  {
    auto batch = Json::array();
    auto op = Json::object();
    op.set("op", "import");
    op.set("text", lights);
    op.set("offset", parseJsonLiteral("[0, 0, 32]"));
    op.set("as", "$lights");
    batch.push(std::move(op));

    const auto json = parseOutput(fixture.apply(batch.dump()));
    CHECK(
      json["ops"][0]["created"]
      == parseJsonLiteral(R"(["layer:0/entity:2", "layer:0/entity:3"])"));
    CHECK(
      json["bindings"]
      == parseJsonLiteral(R"({"$lights": ["layer:0/entity:2", "layer:0/entity:3"]})"));
    CHECK(showNode(map, "layer:0/entity:3")["properties"]["origin"] == Json{"8 0 32"});
  }

  SECTION("binds a single pasted node as a list")
  {
    const auto json = runJson(
      {"import",
       map,
       "--text",
       R"({ "classname" "info_null" "origin" "0 0 0" })",
       "--as",
       "$null"});
    CHECK(json["bindings"] == parseJsonLiteral(R"({"$null": ["layer:0/entity:2"]})"));
  }

  SECTION("pastes a file into a group")
  {
    const auto prefab = fixture.env.copyFixture("maps/simple.map", "prefab.map");
    const auto json =
      runJson({"import", map, "--file", arg(prefab), "--parent", "layer:0/group:0"});
    const auto& created = json["ops"][0]["created"];
    REQUIRE(created.size() > 0);
    CHECK_THAT(created[0].asString(), StartsWith("layer:0/group:0/"));
  }

  SECTION("a fragment pasted into a linked group reaches its linked copies")
  {
    const auto json =
      runJson({"import", map, "--text", lights, "--parent", "layer:0/group:1"});
    const auto& op = json["ops"][0];
    CHECK(
      op["created"]
      == parseJsonLiteral(R"(["layer:0/group:1/entity:1", "layer:0/group:1/entity:2"])"));
    CHECK(
      op["modified"]
      == parseJsonLiteral(R"(["layer:0/group:2/entity:1", "layer:0/group:2/entity:2"])"));
  }

  SECTION("converts brushes in the Standard format")
  {
    const auto json = runJson({"import", map, "--text", R"({
( 0 0 0 ) ( 0 0 1 ) ( 64 0 0 ) std 0 0 0 1 1
( 0 0 0 ) ( 0 64 0 ) ( 0 0 1 ) std 0 0 0 1 1
( 0 0 0 ) ( 64 0 0 ) ( 0 64 0 ) std 0 0 0 1 1
( 64 64 16 ) ( 0 64 16 ) ( 64 64 0 ) std 0 0 0 1 1
( 64 64 16 ) ( 64 64 0 ) ( 64 0 16 ) std 0 0 0 1 1
( 64 64 16 ) ( 64 0 16 ) ( 0 64 16 ) std 0 0 0 1 1
})"});
    CHECK(json["ops"][0]["created"] == parseJsonLiteral(R"(["layer:0/brush:2"])"));
    CHECK(
      showNode(map, "layer:0/brush:2")["aabb"]
      == parseJsonLiteral(R"({"min": [0, 0, 0], "max": [64, 64, 16]})"));
  }

  SECTION("refuses a fragment that does not parse exactly")
  {
    const auto result = runCli({"import", map, "--text", R"({
"classname" "func_wall"
{
( 0 0 0 ) ( 0 1 0 ) ( 0 0 1 ) a [ 0 -1 0 0 ] [ 0 0 -1 0 ] 0 1 1
}
})"});
    checkError(result, "op_failed", 1, 0);
    CHECK(parseOutput(result)["error"].find("line") != nullptr);
    CHECK(fixture.unchanged());
  }

  SECTION("refuses text that holds no nodes")
  {
    checkError(runCli({"import", map, "--text", "no map"}), "op_failed", 1, 0);
    CHECK(fixture.unchanged());
  }

  SECTION("refuses a file that cannot be read")
  {
    checkError(
      runCli({"import", map, "--file", arg(fixture.env.path("missing.map"))}),
      "op_failed",
      1,
      0);
  }

  SECTION("takes exactly one of file and text")
  {
    checkError(runCli({"import", map}), "invalid_argument", 2);
    checkError(
      runCli({"import", map, "--text", lights, "--file", "prefab.map"}),
      "invalid_argument",
      2);
  }
}

} // namespace tb::cli
