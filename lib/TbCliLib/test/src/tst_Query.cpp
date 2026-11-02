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
#include "cli/NodePath.h"
#include "cli/Query.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::cli
{
using namespace Catch::Matchers;

namespace
{

/** The `path` of each item in a query result. */
std::vector<std::string> itemPaths(const Json& result)
{
  auto paths = std::vector<std::string>{};
  for (const auto& item : result["items"].asArray())
  {
    paths.push_back(item["path"].asString());
  }
  return paths;
}

std::string errorCode(const RunResult& result)
{
  return parseOutput(result)["error"]["code"].asString();
}

} // namespace

TEST_CASE("query")
{
  auto env = CliTestEnvironment{};
  env.writeConfig({}, quakeConfig());
  const auto map = arg(env.copyFixture("maps/query.map", "query.map"));

  const auto allBrushes = std::vector<std::string>{
    "layer:0/brush:0",
    "layer:0/brush:1",
    "layer:0/brush:2",
    "layer:0/brush:3",
    "layer:0/entity:1/brush:0",
    "layer:1/group:0/brush:0",
  };

  SECTION("returns matches in file order")
  {
    const auto result = runCli({"query", map, "type is brush"});
    REQUIRE(result.exitCode == 0);

    const auto json = parseOutput(result);
    CHECK(json["schema"] == Json{"tbcli.query/1"});
    CHECK(json["rev"].isString());
    CHECK(json["total"] == Json{6});
    CHECK(json["truncated"] == Json{false});
    CHECK(itemPaths(json) == allBrushes);
  }

  SECTION("pages with --limit and --offset")
  {
    const auto first = parseOutput(
      runCli({"query", map, "type is brush", "--limit", "2", "--offset", "1"}));
    CHECK(
      itemPaths(first) == std::vector<std::string>{"layer:0/brush:1", "layer:0/brush:2"});
    CHECK(first["total"] == Json{6});
    CHECK(first["truncated"] == Json{true});

    const auto last =
      parseOutput(runCli({"query", map, "type is brush", "--offset=5", "--limit=2"}));
    CHECK(itemPaths(last) == std::vector<std::string>{"layer:1/group:0/brush:0"});
    CHECK(last["truncated"] == Json{false});

    const auto beyond =
      parseOutput(runCli({"query", map, "type is brush", "--offset", "10"}));
    CHECK(beyond["items"] == Json::array());
    CHECK(beyond["total"] == Json{6});
    CHECK(beyond["truncated"] == Json{false});
  }

  SECTION("limits to 200 matches by default")
  {
    auto contents = std::string{
      "// Game: Quake\n// Format: Valve\n"
      "{\n\"mapversion\" \"220\"\n\"classname\" \"worldspawn\"\n}\n"};
    for (auto i = 0; i < 250; ++i)
    {
      contents += "{\n\"classname\" \"info_null\"\n\"origin\" \"0 0 0\"\n}\n";
    }
    const auto bigMap = arg(env.writeFile("big.map", contents));

    const auto json =
      parseOutput(runCli({"query", bigMap, R"(classname is "info_null")"}));
    CHECK(json["items"].size() == 200);
    CHECK(json["total"] == Json{250});
    CHECK(json["truncated"] == Json{true});
  }

  SECTION("--count-only returns only the total")
  {
    const auto result = runCli({"query", map, "type is brush", "--count-only"});
    REQUIRE(result.exitCode == 0);
    CHECK(parseOutput(result) == [&]() {
      auto expected = Json::object();
      expected.set("schema", "tbcli.query/1");
      expected.set("rev", parseOutput(result)["rev"]);
      expected.set("total", 6);
      return expected;
    }());
  }

  SECTION("brush summaries")
  {
    const auto json = parseOutput(runCli({"query", map, "type is brush"}));
    const auto& items = json["items"].asArray();
    REQUIRE(items.size() == 6);

    CHECK(items[0] == parseJsonLiteral(R"({
      "path": "layer:0/brush:0",
      "aabb": {"min": [0, 0, 0], "max": [64, 64, 16]},
      "axial_box": true,
      "textures": ["floor", "floor_top"],
      "entity": "world"
    })"));

    // six faces, but one of them slopes
    CHECK(items[3]["axial_box"] == Json{false});
    CHECK(items[3]["textures"] == parseJsonLiteral(R"(["ramp"])"));

    CHECK(items[4]["entity"] == Json{"layer:0/entity:1"});
  }

  SECTION("entity summaries hold only the properties that are set")
  {
    const auto json = parseOutput(runCli({"query", map, R"(classname is "light")"}));
    REQUIRE(json["items"].size() == 1);

    const auto& light = json["items"].asArray().front();
    CHECK(light["path"] == Json{"layer:0/entity:0"});
    CHECK(light["origin"] == parseJsonLiteral("[32, 32, 40]"));
    CHECK(light["properties"] == parseJsonLiteral(R"({
      "classname": "light", "origin": "32 32 40", "light": "200", "spawnflags": "1"
    })"));
    CHECK(light["definition"] == Json{"found"});
  }

  SECTION("layer and group summaries")
  {
    const auto json = parseOutput(runCli({"query", map, "type is group"}));
    CHECK(json["items"] == parseJsonLiteral(R"([{
      "path": "layer:1/group:0", "id": 2, "name": "Room",
      "bounds": {"min": [0, 0, 256], "max": [64, 64, 332]},
      "counts": {"brushes": 1, "entities": 1, "faces": 6},
      "hidden": false, "locked": false
    }])"));
  }

  SECTION("face summaries")
  {
    const auto json = parseOutput(runCli({"query", map, R"(material == "floor_top")"}));
    CHECK(json["items"] == parseJsonLiteral(R"([{
      "path": "layer:0/brush:0/face:3", "normal": [0, 0, 1], "texture": "floor_top"
    }])"));
    CHECK(json["total"] == Json{1});
  }

  SECTION("--within keeps nodes whose bounds lie inside the box")
  {
    const auto json =
      parseOutput(runCli({"query", map, "type is brush", "--within", "0,0,0,64,64,64"}));
    CHECK(
      itemPaths(json) == std::vector<std::string>{"layer:0/brush:0", "layer:0/brush:1"});
  }

  SECTION("--intersects keeps nodes whose bounds overlap the box")
  {
    const auto json = parseOutput(
      runCli({"query", map, "type is brush", "--intersects", "60 0 0 70 10 10"}));
    CHECK(
      itemPaths(json) == std::vector<std::string>{"layer:0/brush:0", "layer:0/brush:2"});
  }

  SECTION("--touching keeps nodes within 0.01 units of the node")
  {
    // brush 1 rests on brush 0, brush 2 is 0.005 units away from it
    const auto json = parseOutput(
      runCli({"query", map, "type is brush", "--touching", "layer:0/brush:0"}));
    CHECK(
      itemPaths(json) == std::vector<std::string>{"layer:0/brush:1", "layer:0/brush:2"});
    CHECK(json["total"] == Json{2});
  }

  SECTION("spatial filters combine with each other")
  {
    const auto json = parseOutput(runCli(
      {"query",
       map,
       "type is brush",
       "--touching",
       "layer:0/brush:0",
       "--within",
       "0 0 0 64 64 64"}));
    CHECK(itemPaths(json) == std::vector<std::string>{"layer:0/brush:1"});
  }

  SECTION("without a query, every node but the world matches")
  {
    const auto all = parseOutput(runCli({"query", map, "--count-only"}));
    // 2 layers, 1 group, 3 entities, 6 brushes
    CHECK(all["total"] == Json{12});

    const auto touching =
      parseOutput(runCli({"query", map, "--touching", "layer:0/brush:0"}));
    CHECK(
      itemPaths(touching)
      == std::vector<std::string>{"layer:0/brush:1", "layer:0/brush:2"});
  }

  SECTION("--format text prints an outline")
  {
    const auto result =
      runCli({"query", map, "type is brush", "--limit", "1", "--format", "text"});
    REQUIRE(result.exitCode == 0);
    CHECK_THAT(result.out, ContainsSubstring("schema: tbcli.query/1"));
    CHECK_THAT(result.out, ContainsSubstring("path: layer:0/brush:0"));
  }

  SECTION("queryMatches")
  {
    auto logger = NullLogger{};
    auto loadedMap = loadFixtureMap("maps/query.map", logger);
    auto& loaded = *loadedMap.map;

    const auto pathsOf = [](const std::vector<NodeOrFace>& matches) {
      auto result = std::vector<std::string>{};
      for (const auto& match : matches)
      {
        CHECK(!match.faceIndex);
        result.push_back(nodePath(*match.node));
      }
      return result;
    };

    CHECK(pathsOf(queryMatches(loaded, R"(type is "brush")")) == allBrushes);

    const auto all = queryMatches(loaded, std::nullopt);
    CHECK(all.size() == 12);
    CHECK(pathsOf(all).front() == "layer:0");
  }

  SECTION("errors")
  {
    SECTION("--touching an unknown node")
    {
      const auto result = runCli({"query", map, "--touching", "layer:9"});
      CHECK(result.exitCode == 1);
      CHECK(errorCode(result) == "node_not_found");
      CHECK(parseOutput(result)["error"]["node"] == Json{"layer:9"});
    }

    SECTION("--touching the world")
    {
      const auto result = runCli({"query", map, "--touching", "world"});
      CHECK(result.exitCode == 1);
      CHECK(errorCode(result) == "wrong_node_kind");
    }
  }
}

} // namespace tb::cli
