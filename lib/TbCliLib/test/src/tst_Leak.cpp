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
#include "cli/Compile.h"
#include "cli/Config.h"
#include "cli/FileUtils.h"
#include "cli/Leak.h"
#include "cli/LoadMap.h"
#include "cli/NodePath.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/WorldNode.h"

#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{

namespace
{

/**
 * The trail qbsp recorded for compile_leak.map, whose east wall stops short of the
 * north wall: from the light at 144 0 96 through the hole at x 272..288, y 64..144 to
 * the void.
 */
const auto RecordedTrail = std::vector<vm::vec3d>{
  {144, 0, 96},
  {144, 32, 64},
  {144, 32, 64},
  {272, 96, 64},
  {272, 96, 64},
  {280, 156, 0},
  {280, 156, 0},
  {-272, 156, -20},
};

std::vector<std::string> pathsOf(const std::vector<mdl::BrushNode*>& brushes)
{
  auto result = std::vector<std::string>{};
  for (const auto* brush : brushes)
  {
    result.push_back(nodePath(*brush));
  }
  return result;
}

} // namespace

TEST_CASE("Leak")
{
  SECTION("parsePointFile")
  {
    SECTION("reads the points of a recorded pointfile as they are")
    {
      const auto text =
        *readFile(fixturePath("compiled/compile_leak/qbsp/compile_leak.pts"));
      CHECK(parsePointFile(text) == RecordedTrail);
    }

    SECTION("skips blank lines and reads decimals")
    {
      CHECK(
        parsePointFile("1.5 -2 3e2\r\n\n  \n0 0 0\n")
        == std::vector<vm::vec3d>{{1.5, -2, 300}, {0, 0, 0}});
      CHECK(parsePointFile("") == std::vector<vm::vec3d>{});
    }

    SECTION("returns nothing for a line that is not a point")
    {
      CHECK(parsePointFile("1 2\n") == std::nullopt);
      CHECK(parsePointFile("1 2 3 4\n") == std::nullopt);
      CHECK(parsePointFile("1 2 x\n") == std::nullopt);
    }
  }

  SECTION("simplifyTrail")
  {
    CHECK(
      simplifyTrail(RecordedTrail)
      == std::vector<vm::vec3d>{
        {144, 0, 96}, {144, 32, 64}, {272, 96, 64}, {280, 156, 0}, {-272, 156, -20}});

    // points on a straight line, as qbsp writes them with -leakdist
    CHECK(
      simplifyTrail({{0, 0, 0}, {2, 0, 0}, {4, 0, 0}, {4, 2, 0}, {4, 4, 0}})
      == std::vector<vm::vec3d>{{0, 0, 0}, {4, 0, 0}, {4, 4, 0}});

    // turning back is a turn
    CHECK(
      simplifyTrail({{0, 0, 0}, {4, 0, 0}, {2, 0, 0}})
      == std::vector<vm::vec3d>{{0, 0, 0}, {4, 0, 0}, {2, 0, 0}});

    CHECK(simplifyTrail({{1, 2, 3}}) == std::vector<vm::vec3d>{{1, 2, 3}});
    CHECK(simplifyTrail({}).empty());
  }

  SECTION("brushesNearExit")
  {
    auto logger = NullLogger{};
    const auto loadedMap = loadFixtureMap("compiled/compile_leak.map", logger);
    auto& worldNode = loadedMap.map->worldNode();

    SECTION("lists the brushes nearest where the trail leaves the map")
    {
      // the trail leaves the map after 272 96 64, in the hole between the end of the
      // east wall (brush 3) and the north wall (brush 5), 32 units from each and 64
      // from the floor (brush 0) and the ceiling (brush 1)
      CHECK(
        pathsOf(brushesNearExit(worldNode, RecordedTrail, 3))
        == std::vector<std::string>{
          "layer:0/brush:3", "layer:0/brush:5", "layer:0/brush:0"});
      CHECK(
        pathsOf(brushesNearExit(worldNode, RecordedTrail, 1))
        == std::vector<std::string>{"layer:0/brush:3"});
    }

    SECTION("uses the last point of a trail that stays inside the map")
    {
      // ends on the floor (brush 0) west of the doorway, 8 units from the wall brushes
      // 6 and 7 at x 0..16 in x and 32 in y
      CHECK(
        pathsOf(brushesNearExit(worldNode, {{-128, 0, 24}, {-8, 0, 0}}, 3))
        == std::vector<std::string>{
          "layer:0/brush:0", "layer:0/brush:6", "layer:0/brush:7"});
    }

    SECTION("leaves out brushes that do not seal the map")
    {
      // a func_wall that fills the hole does not seal it
      auto env = CliTestEnvironment{};
      const auto map =
        env.writeFile("m.map", *readFile(fixturePath("compiled/compile_leak.map")) + R"(
{
"classname" "func_wall"
{
( 272 64 0 ) ( 272 65 0 ) ( 272 64 1 ) wall1_1 [ 0 -1 0 0 ] [ 0 0 -1 0 ] 0 1 1
( 272 64 0 ) ( 272 64 1 ) ( 273 64 0 ) wall1_1 [ 1 0 0 0 ] [ 0 0 -1 0 ] 0 1 1
( 272 64 0 ) ( 273 64 0 ) ( 272 65 0 ) wall1_1 [ -1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1
( 288 144 128 ) ( 288 145 128 ) ( 289 144 128 ) wall1_1 [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1
( 288 144 128 ) ( 289 144 128 ) ( 288 144 129 ) wall1_1 [ -1 0 0 0 ] [ 0 0 -1 0 ] 0 1 1
( 288 144 128 ) ( 288 144 129 ) ( 288 145 128 ) wall1_1 [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 1 1
}
}
)");
      auto config = Config{};
      config.gameConfig = quakeGameConfigPath();
      config.fgd = {quakeFgdPath()};
      const auto withWall = loadMap(map, config, logger);
      CHECK(
        pathsOf(brushesNearExit(withWall.map->worldNode(), RecordedTrail, 3))
        == std::vector<std::string>{
          "layer:0/brush:3", "layer:0/brush:5", "layer:0/brush:0"});
    }

    SECTION("returns nothing for an empty trail")
    {
      CHECK(brushesNearExit(worldNode, {}, 3).empty());
    }
  }

  SECTION("isLeakOccupant")
  {
    const auto messages = parseCompilerLog(
      *readFile(fixturePath("compiled/compile_leak/qbsp/compile_leak.log")), {});
    auto occupants = std::vector<std::string>{};
    for (const auto& message : messages)
    {
      if (isLeakOccupant(message))
      {
        occupants.push_back(message.message);
      }
    }
    // once for hull 0 and once for hull 1
    CHECK(
      occupants
      == std::vector<std::string>{
        R"(Reached occupant "light" at (144 0 96), no filling performed.)",
        R"(Reached occupant "light" at (144 0 96), no filling performed.)"});
  }
}

} // namespace tb::cli
