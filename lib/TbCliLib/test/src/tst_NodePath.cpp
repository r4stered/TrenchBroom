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

#include "cli/CliError.h"
#include "cli/NodePath.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/MapFormat.h"
#include "mdl/WorldNode.h"

#include "kd/result.h"

#include <memory>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{

TEST_CASE("NodePath")
{
  const auto worldBounds = vm::bbox3d{8192.0};
  const auto builder = mdl::BrushBuilder{mdl::MapFormat::Valve, worldBounds};
  const auto makeBrush = [&]() {
    return std::make_unique<mdl::BrushNode>(
      builder.createCube(64.0, "material") | kdl::value());
  };

  auto worldNode =
    mdl::WorldNode{mdl::EntityPropertyConfig{}, mdl::Entity{}, mdl::MapFormat::Valve};
  auto& defaultLayer = *worldNode.defaultLayer();
  auto& customLayer =
    worldNode.addChild(std::make_unique<mdl::LayerNode>(mdl::Layer{"Custom"}));

  // indices count the preceding siblings of the same kind
  auto& brush0 = defaultLayer.addChild(makeBrush());
  auto& entity0 = defaultLayer.addChild(std::make_unique<mdl::EntityNode>(mdl::Entity{}));
  auto& brush1 = defaultLayer.addChild(makeBrush());

  auto& group0 =
    customLayer.addChild(std::make_unique<mdl::GroupNode>(mdl::Group{"group"}));
  auto& group1 =
    customLayer.addChild(std::make_unique<mdl::GroupNode>(mdl::Group{"group"}));
  auto& nestedGroup =
    group1.addChild(std::make_unique<mdl::GroupNode>(mdl::Group{"nested"}));
  auto& brushEntity =
    nestedGroup.addChild(std::make_unique<mdl::EntityNode>(mdl::Entity{}));
  auto& entityBrush = brushEntity.addChild(makeBrush());

  SECTION("nodePath")
  {
    CHECK(nodePath(worldNode) == "world");
    CHECK(nodePath(defaultLayer) == "layer:0");
    CHECK(nodePath(customLayer) == "layer:1");
    CHECK(nodePath(brush0) == "layer:0/brush:0");
    CHECK(nodePath(entity0) == "layer:0/entity:0");
    CHECK(nodePath(brush1) == "layer:0/brush:1");
    CHECK(nodePath(group0) == "layer:1/group:0");
    CHECK(nodePath(group1) == "layer:1/group:1");
    CHECK(nodePath(nestedGroup) == "layer:1/group:1/group:0");
    CHECK(nodePath(brushEntity) == "layer:1/group:1/group:0/entity:0");
    CHECK(nodePath(entityBrush) == "layer:1/group:1/group:0/entity:0/brush:0");
  }

  SECTION("nodePaths")
  {
    const auto paths = nodePaths(worldNode);
    CHECK(paths.size() == 11);
    CHECK(paths.at(&worldNode) == "world");
    CHECK(paths.at(&defaultLayer) == "layer:0");
    CHECK(paths.at(&customLayer) == "layer:1");
    CHECK(paths.at(&brush0) == "layer:0/brush:0");
    CHECK(paths.at(&entity0) == "layer:0/entity:0");
    CHECK(paths.at(&brush1) == "layer:0/brush:1");
    CHECK(paths.at(&group0) == "layer:1/group:0");
    CHECK(paths.at(&group1) == "layer:1/group:1");
    CHECK(paths.at(&nestedGroup) == "layer:1/group:1/group:0");
    CHECK(paths.at(&brushEntity) == "layer:1/group:1/group:0/entity:0");
    CHECK(paths.at(&entityBrush) == "layer:1/group:1/group:0/entity:0/brush:0");
  }

  SECTION("facePath")
  {
    CHECK(facePath(static_cast<mdl::BrushNode&>(brush1), 0) == "layer:0/brush:1/face:0");
    CHECK(
      facePath(static_cast<mdl::BrushNode&>(entityBrush), 5)
      == "layer:1/group:1/group:0/entity:0/brush:0/face:5");
  }

  SECTION("resolveNodePath")
  {
    // every node's path resolves to the node
    for (auto* node : std::vector<mdl::Node*>{
           &worldNode,
           &defaultLayer,
           &customLayer,
           &brush0,
           &entity0,
           &brush1,
           &group0,
           &group1,
           &nestedGroup,
           &brushEntity,
           &entityBrush})
    {
      CAPTURE(nodePath(*node));
      const auto resolved = resolveNodePath(worldNode, nodePath(*node));
      CHECK(resolved.node == node);
      CHECK(resolved.faceIndex == std::nullopt);
    }

    const auto face =
      resolveNodePath(worldNode, facePath(static_cast<mdl::BrushNode&>(entityBrush), 5));
    CHECK(face.node == &entityBrush);
    CHECK(face.faceIndex == 5);

    CHECK_THROWS_AS(resolveNodePath(worldNode, "layer:0/brush:2"), CliError);
    CHECK_THROWS_AS(resolveNodePath(worldNode, "layer:0/brush:0/face:6"), CliError);
    CHECK_THROWS_AS(resolveNodePath(worldNode, "layer:0/group:0"), CliError);
  }
}

} // namespace tb::cli
