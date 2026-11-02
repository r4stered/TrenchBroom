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

#include "cli/MapContents.h"
#include "mdl/BezierPatch.h"
#include "mdl/BrushBuilder.h"
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

#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{

TEST_CASE("MapContents")
{
  const auto builder = mdl::BrushBuilder{mdl::MapFormat::Valve, vm::bbox3d{8192.0}};
  const auto makeBrush = [&](const vm::bbox3d& bounds) {
    return new mdl::BrushNode{builder.createCuboid(bounds, "material") | kdl::value()};
  };

  auto worldNode =
    mdl::WorldNode{mdl::EntityPropertyConfig{}, mdl::Entity{}, mdl::MapFormat::Valve};
  auto& layer = *worldNode.defaultLayer();

  auto* brush = makeBrush({{0, 0, 0}, {64, 64, 16}});
  layer.addChild(brush);

  auto* group = new mdl::GroupNode{mdl::Group{"group"}};
  layer.addChild(group);
  // a point entity without a definition is 16 units wide
  auto* pointEntity =
    new mdl::EntityNode{mdl::Entity{{{"classname", "light"}, {"origin", "100 0 0"}}}};
  group->addChild(pointEntity);
  auto* brushEntity = new mdl::EntityNode{mdl::Entity{{{"classname", "func_door"}}}};
  group->addChild(brushEntity);
  auto* entityBrush = makeBrush({{0, 0, 100}, {8, 8, 200}});
  brushEntity->addChild(entityBrush);

  auto* patch = new mdl::PatchNode{mdl::BezierPatch{
    3,
    3,
    {{-50, 0, 0, 0, 0},
     {-49, 0, 0, 0, 0},
     {-48, 0, 0, 0, 0},
     {-50, 1, 0, 0, 0},
     {-49, 1, 0, 0, 0},
     {-48, 1, 0, 0, 0},
     {-50, 2, 0, 0, 0},
     {-49, 2, 0, 0, 0},
     {-48, 2, 0, 0, 0}},
    "material"}};
  layer.addChild(patch);

  SECTION("collectContents")
  {
    const auto contents = collectContents(worldNode);
    CHECK(contents.groups == std::vector<const mdl::GroupNode*>{group});
    CHECK(
      contents.entities == std::vector<const mdl::EntityNode*>{pointEntity, brushEntity});
    CHECK(contents.brushes == std::vector<const mdl::BrushNode*>{brush, entityBrush});
    CHECK(contents.patches == std::vector<const mdl::PatchNode*>{patch});

    // a group's contents include the group itself
    const auto groupContents = collectContents(*group);
    CHECK(groupContents.groups == std::vector<const mdl::GroupNode*>{group});
    CHECK(groupContents.brushes == std::vector<const mdl::BrushNode*>{entityBrush});
    CHECK(groupContents.patches.empty());
  }

  SECTION("summarize")
  {
    const auto summary = summarize(collectContents(worldNode));
    CHECK(summary.brushes == 2);
    CHECK(summary.entities == 2);
    CHECK(summary.faces == 12);
    CHECK(summary.bounds == vm::bbox3d{{-50, -8, -8}, {108, 64, 200}});

    // a brush entity is bounded by its brushes, not by a point entity's size
    CHECK(
      summarize(collectContents(*brushEntity)).bounds == entityBrush->logicalBounds());

    const auto empty = summarize(collectContents(mdl::LayerNode{mdl::Layer{"empty"}}));
    CHECK(empty.brushes == 0);
    CHECK(empty.bounds == std::nullopt);
  }
}

} // namespace tb::cli
