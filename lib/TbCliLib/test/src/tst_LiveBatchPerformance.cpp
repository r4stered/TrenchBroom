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
#include "FakeEditorHost.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{
namespace
{

/**
 * A Quake map of a square grid of rooms, each with a floor, a ceiling, two walls with a
 * doorway (six brushes) and a light, inside Quake's soft map bounds.
 */
std::string roomGrid(const int roomsPerSide)
{
  constexpr auto roomSize = 160;
  constexpr auto gridOrigin = -4000;
  constexpr auto wallThickness = 16;

  using Point = std::array<int, 3>;
  const auto face = [](
                      const Point& p1, const Point& p2, const Point& p3, const auto& uv) {
    return fmt::format(
      "( {} ) ( {} ) ( {} ) wall {} 0 1 1\n",
      fmt::join(p1, " "),
      fmt::join(p2, " "),
      fmt::join(p3, " "),
      uv);
  };
  const auto brush = [&](const Point& min, const Point& max) {
    const auto [x0, y0, z0] = min;
    const auto [x1, y1, z1] = max;
    return "{\n"
           + face(min, {x0, y0 + 1, z0}, {x0, y0, z0 + 1}, "[ 0 -1 0 0 ] [ 0 0 -1 0 ]")
           + face(min, {x0, y0, z0 + 1}, {x0 + 1, y0, z0}, "[ 1 0 0 0 ] [ 0 0 -1 0 ]")
           + face(min, {x0 + 1, y0, z0}, {x0, y0 + 1, z0}, "[ -1 0 0 0 ] [ 0 -1 0 0 ]")
           + face(max, {x1, y1 + 1, z1}, {x1 + 1, y1, z1}, "[ 1 0 0 0 ] [ 0 -1 0 0 ]")
           + face(max, {x1 + 1, y1, z1}, {x1, y1, z1 + 1}, "[ -1 0 0 0 ] [ 0 0 -1 0 ]")
           + face(max, {x1, y1, z1 + 1}, {x1, y1 + 1, z1}, "[ 0 1 0 0 ] [ 0 0 -1 0 ]")
           + "}\n";
  };

  auto result = std::string{
    "// Game: Quake\n// Format: Valve\n{\n\"mapversion\" \"220\"\n"
    "\"classname\" \"worldspawn\"\n"};
  for (auto i = 0; i < roomsPerSide; ++i)
  {
    for (auto j = 0; j < roomsPerSide; ++j)
    {
      const auto x = gridOrigin + i * roomSize, y = gridOrigin + j * roomSize;
      result += brush({x, y, -wallThickness}, {x + roomSize, y + roomSize, 0});
      result += brush({x, y, 128}, {x + roomSize, y + roomSize, 128 + wallThickness});
      result += brush({x, y, 0}, {x + 48, y + wallThickness, 128});
      result += brush({x + 112, y, 0}, {x + roomSize, y + wallThickness, 128});
      result += brush({x, y + wallThickness, 0}, {x + wallThickness, y + 48, 128});
      result += brush({x, y + 112, 0}, {x + wallThickness, y + roomSize, 128});
    }
  }
  result += "}\n";
  for (auto i = 0; i < roomsPerSide; ++i)
  {
    for (auto j = 0; j < roomsPerSide; ++j)
    {
      result += fmt::format(
        "{{\n\"classname\" \"light\"\n\"origin\" \"{} {} 96\"\n\"light\" \"300\"\n}}\n",
        gridOrigin + i * roomSize + roomSize / 2,
        gridOrigin + j * roomSize + roomSize / 2);
    }
  }
  result += fmt::format(
    "{{\n\"classname\" \"info_player_start\"\n\"origin\" \"{} {} 24\"\n}}\n",
    gridOrigin + roomSize / 2,
    gridOrigin + roomSize / 2);
  return result;
}

/** A JSON array of the given number of elements, each made by the given function. */
std::string jsonArray(const int count, const std::function<std::string(int)>& element)
{
  auto result = std::string{"["};
  for (auto i = 0; i < count; ++i)
  {
    result += (i == 0 ? "" : ",") + element(i);
  }
  return result + "]";
}

/** Creates the given number of boxes in rows of 20, above the rooms. */
std::string createBoxBatch(const int count)
{
  return jsonArray(count, [](const int i) {
    const auto x = (i % 20) * 64, y = -512 - (i / 20) * 64;
    return fmt::format(
      R"({{"op": "create_box", "min": [{}, {}, 200], "max": [{}, {}, 264], )"
      R"("material": "wall"}})",
      x,
      y,
      x + 32,
      y + 32);
  });
}

/** Sets a property of each of the given number of lights, round robin. */
std::string setPropertyBatch(const int count, const int lights)
{
  return jsonArray(count, [&](const int i) {
    return fmt::format(
      R"({{"op": "set_property", "target": "layer:0/entity:{}", "key": "light", )"
      R"("value": "{}"}})",
      i % lights,
      200 + i);
  });
}

void measure(const std::string& name, const int roomsPerSide)
{
  auto env = CliTestEnvironment{};
  const auto mapPath = env.writeFile("bench.map", roomGrid(roomsPerSide));
  const auto mapArg = arg(mapPath);
  auto editor = FakeEditorHost{mapPath};

  const auto timeRequest = [&](
                             const std::string& what, const auto& args, const auto& in) {
    const auto start = std::chrono::steady_clock::now();
    const auto result = editor.run(args, in);
    const auto elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
    INFO(result.out);
    REQUIRE(result.exitCode == 0);
    std::cout << fmt::format("{} map, {}: {:.1f} ms\n", name, what, elapsed);
  };

  timeRequest("describe", std::vector<std::string>{"describe", mapArg}, std::string{});
  timeRequest(
    "200-op create_box batch",
    std::vector<std::string>{"apply", mapArg, "-"},
    createBoxBatch(200));
  timeRequest(
    "50-op set_property batch",
    std::vector<std::string>{"apply", mapArg, "-"},
    setPropertyBatch(50, roomsPerSide * roomsPerSide));
}

} // namespace

// not run by default; run it in a Release build for meaningful numbers
TEST_CASE("live batch performance", "[.][benchmark]")
{
  measure("small (96 brushes)", 4);
  measure("large (15000 brushes)", 50);
}

} // namespace tb::cli
