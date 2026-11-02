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

#include "cli/Show.h"

#include "cli/CappedList.h"
#include "cli/EntityJson.h"
#include "cli/Json.h"
#include "cli/LoadMap.h"
#include "cli/MapContents.h"
#include "cli/NodeJson.h"
#include "cli/NodePath.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

#include <vector>

namespace tb::cli
{
namespace
{

constexpr auto MaxChildren = size_t{200};

Json childPaths(const mdl::Node& node)
{
  return capped(node.children(), MaxChildren, [](const auto* child) {
    return Json{nodePath(*child)};
  });
}

/** A group with its children and, if it is linked, the paths of its siblings. */
Json groupInFullToJson(const mdl::GroupNode& groupNode, const mdl::WorldNode& worldNode)
{
  auto result = groupToJson(groupNode);

  auto siblings = Json::array();
  for (const auto* other : collectContents(worldNode).groups)
  {
    if (other != &groupNode && other->linkId() == groupNode.linkId())
    {
      siblings.push(nodePath(*other));
    }
  }
  if (siblings.size() > 0)
  {
    result.set("link_id", groupNode.linkId());
    result.set("siblings", std::move(siblings));
  }

  result.set("children", childPaths(groupNode));
  return result;
}

Json nodeToJson(const mdl::Node& node, const mdl::WorldNode& worldNode, const bool raw)
{
  return node.accept(
    kdl::overload(
      [](const mdl::WorldNode& world) {
        auto result = entityToJson(world);
        result.set("layers", childPaths(world));
        return result;
      },
      [](const mdl::LayerNode& layerNode) {
        auto result = layerToJson(layerNode);
        result.set("children", childPaths(layerNode));
        return result;
      },
      [&](const mdl::GroupNode& groupNode) {
        return groupInFullToJson(groupNode, worldNode);
      },
      [](const mdl::EntityNode& entityNode) {
        auto result = entityToJson(entityNode);
        if (entityNode.hasChildren())
        {
          result.set("brushes", childPaths(entityNode));
        }
        return result;
      },
      [&](const mdl::BrushNode& brushNode) { return brushToJson(brushNode, raw); },
      [](const mdl::PatchNode& patchNode) { return patchSummaryToJson(patchNode); }));
}

} // namespace

Json show(LoadedMap& loadedMap, const std::string_view path, const bool raw)
{
  auto& worldNode = loadedMap.map->worldNode();
  const auto resolved = resolveNodePath(worldNode, path);

  auto result = Json::object();
  result.set("schema", "tbcli.show/1");
  result.set("rev", loadedMap.rev);
  result.set(
    "node",
    resolved.faceIndex
      ? faceToJson(
          static_cast<const mdl::BrushNode&>(*resolved.node), *resolved.faceIndex, raw)
      : nodeToJson(*resolved.node, worldNode, raw));
  return result;
}

} // namespace tb::cli
