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

#include "cli/EditOps.h"

#include "cli/CliError.h"
#include "cli/Ops.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/WorldNode.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace tb::cli
{
namespace
{

vm::vec3d toVec3(const Json& json)
{
  return {json[0].asNumber(), json[1].asNumber(), json[2].asNumber()};
}

} // namespace

void createBox(OpContext& context)
{
  const auto& args = context.args;
  const auto min = toVec3(args["min"]);
  const auto max = toVec3(args["max"]);
  for (size_t i = 0; i < 3; ++i)
  {
    if (!(min[i] < max[i]))
    {
      throw CliError{
        ErrorCode::InvalidArgument,
        fmt::format(
          "create_box: min {} must be less than max {} on every axis",
          args["min"].dump(),
          args["max"].dump())};
    }
  }

  const auto& material = args["material"].asString();
  if (material.empty())
  {
    throw CliError{
      ErrorCode::InvalidArgument, "create_box: \"material\" must not be empty"};
  }

  const auto& parent = context.node("parent");

  auto& map = context.map;
  const auto& faceAttribsConfig = map.gameInfo().gameConfig.faceAttribsConfig;
  const auto builder = mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    faceAttribsConfig.defaultUvAttributes,
    faceAttribsConfig.defaultSurfaceAttributes};

  auto brush = builder.createCuboid(vm::bbox3d{min, max}, material);
  if (!brush)
  {
    throw CliError{
      ErrorCode::OpFailed,
      fmt::format(
        "create_box: cannot create the box: {}", std::get<Error>(brush.error()).msg)};
  }

  auto brushNode = std::make_unique<mdl::BrushNode>(std::move(brush).value());
  if (mdl::addNodes(map, {{parent.node, {brushNode.get()}}}).empty())
  {
    throw CliError{ErrorCode::OpFailed, "create_box: cannot add the box to the map"};
  }
  context.outcome.created.push_back(brushNode.release());
}

void deleteNode(OpContext& context)
{
  // a node inside another target is removed with it
  auto targets = std::vector<mdl::Node*>{};
  for (const auto& [node, faceIndex] : context.targets("target"))
  {
    targets.push_back(node);
  }
  if (targets.empty())
  {
    return;
  }

  auto toRemove = targets;
  std::erase_if(
    toRemove, [&](const auto* node) { return node->isDescendantOf(targets); });

  auto ancestors = std::vector<mdl::Node*>{};
  for (auto* target : toRemove)
  {
    for (auto* parent = target->parent(); parent; parent = parent->parent())
    {
      if (std::ranges::find(ancestors, parent) == ancestors.end())
      {
        ancestors.push_back(parent);
      }
    }
  }

  auto& worldNode = context.map.worldNode();
  mdl::removeNodes(context.map, toRemove);

  // Removing the last child of an entity or group also removes the emptied parent.
  auto& removed = context.outcome.removed;
  removed = targets;
  for (auto* ancestor : ancestors)
  {
    if (!isInMap(*ancestor, worldNode))
    {
      removed.push_back(ancestor);
    }
  }
}

} // namespace tb::cli
