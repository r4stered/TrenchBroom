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
#include "cli/FileUtils.h"
#include "cli/Process.h"

#include "kd/string_utils.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::cli
{
using namespace std::chrono_literals;
using namespace Catch::Matchers;

namespace
{

/** The map text without the lines that differ between saves of the same map. */
std::string withoutVolatileLines(const std::string& text)
{
  auto lines = std::vector<std::string>{};
  for (const auto& line : kdl::str_split(text, "\n"))
  {
    if (
      !line.starts_with("// Generator:") && !line.starts_with("\"_tb_linked_group_id\""))
    {
      lines.push_back(line);
    }
  }
  return kdl::str_join(lines, "\n");
}

} // namespace

// The other tests call tbcli in-process, but the built executable is linked
// differently, and its static initialization can differ: it once saved group properties
// with keys such as "type" instead of "_tb_type", because the keys were initialized
// before the prefix they were made from.
TEST_CASE("tbcli executable")
{
  auto env = CliTestEnvironment{};
  env.writeConfig({}, quakeConfig());

  const auto runTbcli = [&](const std::vector<std::string>& arguments) {
    return runProcess(
      std::filesystem::path{TB_CLI_EXECUTABLE}, arguments, env.path({}), 120s);
  };

  SECTION("saves groups with their TrenchBroom property keys")
  {
    const auto map = env.copyFixture("maps/simple.map", "simple.map");
    const auto batch = env.writeFile(
      "batch.json",
      R"([{"op": "group", "target": ["layer:0/brush:1", "layer:0/entity:1"],
           "name": "Lamp"}])");

    const auto result = runTbcli({"apply", arg(map), arg(batch)});
    REQUIRE(result.exitCode == 0);

    const auto text = env.readFile("simple.map");
    CHECK_THAT(text, ContainsSubstring(R"("_tb_type" "_tb_group")"));
    CHECK_THAT(text, ContainsSubstring(R"("_tb_name" "Lamp")"));
    CHECK_THAT(text, ContainsSubstring(R"("_tb_id" "1")"));
    CHECK_THAT(text, ContainsSubstring(R"("_tb_linked_group_id" ")"));
    CHECK_THAT(text, ContainsSubstring(R"("_tb_group" "1")"));
  }

  SECTION("regenerates compiled/connected_rooms.map")
  {
    const auto map = env.writeFile("rooms.map", R"(// Game: Quake
// Format: Valve
// entity 0
{
"mapversion" "220"
"classname" "worldspawn"
"wad" ""
}
)");
    // the batch that tst_BuiltRooms makes compiled/connected_rooms.map with
    const auto batch = env.writeFile(
      "batch.json",
      R"([{"op": "add_room", "name": "A", "min": [0, 0, 0], "size": [256, 256, 128]},
          {"op": "add_room", "name": "B", "size": [192, 320, 96],
           "place": {"east_of": "A", "gap": 128}},
          {"op": "create_point_entity", "classname": "info_player_start",
           "origin": [128, 128, 24]},
          {"op": "create_point_entity", "classname": "light",
           "origin": [480, 128, 64]},
          {"op": "connect", "from": "A", "to": "B", "height": 96}])");

    const auto result = runTbcli({"apply", arg(map), arg(batch)});
    REQUIRE(result.exitCode == 0);

    const auto generated = env.readFile("rooms.map");
    const auto recorded = *readFile(fixturePath("compiled/connected_rooms.map"));
    CHECK(withoutVolatileLines(generated) == withoutVolatileLines(recorded));
  }
}

} // namespace tb::cli
