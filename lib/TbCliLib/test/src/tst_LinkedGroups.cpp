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

/*
 * The snapshot is exercised through apply, on maps/structure.map, whose layer:0/group:1
 * and layer:0/group:2 are linked, each holding a brush and a light.
 */
TEST_CASE("LinkedGroupSnapshot")
{
  auto fixture = EditFixture{"maps/structure.map"};
  const auto& map = fixture.map;

  SECTION("recordLinkedUpdates")
  {
    SECTION("the linked copies of a modified node are modified")
    {
      const auto json = runJson(
        {"translate", map, "--target", "layer:0/group:2/brush:0", "--delta", "0,0,8"});
      const auto& op = json["ops"][0];
      CHECK(
        op["modified"]
        == parseJsonLiteral(R"(["layer:0/group:2/brush:0", "layer:0/group:1/brush:0"])"));
      CHECK(
        op["instances_affected"]
        == parseJsonLiteral(R"(["layer:0/group:1", "layer:0/group:2"])"));
      CHECK(
        runJson({"show", map, "layer:0/group:1/brush:0"})["node"]["aabb"]
        == parseJsonLiteral(R"({"min": [0, 200, 8], "max": [32, 232, 40]})"));
    }

    SECTION("the linked copies of a created node are modified")
    {
      const auto json = parseOutput(fixture.apply(R"([{
        "op": "create_box", "min": [0, 200, 40], "max": [8, 208, 48], "material": "knob",
        "parent": "layer:0/group:1"
      }])"));
      const auto& op = json["ops"][0];
      CHECK(op["created"] == parseJsonLiteral(R"(["layer:0/group:1/brush:1"])"));
      CHECK(op["modified"] == parseJsonLiteral(R"(["layer:0/group:2/brush:1"])"));
    }

    SECTION("the linked copies of a removed node are removed")
    {
      const auto json = runJson({"delete", map, "--target", "layer:0/group:1/entity:0"});
      const auto& op = json["ops"][0];
      CHECK(
        op["removed"]
        == parseJsonLiteral(
          R"(["layer:0/group:1/entity:0", "layer:0/group:2/entity:0"])"));
      CHECK(op["modified"] == parseJsonLiteral("[]"));
    }

    SECTION("CSG in a linked group replaces the brushes of its linked copies")
    {
      const auto json = runJson(
        {"hollow", map, "--target", "layer:0/group:1/brush:0", "--thickness", "4"});
      const auto& op = json["ops"][0];
      CHECK(op["created"].size() == 6);
      CHECK(op["modified"].size() == 6);
      CHECK_THAT(
        op["modified"][0].asString(), Catch::Matchers::StartsWith("layer:0/group:2/"));
      CHECK(
        op["removed"]
        == parseJsonLiteral(R"(["layer:0/group:1/brush:0", "layer:0/group:2/brush:0"])"));
    }

    SECTION("a face op in a linked group modifies the brushes of its linked copies")
    {
      const auto json = runJson(
        {"set_material",
         map,
         "--target",
         "layer:0/group:1/brush:0",
         "--faces",
         "+z",
         "--material",
         "lid"});
      CHECK(
        json["ops"][0]["modified"]
        == parseJsonLiteral(R"(["layer:0/group:1/brush:0", "layer:0/group:2/brush:0"])"));
      CHECK(
        showNode(map, "layer:0/group:2/brush:0")["textures"]
        == parseJsonLiteral(R"(["box", "lid"])"));
    }

    SECTION("linked groups nested in the same group are told apart")
    {
      // layer:0/group:1/group:0 and layer:0/group:1/group:1 are linked to each other,
      // and copied into layer:0/group:2
      REQUIRE(
        fixture
          .apply(R"([
            {"op": "group", "target": "layer:0/group:1/brush:0", "name": "Inner",
             "as": "$inner"},
            {"op": "linked_duplicate", "target": "$inner", "offset": [0, 0, 64]}
          ])")
          .exitCode
        == 0);

      const auto json = runJson({"delete", map, "--target", "layer:0/group:1/group:0"});
      const auto& op = json["ops"][0];
      CHECK(
        op["removed"]
        == parseJsonLiteral(R"(["layer:0/group:1/group:0", "layer:0/group:2/group:0"])"));
      CHECK(
        op["instances_affected"]
        == parseJsonLiteral(R"(["layer:0/group:1", "layer:0/group:2"])"));
      CHECK(
        showNode(map, "layer:0/group:2/group:0/brush:0")["aabb"]
        == parseJsonLiteral(R"({"min": [256, 200, 64], "max": [288, 232, 96]})"));

      SECTION("an edit in a nested linked group affects the copies inside copies")
      {
        const auto edit = runJson(
          {"set_material",
           map,
           "--target",
           "layer:0/group:1/group:0/brush:0",
           "--material",
           "lid"});
        CHECK(
          edit["ops"][0]["instances_affected"]
          == parseJsonLiteral(R"(["layer:0/group:1", "layer:0/group:1/group:0",
            "layer:0/group:2", "layer:0/group:2/group:0"])"));
      }
    }

    SECTION("edits that do not reach into a linked group affect no instances")
    {
      const auto target = GENERATE("layer:0/group:2", "layer:0/brush:1");
      CAPTURE(target);
      const auto json =
        runJson({"translate", map, "--target", target, "--delta", "0,0,8"});
      CHECK(json["ops"][0].find("instances_affected") == nullptr);
    }
  }
}

} // namespace tb::cli
