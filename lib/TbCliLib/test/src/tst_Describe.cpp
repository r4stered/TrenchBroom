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
#include "cli/Describe.h"
#include "mdl/Entity.h"

#include "vm/vec.h"

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

Json vec(const double x, const double y, const double z)
{
  return Json{Json::Array{x, y, z}};
}

/** An axis-aligned box brush in Valve 220 format. */
std::string boxBrush(
  const vm::vec3i& min, const vm::vec3i& max, const std::string& texture)
{
  const auto point = [](const vm::vec3i& p) {
    return fmt::format("( {} {} {} )", p.x(), p.y(), p.z());
  };
  const auto face = [&](
                      const vm::vec3i& p0,
                      const vm::vec3i& p1,
                      const vm::vec3i& p2,
                      const std::string_view uv) {
    return fmt::format(
      "{} {} {} {} {} 0 1 1\n", point(p0), point(p1), point(p2), texture, uv);
  };

  const auto x = vm::vec3i{1, 0, 0};
  const auto y = vm::vec3i{0, 1, 0};
  const auto z = vm::vec3i{0, 0, 1};
  return "{\n" + face(min, min + y, min + z, "[ 0 -1 0 0 ] [ 0 0 -1 0 ]")
         + face(min, min + z, min + x, "[ 1 0 0 0 ] [ 0 0 -1 0 ]")
         + face(min, min + x, min + y, "[ -1 0 0 0 ] [ 0 -1 0 0 ]")
         + face(max, max + y, max + x, "[ 1 0 0 0 ] [ 0 -1 0 0 ]")
         + face(max, max + x, max + z, "[ -1 0 0 0 ] [ 0 0 -1 0 ]")
         + face(max, max + z, max + y, "[ 0 1 0 0 ] [ 0 0 -1 0 ]") + "}\n";
}

/**
 * A map with 2000 brushes using 1000 textures, 500 groups, and 1000 entities of 500
 * classes, each targeting the next, with every fifth target dangling, followed by a
 * player start. Worldspawn has 103 properties and a long message.
 */
std::string largeMap()
{
  auto map = std::string{
    "// Game: Quake\n// Format: Valve\n"
    "{\n\"mapversion\" \"220\"\n\"classname\" \"worldspawn\"\n"};
  map += fmt::format("\"message\" \"{}\"\n", std::string(1000, 'm'));
  for (auto i = 0; i < 100; ++i)
  {
    map += fmt::format("\"key{}\" \"value\"\n", i);
  }
  for (auto i = 0; i < 1000; ++i)
  {
    map += boxBrush({i * 16, 0, 0}, {i * 16 + 8, 8, 8}, fmt::format("tex{}", i));
  }
  map += "}\n";

  for (auto i = 0; i < 500; ++i)
  {
    map += fmt::format(
      "{{\n\"classname\" \"func_group\"\n\"_tb_type\" \"_tb_group\"\n"
      "\"_tb_name\" \"group\"\n\"_tb_id\" \"{}\"\n",
      i + 1);
    map += boxBrush({i * 16, 32, 0}, {i * 16 + 8, 40, 8}, fmt::format("tex{}", i));
    map += boxBrush({i * 16, 64, 0}, {i * 16 + 8, 72, 8}, fmt::format("tex{}", i));
    map += "}\n";
  }

  for (auto i = 0; i < 1000; ++i)
  {
    const auto target = i % 5 == 0 ? std::string{"nowhere"} : fmt::format("t{}", i + 1);
    map += fmt::format(
      "{{\n\"classname\" \"custom_{}\"\n\"origin\" \"{} 128 0\"\n"
      "\"targetname\" \"t{}\"\n\"target\" \"{}\"\n}}\n",
      i % 500,
      i * 16,
      i,
      target);
  }

  map += "{\n\"classname\" \"info_player_start\"\n\"origin\" \"0 0 0\"\n}\n";
  return map;
}

/**
 * Copies maps/describe.map with its texture wad and a Quake directory holding the
 * palette into the given environment, and returns the map's path.
 */
