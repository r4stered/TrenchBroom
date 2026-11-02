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

#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

namespace tb::cli
{

MapContents collectContents(const mdl::Node& node)
{
  auto contents = MapContents{};
  node.accept(
    kdl::overload(
      [](auto&& thisLambda, const mdl::WorldNode& worldNode) {
        worldNode.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, const mdl::LayerNode& layerNode) {
        layerNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, const mdl::GroupNode& groupNode) {
        contents.groups.push_back(&groupNode);
        groupNode.visitChildren(thisLambda);
      },
      [&](auto&& thisLambda, const mdl::EntityNode& entityNode) {
        contents.entities.push_back(&entityNode);
        entityNode.visitChildren(thisLambda);
      },
      [&](const mdl::BrushNode& brushNode) { contents.brushes.push_back(&brushNode); },
      [&](const mdl::PatchNode& patchNode) { contents.patches.push_back(&patchNode); }));
  return contents;
}

void Summary::addBounds(const vm::bbox3d& nodeBounds)
{
  bounds = bounds ? vm::merge(*bounds, nodeBounds) : nodeBounds;
}

Summary summarize(const MapContents& contents)
{
  auto summary = Summary{};
  summary.brushes = contents.brushes.size();
  summary.entities = contents.entities.size();
  for (const auto* brushNode : contents.brushes)
  {
    summary.faces += brushNode->brush().faceCount();
    summary.addBounds(brushNode->logicalBounds());
  }
  for (const auto* entityNode : contents.entities)
  {
    // a brush entity's bounds are its brushes' bounds
    if (!entityNode->hasChildren())
    {
      summary.addBounds(entityNode->logicalBounds());
    }
  }
  for (const auto* patchNode : contents.patches)
  {
    summary.addBounds(patchNode->logicalBounds());
  }
  return summary;
}

} // namespace tb::cli
