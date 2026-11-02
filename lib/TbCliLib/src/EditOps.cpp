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

CliError wrongNodeKind(const std::string& message, const std::string& path)
{
  return CliError{ErrorCode::WrongNodeKind, message, path};
}

/** Whether the node at the given path can hold brushes: a layer, group or brush entity.
 */
bool canHoldBrushes(const NodeOrFace& nodeOrFace)
{
  if (nodeOrFace.faceIndex)
  {
    return false;
  }

  const auto kind = kindOf(*nodeOrFace.node);
  // an entity without brushes is a point entity
  return canContain(kind, NodeKind::Brush)
         && (kind != NodeKind::Entity || nodeOrFace.node->hasChildren());
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

  const auto& parentPath = args["parent"].asString();
  const auto& parent = context.node("parent");
  if (!canHoldBrushes(parent))
  {
    throw wrongNodeKind(
      fmt::format(
        "create_box: the parent {} must be a layer, a group or a brush entity",
        parentPath),
      parentPath);
  }

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
  const auto& targetPath = context.args["target"].asString();
  const auto& [target, faceIndex] = context.node("target");

  // what a layer can contain can be deleted: groups, entities, brushes and patches
  if (faceIndex || !canContain(NodeKind::Layer, kindOf(*target)))
  {
    throw wrongNodeKind(
      fmt::format(
        "delete: {} must be a group, an entity, a brush or a patch", targetPath),
      targetPath);
  }

  auto ancestors = std::vector<mdl::Node*>{};
  for (auto* parent = target->parent(); parent; parent = parent->parent())
  {
    ancestors.push_back(parent);
  }

  auto& worldNode = context.map.worldNode();
  mdl::removeNodes(context.map, {target});

  // Removing the last child of an entity or group also removes the emptied parent.
  context.outcome.removed.push_back(target);
  for (auto* ancestor : ancestors)
  {
    if (ancestor == &worldNode || ancestor->isDescendantOf(worldNode))
    {
      break;
    }
    context.outcome.removed.push_back(ancestor);
  }
}

} // namespace tb::cli
