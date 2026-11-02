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

#include "cli/Describe.h"

#include "cli/GeometryJson.h"
#include "cli/Json.h"
#include "cli/LoadMap.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

#include "vm/bbox.h"

#include <optional>

namespace tb::cli
{
namespace
{

struct Summary
{
  size_t brushes = 0;
  size_t entities = 0;
  size_t faces = 0;
  std::optional<vm::bbox3d> bounds;

  void addBounds(const vm::bbox3d& nodeBounds)
  {
    bounds = bounds ? vm::merge(*bounds, nodeBounds) : nodeBounds;
  }
};

Summary summarize(const mdl::WorldNode& worldNode)
{
  auto summary = Summary{};
  worldNode.accept(
    kdl::overload(
      [](auto&& thisLambda, const mdl::WorldNode& node) {
        node.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, const mdl::LayerNode& node) {
        node.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, const mdl::GroupNode& node) {
        node.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, const mdl::EntityNode& node) {
        ++summary.entities;
        if (!node.hasChildren())
        {
          // point entity; a brush entity's bounds are its brushes' bounds
          summary.addBounds(node.logicalBounds());
        }
        node.visitChildren(thisLambda);
      },
      [&](const mdl::BrushNode& node) {
        ++summary.brushes;
        summary.faces += node.brush().faceCount();
        summary.addBounds(node.logicalBounds());
      },
      [&](const mdl::PatchNode& node) { summary.addBounds(node.logicalBounds()); }));
  return summary;
}

} // namespace

Json describe(const LoadedMap& loadedMap)
{
  const auto summary = summarize(loadedMap.map->worldNode());

  auto counts = Json::object();
  counts.set("brushes", summary.brushes);
  counts.set("entities", summary.entities);
  counts.set("faces", summary.faces);

  auto result = Json::object();
  result.set("schema", "tbcli.describe/1");
  result.set("rev", loadedMap.rev);
  result.set("counts", std::move(counts));
  result.set("bounds", summary.bounds ? toJson(*summary.bounds) : Json{});
  return result;
}

} // namespace tb::cli