std::filesystem::path setUpDescribeFixture(CliTestEnvironment& env)
{
  env.copyFile(texturesFixturePath() / "cr8_a_excerpt.wad", "maps/textures.wad");
  env.copyFile(texturesFixturePath() / "palette.lmp", "quake/id1/gfx/palette.lmp");

  auto config = quakeConfig();
  config.set("quake_dir", arg(env.path("quake")));
  env.writeConfig("maps", config);

  return env.copyFixture("maps/describe.map", "maps/describe.map");
}

} // namespace

TEST_CASE("describe")
{
  auto env = CliTestEnvironment{};
  env.writeConfig({}, quakeConfig());

  SECTION("keyEntityClasses")
  {
    CHECK(
      keyEntityClasses(std::nullopt)
      == std::vector<std::string>{
        "info_player_start",
        "info_player_deathmatch",
        "info_player_coop",
        "trigger_changelevel"});
    CHECK(
      keyEntityClasses(std::vector<std::string>{"light"})
      == std::vector<std::string>{"light"});
  }

  SECTION("hasLink")
  {
    CHECK_FALSE(hasLink(mdl::Entity{{{"classname", "light"}}}));
    CHECK(hasLink(mdl::Entity{{{"classname", "light"}, {"targetname", "t"}}}));
    CHECK(hasLink(mdl::Entity{{{"classname", "trigger_once"}, {"target", "t"}}}));
  }

  SECTION("isKeyEntity")
  {
    const auto classes = std::vector<std::string>{"info_player_start"};
    CHECK(isKeyEntity(mdl::Entity{{{"classname", "info_player_start"}}}, classes));
    CHECK(isKeyEntity(mdl::Entity{{{"classname", "light"}, {"target", "t"}}}, classes));
    CHECK_FALSE(isKeyEntity(mdl::Entity{{{"classname", "light"}}}, classes));
  }

  SECTION("Valve 220 map")
  {
    const auto map = env.copyFixture("maps/simple.map", "simple.map");
    const auto result = runCli({"describe", arg(map)});
    REQUIRE(result.exitCode == 0);

    const auto json = parseOutput(result);
    CHECK(json["schema"] == Json{"tbcli.describe/1"});

    SECTION("counts brushes, entities and faces")
    {
      auto counts = Json::object();
      counts.set("brushes", 3);
      counts.set("entities", 3);
      counts.set("faces", 18);
      CHECK(json["counts"] == counts);
    }

    SECTION("world bounds cover brushes and point entities")
    {
      auto bounds = Json::object();
      bounds.set("min", vec(-64, -64, -16));
      // the light at x=100.1234 extends 8 units to 108.1234, rounded to 3 decimals
      bounds.set("max", vec(108.123, 96, 128));
      CHECK(json["bounds"] == bounds);
    }

    SECTION("numbers are written rounded, integers as integers")
    {
      CHECK_THAT(
        result.out,
        ContainsSubstring(R"("bounds":{"min":[-64,-64,-16],"max":[108.123,96,128]})"));
      CHECK_THAT(
        result.out,
        ContainsSubstring(R"("counts":{"brushes":3,"entities":3,"faces":18})"));
    }
  }

  SECTION("layer and group tree")
  {
    const auto map = setUpDescribeFixture(env);
    const auto result = runCli({"describe", arg(map)});
    REQUIRE(result.exitCode == 0);

    // linked groups are reported once per instance, with their own paths
    CHECK(parseOutput(result)["layers"] == parseJsonLiteral(R"({"items": [
      {
        "path": "layer:0", "id": null, "name": "Default Layer",
        "bounds": {"min": [-272, -256, -16], "max": [256, 256, 144]},
        "counts": {"brushes": 10, "entities": 11, "faces": 60},
        "hidden": false, "locked": false, "omit_from_export": false,
        "groups": [
          {
            "path": "layer:0/group:0", "id": 4, "name": "Pillar",
            "bounds": {"min": [-100, 92, 0], "max": [-76, 116, 128]},
            "counts": {"brushes": 2, "entities": 1, "faces": 12},
            "hidden": false, "locked": false,
            "link_id": "pillar-link", "siblings": ["layer:0/group:1"],
            "groups": [
              {
                "path": "layer:0/group:0/group:0", "id": 5, "name": "Cap",
                "bounds": {"min": [-100, 92, 104], "max": [-76, 116, 128]},
                "counts": {"brushes": 1, "entities": 1, "faces": 6},
                "hidden": false, "locked": false,
                "link_id": "cap-link", "siblings": ["layer:0/group:1/group:0"],
                "groups": []
              }
            ]
          },
          {
            "path": "layer:0/group:1", "id": 6, "name": "Pillar",
            "bounds": {"min": [28, 92, 0], "max": [52, 116, 128]},
            "counts": {"brushes": 2, "entities": 1, "faces": 12},
            "hidden": false, "locked": false,
            "link_id": "pillar-link", "siblings": ["layer:0/group:0"],
            "groups": [
              {
                "path": "layer:0/group:1/group:0", "id": 7, "name": "Cap",
                "bounds": {"min": [28, 92, 104], "max": [52, 116, 128]},
                "counts": {"brushes": 1, "entities": 1, "faces": 6},
                "hidden": false, "locked": false,
                "link_id": "cap-link", "siblings": ["layer:0/group:0/group:0"],
                "groups": []
              }
            ]
          }
        ]
      },
      {
        "path": "layer:1", "id": 1, "name": "Upper",
        "bounds": {"min": [-256, -256, 256], "max": [256, 256, 348]},
        "counts": {"brushes": 3, "entities": 1, "faces": 18},
        "hidden": true, "locked": true, "omit_from_export": true,
        "groups": [
          {
            "path": "layer:1/group:0", "id": 2, "name": "Room",
            "bounds": {"min": [-64, -64, 272], "max": [64, 64, 348]},
            "counts": {"brushes": 2, "entities": 1, "faces": 12},
            "hidden": true, "locked": true,
            "groups": [
              {
                "path": "layer:1/group:0/group:0", "id": 3, "name": "Crate",
                "bounds": {"min": [-16, -16, 288], "max": [16, 16, 348]},
                "counts": {"brushes": 1, "entities": 1, "faces": 6},
                "hidden": true, "locked": true,
                "groups": []
              }
            ]
          }
        ]
      }
    ], "total": 8, "truncated": false})"));
  }

  SECTION("entity class histogram")
  {
    const auto map = setUpDescribeFixture(env);
    const auto output = parseOutput(runCli({"describe", arg(map)}));

    // by count, then by name
    CHECK(output["entity_classes"] == parseJsonLiteral(R"({"items": [
      {"classname": "light", "count": 5, "definition": "found"},
      {"classname": "func_door", "count": 1, "definition": "found"},
      {"classname": "info_player_deathmatch", "count": 1, "definition": "found"},
      {"classname": "info_player_start", "count": 1, "definition": "found"},
      {"classname": "monster_army", "count": 1, "definition": "found"},
      {"classname": "my_custom_thing", "count": 1, "definition": "missing"},
      {"classname": "trigger_changelevel", "count": 1, "definition": "found"},
      {"classname": "trigger_once", "count": 1, "definition": "found"}
    ], "total": 8, "truncated": false})"));
  }

  SECTION("texture histogram")
  {
    const auto map = setUpDescribeFixture(env);

    SECTION("flags missing textures and lists them first")
    {
      const auto output = parseOutput(runCli({"describe", arg(map)}));
      CHECK(output["textures"] == parseJsonLiteral(R"({
        "checked": true, "missing": 2, "items": [
          {"name": "trigger", "count": 12, "missing": true},
          {"name": "missing_wall", "count": 6, "missing": true},
          {"name": "added", "count": 60, "missing": false}
        ], "total": 3, "truncated": false})"));
    }

    SECTION("does not flag textures without a Quake directory")
    {
      env.writeConfig("maps", quakeConfig());

      const auto output = parseOutput(runCli({"describe", arg(map)}));
      CHECK(output["textures"] == parseJsonLiteral(R"({
        "checked": false, "items": [
          {"name": "added", "count": 60},
          {"name": "trigger", "count": 12},
          {"name": "missing_wall", "count": 6}
        ], "total": 3, "truncated": false})"));
    }
  }

  SECTION("key entities")
  {
    const auto map = setUpDescribeFixture(env);

    const auto paths = [](const Json& keyEntities) {
      auto result = std::vector<std::string>{};
      for (const auto& keyEntity : keyEntities["items"].asArray())
      {
        result.push_back(keyEntity["path"].asString());
      }
      return result;
    };

    SECTION("lists worldspawn, the key classes, then entities with targetname or target")
    {
      const auto keyEntities =
        parseOutput(runCli({"describe", arg(map)}))["key_entities"];

      CHECK(
        paths(keyEntities)
        == std::vector<std::string>{
          "world",
          "layer:0/entity:0",
          "layer:0/entity:1",
          "layer:0/entity:8",
          "layer:0/entity:3",
          "layer:0/entity:4",
          "layer:0/entity:5",
          "layer:0/entity:6",
          "layer:0/entity:7",
          // once per linked instance
          "layer:0/group:0/group:0/entity:0",
          "layer:0/group:1/group:0/entity:0",
        });
      CHECK(keyEntities["total"] == Json{11});
      CHECK(keyEntities["truncated"] == Json{false});

      const auto& items = keyEntities["items"];
      CHECK(items[0] == parseJsonLiteral(R"({
        "path": "world", "classname": "worldspawn",
        "properties": {
          "mapversion": "220", "classname": "worldspawn", "wad": "textures.wad",
          "message": "Describe fixture"
        },
        "definition": "found"})"));
      CHECK(items[1] == parseJsonLiteral(R"({
        "path": "layer:0/entity:0", "classname": "info_player_start",
        "origin": [0, 0, 24],
        "properties": {
          "classname": "info_player_start", "origin": "0 0 24", "angle": "90"
        },
        "definition": "found"})"));
      CHECK(items[4] == parseJsonLiteral(R"({
        "path": "layer:0/entity:3", "classname": "light", "origin": [32, 0, 96],
        "properties": {
          "classname": "light", "origin": "32 0 96", "targetname": "lamp",
          "spawnflags": "1"
        },
        "definition": "found", "spawnflags": ["Start off"]})"));
      CHECK(items[5] == parseJsonLiteral(R"({
        "path": "layer:0/entity:4", "classname": "trigger_once",
        "aabb": {"min": [-64, -64, 0], "max": [-32, 64, 64]},
        "properties": {
          "classname": "trigger_once", "target": "door1", "killtarget": "lamp"
        },
        "definition": "found"})"));
      CHECK(items[7]["spawnflags"] == parseJsonLiteral(R"(["Ambush", "Not on Easy"])"));
      CHECK(items[8] == parseJsonLiteral(R"({
        "path": "layer:0/entity:7", "classname": "my_custom_thing",
        "origin": [-128, 0, 24],
        "properties": {
          "classname": "my_custom_thing", "origin": "-128 0 24", "targetname": "custom"
        },
        "definition": "missing"})"));
      CHECK(items[10]["origin"] == parseJsonLiteral("[40, 104, 112]"));
    }

    SECTION("the class list can be overridden in tbcli.json")
    {
      auto describeConfig = Json::object();
      describeConfig.set("key_entities", parseJsonLiteral(R"(["light"])"));
      auto config = quakeConfig();
      config.set("describe", describeConfig);
      env.writeConfig("maps", config);

      const auto keyEntities =
        parseOutput(runCli({"describe", arg(map)}))["key_entities"];
      CHECK(
        paths(keyEntities)
        == std::vector<std::string>{
          "world",
          "layer:0/entity:2",
          "layer:0/entity:3",
          "layer:0/group:0/group:0/entity:0",
          "layer:0/group:1/group:0/entity:0",
          "layer:1/group:0/group:0/entity:0",
          "layer:0/entity:4",
          "layer:0/entity:5",
          "layer:0/entity:6",
          "layer:0/entity:7",
        });
    }

    SECTION("the class list can be overridden by a flag")
    {
      const auto keyEntities = parseOutput(runCli(
        {"describe",
         arg(map),
         "--key-entities",
         "info_player_deathmatch,light"}))["key_entities"];
      CHECK(keyEntities["total"] == Json{11});
      CHECK(paths(keyEntities)[1] == "layer:0/entity:1");
      CHECK(paths(keyEntities)[2] == "layer:0/entity:2");
    }
  }

  SECTION("link graph with dangling targets")
  {
    const auto map = setUpDescribeFixture(env);
    const auto output = parseOutput(runCli({"describe", arg(map)}));

    CHECK(output["link_graph"] == parseJsonLiteral(R"({
      "edges": {"items": [
        {"from": "layer:0/entity:4", "to": "layer:0/entity:5", "key": "target"},
        {"from": "layer:0/entity:4", "to": "layer:0/entity:3", "key": "killtarget"},
        {"from": "layer:0/entity:6", "to": "layer:0/entity:3", "key": "target"}
      ], "total": 3, "truncated": false},
      "dangling": {"items": [
        {"from": "layer:0/entity:5", "key": "target", "target": "nowhere"}
      ], "total": 1, "truncated": false}})"));
  }

  SECTION("validator issue counts per type")
  {
    const auto map = env.copyFixture("maps/verify.map", "verify.map");

    // the same counts verify reports, with tbcli.json's severities and suppressions
    auto config = quakeConfig();
    config.set("verify", parseJsonLiteral(R"({
        "severity": {"empty_group": "error"},
        "ignore": [{"type": "link_source"}]
      })"));
    env.writeConfig({}, config);

    const auto output = parseOutput(runCli({"describe", arg(map)}));
    CHECK(output["issues"] == parseJsonLiteral(R"({
      "counts": {
        "missing_classname": 1,
        "missing_definition": 1,
        "empty_group": 1,
        "link_target": 1,
        "non_integer_vertices": 1,
        "empty_property_value": 3,
        "invalid_uv_scale": 1,
        "suppressed": 1
      },
      "by_severity": {"error": 2, "warning": 6, "info": 1}
    })"));
  }

  SECTION("CLI check issue counts")
  {
    const auto map = env.copyFixture("maps/cli_checks.map", "cli_checks.map");
    env.writeConfig({}, quakeConfig());

    const auto counts = parseOutput(runCli({"describe", arg(map)}))["issues"]["counts"];
    CHECK(counts["fgd_property_type"] == Json{3});
    CHECK(counts["entity_inside_solid"] == Json{2});
    CHECK(counts["no_player_start"] == Json{1});
    CHECK(counts["degenerate_brush"] == Json{2});
  }

  SECTION("output stays bounded on a large map")
  {
    const auto map = env.writeFile("large.map", largeMap());
    const auto result = runCli({"describe", arg(map)});
    REQUIRE(result.exitCode == 0);

    const auto output = parseOutput(result);
    CHECK(output["counts"]["brushes"] == Json{2000});
    CHECK(output["layers"]["total"] == Json{501});
    CHECK(output["layers"]["truncated"] == Json{true});
    CHECK(output["entity_classes"]["truncated"] == Json{true});
    CHECK(output["textures"]["truncated"] == Json{true});
    CHECK(output["key_entities"]["total"] == Json{1002});
    CHECK(output["key_entities"]["truncated"] == Json{true});

    // the player start at the end of the file is listed before the targeted entities
    const auto& keyEntities = output["key_entities"]["items"];
    CHECK(keyEntities[1]["classname"] == Json{"info_player_start"});

    // worldspawn's properties are capped and its long message is cut
    const auto& worldspawn = keyEntities[0];
    CHECK(worldspawn["properties"].size() == 64);
    CHECK(worldspawn["properties"]["message"].asString() == std::string(256, 'm'));
    CHECK(worldspawn["properties_truncated"] == Json{true});
    CHECK(output["link_graph"]["edges"]["total"] == Json{799});
    CHECK(output["link_graph"]["edges"]["truncated"] == Json{true});
    CHECK(output["link_graph"]["dangling"]["total"] == Json{201});
    CHECK(output["link_graph"]["dangling"]["truncated"] == Json{true});

    // the map itself is about 1 MB
    CHECK(result.out.size() < 100'000);
  }

  SECTION("rev changes when the file changes")
  {
    const auto map = env.copyFixture("maps/simple.map", "simple.map");
    const auto before = parseOutput(runCli({"describe", arg(map)}))["rev"];

    env.writeFile("simple.map", env.readFile("simple.map") + "\n");
    const auto after = parseOutput(runCli({"describe", arg(map)}))["rev"];

    CHECK(before.isString());
    CHECK(after.isString());
    CHECK(before != after);
  }

  SECTION("empty map has a rev and no bounds")
  {
    const auto map = env.writeFile(
      "empty.map",
      "// Game: Quake\n// Format: Valve\n{\n\"classname\" \"worldspawn\"\n}\n");
    const auto json = parseOutput(runCli({"describe", arg(map)}));

    // the first 12 hex digits of the SHA-256 of the file bytes above
    CHECK(json["rev"] == Json{"5cd21b576a4f"});

    auto counts = Json::object();
    counts.set("brushes", 0);
    counts.set("entities", 0);
    counts.set("faces", 0);
    CHECK(json["counts"] == counts);
    CHECK(json["bounds"].isNull());
  }

  SECTION("non-Valve-220 map is refused")
  {
    env.copyFixture("maps/standard.map", "standard.map");
    auto contents = env.readFile("standard.map");

    // without a format header, and with a face that a Standard load would skip
    const auto variant = GENERATE("with header", "without header and with a bad face");
    if (std::string_view{variant} != "with header")
    {
      contents.erase(0, contents.find("// entity 0"));
      const auto points = std::string{"( -64 -64 -16 ) ( -64 -63 -16 ) ( -64 -64 -15 )"};
      contents.replace(
        contents.find(points), points.size(), "( 0 0 0 ) ( 1 0 0 ) ( 2 0 0 )");
    }
    const auto map = env.writeFile("standard.map", contents);
    const auto result = runCli({"describe", arg(map)});

    CHECK(result.exitCode == 1);
    const auto json = parseOutput(result);
    const auto& error = json["error"];
    CHECK(error["code"] == Json{"not_valve_220"});
    CHECK_THAT(error["message"].asString(), ContainsSubstring("Standard"));
    CHECK_THAT(error["message"].asString(), ContainsSubstring("Valve 220"));
  }

  SECTION("unparseable map fails with its line")
  {
    const auto map = env.copyFixture("maps/broken.map", "broken.map");
    const auto result = runCli({"describe", arg(map)});

    CHECK(result.exitCode == 1);
    const auto json = parseOutput(result);
    const auto& error = json["error"];
    CHECK(error["code"] == Json{"parse_error"});
    CHECK(error["line"] == Json{19});
  }

  SECTION("map with a skipped face still loads")
  {
    env.copyFixture("maps/simple.map", "simple.map");
    auto contents = env.readFile("simple.map");
    // collinear plane points are read, but the face cannot be created and is skipped
    const auto points = std::string{"( -64 -64 0 ) ( -64 -63 0 ) ( -64 -64 1 )"};
    contents.replace(
      contents.find(points), points.size(), "( 0 0 0 ) ( 1 0 0 ) ( 2 0 0 )");
    const auto map = env.writeFile("lossy.map", contents);

    const auto result = runCli({"describe", arg(map)});
    CHECK(result.exitCode == 0);
    CHECK_THAT(result.err, ContainsSubstring("Skipping face"));
    CHECK(parseOutput(result)["schema"] == Json{"tbcli.describe/1"});
  }

  SECTION("--format text prints an outline of the same data")
  {
    const auto map = env.copyFixture("maps/simple.map", "simple.map");
    const auto rev = parseOutput(runCli({"describe", arg(map)}))["rev"].asString();
    const auto result = runCli({"describe", arg(map), "--format", "text"});

    CHECK(result.exitCode == 0);
    CHECK_THAT(
      result.out,
      StartsWith(
        "schema: tbcli.describe/1\n"
        "rev: "
        + rev
        + "\n"
          "counts: {brushes: 3, entities: 3, faces: 18}\n"
          "bounds: {min: [-64, -64, -16], max: [108.123, 96, 128]}\n"
          "layers:\n"));
    CHECK_THAT(result.out, ContainsSubstring("classname: info_player_start"));
  }
}

} // namespace tb::cli
