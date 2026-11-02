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
#include "cli/Json.h"
#include "cli/MapContents.h"
#include "cli/NodeJson.h"
#include "mdl/BezierPatch.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/MapFormat.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"

#include "kd/result.h"

#include <algorithm>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{

TEST_CASE("NodeJson")
{
  const auto builder = mdl::BrushBuilder{mdl::MapFormat::Valve, vm::bbox3d{8192.0}};

  auto worldNode = mdl::WorldNode{
    mdl::EntityPropertyConfig{},
    mdl::Entity{{{"classname", "worldspawn"}}},
    mdl::MapFormat::Valve};
  auto& defaultLayer = *worldNode.defaultLayer();

  // faces in the order left, right, front, back, top, bottom
  auto* brush = new mdl::BrushNode{
    builder.createCuboid(
      vm::bbox3d{{0, 0, 0}, {64, 32, 16}}, "a", "a", "a", "a", "top", "a")
    | kdl::value()};
  defaultLayer.addChild(brush);

  auto* customLayer = new mdl::LayerNode{mdl::Layer{"Custom"}};
  customLayer->setPersistentId(3);
  worldNode.addChild(customLayer);

  auto* group = new mdl::GroupNode{mdl::Group{"Room"}};
  group->setPersistentId(7);
  customLayer->addChild(group);
  auto* brushEntity = new mdl::EntityNode{mdl::Entity{{{"classname", "func_door"}}}};
  group->addChild(brushEntity);
  auto* entityBrush = new mdl::BrushNode{
    builder.createCuboid(vm::bbox3d{{0, 0, 100}, {8, 8, 200}}, "door") | kdl::value()};
  brushEntity->addChild(entityBrush);

  auto* patch = new mdl::PatchNode{mdl::BezierPatch{
    3,
    3,
    {{0, 0, 0, 0, 0},
     {1, 0, 0, 0, 0},
     {2, 0, 0, 0, 0},
     {0, 1, 0, 0, 0},
     {1, 1, 0, 0, 0},
     {2, 1, 0, 0, 0},
     {0, 2, 1, 0, 0},
     {1, 2, 1, 0, 0},
     {2, 2, 1, 0, 0}},
    "patch"}};
  defaultLayer.addChild(patch);

  const auto& faces = brush->brush().faces();
  const auto topFaceIt = std::ranges::find_if(
    faces, [](const auto& face) { return face.normal() == vm::vec3d{0, 0, 1}; });
  REQUIRE(topFaceIt != faces.end());
  const auto topFace = size_t(topFaceIt - faces.begin());
  const auto topFacePath = "layer:0/brush:0/face:" + std::to_string(topFace);

  // rounds the numbers as the output does
  const auto rounded = [](const Json& json) { return parseJsonLiteral(json.dump()); };

  SECTION("countsToJson")
  {
    auto summary = Summary{};
    summary.brushes = 1;
    summary.entities = 2;
    summary.faces = 6;
    CHECK(
      countsToJson(summary)
      == parseJsonLiteral(R"({"brushes": 1, "entities": 2, "faces": 6})"));
  }

  SECTION("boundsToJson")
  {
    auto summary = Summary{};
    CHECK(boundsToJson(summary) == Json{});

    summary.addBounds(vm::bbox3d{{0, 0, 0}, {1, 2, 3}});
    CHECK(
      boundsToJson(summary)
      == parseJsonLiteral(R"({"min": [0, 0, 0], "max": [1, 2, 3]})"));
  }

  SECTION("layerToJson")
  {
    CHECK(layerToJson(*customLayer) == parseJsonLiteral(R"({
      "path": "layer:1", "id": 3, "name": "Custom",
      "bounds": {"min": [0, 0, 100], "max": [8, 8, 200]},
      "counts": {"brushes": 1, "entities": 1, "faces": 6},
      "hidden": false, "locked": false, "omit_from_export": false
    })"));
    CHECK(layerToJson(defaultLayer)["id"] == Json{});
  }

  SECTION("groupToJson")
  {
    CHECK(groupToJson(*group) == parseJsonLiteral(R"({
      "path": "layer:1/group:0", "id": 7, "name": "Room",
      "bounds": {"min": [0, 0, 100], "max": [8, 8, 200]},
      "counts": {"brushes": 1, "entities": 1, "faces": 6},
      "hidden": false, "locked": false
    })"));
  }

  SECTION("brushSummaryToJson")
  {
    CHECK(brushSummaryToJson(*brush) == parseJsonLiteral(R"({
      "path": "layer:0/brush:0",
      "aabb": {"min": [0, 0, 0], "max": [64, 32, 16]},
      "axial_box": true,
      "textures": ["a", "top"],
      "entity": "world"
    })"));
    CHECK(brushSummaryToJson(*entityBrush)["entity"] == Json{"layer:1/group:0/entity:0"});
  }

  SECTION("brushToJson")
  {
    const auto json = brushToJson(*brush, false);
    CHECK(json["path"] == Json{"layer:0/brush:0"});
    CHECK(json["axial_box"] == Json{true});
    CHECK(json["vertices"].size() == 8);
    REQUIRE(json["faces"].size() == 6);
    CHECK(json["faces"][topFace] == faceToJson(*brush, topFace, false));
    CHECK(json["faces"][0].find("points") == nullptr);

    CHECK(brushToJson(*brush, true)["faces"][0].find("points") != nullptr);
  }

  SECTION("faceSummaryToJson")
  {
    CHECK(rounded(faceSummaryToJson(*brush, topFace)) == [&]() {
      auto expected = Json::object();
      expected.set("path", topFacePath);
      expected.set("normal", parseJsonLiteral("[0, 0, 1]"));
      expected.set("texture", "top");
      return expected;
    }());
  }

  SECTION("faceToJson")
  {
    const auto json = rounded(faceToJson(*brush, topFace, false));
    CHECK(json["path"] == Json{topFacePath});
    CHECK(json["normal"] == parseJsonLiteral("[0, 0, 1]"));
    CHECK(json["texture"] == Json{"top"});
    CHECK(json["uv"]["offset"] == parseJsonLiteral("[0, 0]"));
    CHECK(json["uv"]["scale"] == parseJsonLiteral("[1, 1]"));
    CHECK(json["uv"]["rotation"] == Json{0});
    CHECK(json["uv"]["u_axis"].size() == 3);
    CHECK(json["uv"]["v_axis"].size() == 3);
    CHECK(json["vertices"].size() == 4);
    CHECK(json.find("points") == nullptr);

    const auto raw = rounded(faceToJson(*brush, topFace, true));
    REQUIRE(raw["points"].size() == 3);
    for (const auto& point : raw["points"].asArray())
    {
      // every plane point of the top face lies in its plane
      CHECK(point[2] == Json{16});
    }
  }

  SECTION("patchSummaryToJson")
  {
    CHECK(patchSummaryToJson(*patch) == parseJsonLiteral(R"({
      "path": "layer:0/patch:0",
      "aabb": {"min": [0, 0, 0], "max": [2, 2, 1]},
      "texture": "patch"
    })"));
  }

  SECTION("nodeSummaryToJson")
  {
    CHECK(nodeSummaryToJson(worldNode)["path"] == Json{"world"});
    CHECK(nodeSummaryToJson(worldNode)["classname"] == Json{"worldspawn"});
    CHECK(nodeSummaryToJson(*customLayer) == layerToJson(*customLayer));
    CHECK(nodeSummaryToJson(*group) == groupToJson(*group));
    CHECK(nodeSummaryToJson(*brushEntity)["classname"] == Json{"func_door"});
    CHECK(nodeSummaryToJson(*brush) == brushSummaryToJson(*brush));
    CHECK(nodeSummaryToJson(*patch) == patchSummaryToJson(*patch));

    // entity summaries are bounded
    brushEntity->setEntity(
      mdl::Entity{{{"classname", "func_door"}, {"message", std::string(1000, 'm')}}});
    const auto json = nodeSummaryToJson(*brushEntity);
    CHECK(json["properties"]["message"].asString().size() == 256);
    CHECK(json["properties_truncated"] == Json{true});
  }
}

} // namespace tb::cli
