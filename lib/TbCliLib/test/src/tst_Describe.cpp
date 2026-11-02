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

} // namespace

TEST_CASE("describe")
{
  auto env = CliTestEnvironment{};
  env.writeConfig({}, quakeConfig());

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
    CHECK(
      result.out
      == "schema: tbcli.describe/1\n"
         "rev: "
           + rev
           + "\n"
             "counts: {brushes: 3, entities: 3, faces: 18}\n"
             "bounds: {min: [-64, -64, -16], max: [108.123, 96, 128]}\n");
  }
}

} // namespace tb::cli
