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

#include "cli/BuiltRooms.h"
#include "cli/EntityJson.h"
#include "cli/Json.h"
#include "cli/MapContents.h"
#include "cli/MapRef.h"
#include "cli/NodeHandles.h"
#include "cli/NodeJson.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

#include <ranges>

namespace tb::cli
{
namespace
{

constexpr auto MaxChildren = size_t{200};

/**
 * `{items, total, truncated}` for the paths of the node's first children, with
 * `items_handles` in a live session.
 */
Json childPaths(const mdl::Node& node, NodeHandleTable* handles)
{
  const auto& children = node.children();

  auto items = NodePathList{handles};
  for (const auto* child : children | std::views::take(MaxChildren))
  {
    items.push(*child);
  }

  auto result = Json::object();
  std::move(items).setOn(result, "items");
  result.set("total", children.size());
  result.set("truncated", children.size() > MaxChildren);
  return result;
}

/** A group with its children and, if it is linked, the paths of its siblings. */
Json groupInFullToJson(
  const mdl::GroupNode& groupNode,
  const mdl::WorldNode& worldNode,
  NodeHandleTable* handles)
{
  auto result = groupToJson(groupNode, handles);

  auto siblings = NodePathList{handles};
  for (const auto* other : collectContents(worldNode).groups)
  {
    if (other != &groupNode && other->linkId() == groupNode.linkId())
    {
      siblings.push(*other);
    }
  }
  if (siblings.size() > 0)
  {
    result.set("link_id", groupNode.linkId());
    std::move(siblings).setOn(result, "siblings");
  }

  result.set("children", childPaths(groupNode, handles));
  return result;
}

/**
 * Adds what a carrier or the group that wraps it says about its built room:
 * `instance_of` (the room's name), `instances` (the number of its linked instances),
 * `params` (or null if malformed) and `status`.
 */
void addBuiltRoom(Json& json, const mdl::Node& node, const mdl::WorldNode& worldNode)
{
  if (
    !dynamic_cast<const mdl::EntityNode*>(&node)
    && !dynamic_cast<const mdl::GroupNode*>(&node))
  {
    return;
  }
  // statuses depend on the other rooms and the connections
  const auto rooms = collectBuiltRooms(worldNode);
  if (const auto* room = builtRoomOf(node, rooms))
  {
    json.set("instance_of", room->name);
    json.set("instances", room->instances.size());
    json.set("params", room->params ? paramsToJson(*room->params) : Json{});
    json.set("status", toString(room->status));
  }
}

Json nodeToJson(
  const mdl::Node& node,
  const mdl::WorldNode& worldNode,
  const bool raw,
  NodeHandleTable* handles)
{
  auto json = node.accept(
    kdl::overload(
      [&](const mdl::WorldNode& world) {
        auto result = entityToJson(world, handles);
        result.set("layers", childPaths(world, handles));
        return result;
      },
      [&](const mdl::LayerNode& layerNode) {
        auto result = layerToJson(layerNode, handles);
        result.set("children", childPaths(layerNode, handles));
        return result;
      },
      [&](const mdl::GroupNode& groupNode) {
        return groupInFullToJson(groupNode, worldNode, handles);
      },
      [&](const mdl::EntityNode& entityNode) {
        auto result = entityToJson(entityNode, handles);
        if (entityNode.hasChildren())
        {
          result.set("brushes", childPaths(entityNode, handles));
        }
        return result;
      },
      [&](const mdl::BrushNode& brushNode) {
        return brushToJson(brushNode, raw, handles);
      },
      [&](const mdl::PatchNode& patchNode) {
        return patchSummaryToJson(patchNode, handles);
      }));
  addBuiltRoom(json, node, worldNode);
  return json;
}

} // namespace

Json show(const MapRef& mapRef, const std::string_view path, const bool raw)
{
  auto& worldNode = mapRef.map.worldNode();
  const auto resolved = resolveNodeReference(worldNode, path, mapRef.handles);

  auto result = Json::object();
  result.set("schema", "tbcli.show/1");
  result.set("rev", mapRef.rev);
  result.set(
    "node",
    resolved.faceIndex ? faceToJson(
                           static_cast<const mdl::BrushNode&>(*resolved.node),
                           *resolved.faceIndex,
                           raw,
                           mapRef.handles)
                       : nodeToJson(*resolved.node, worldNode, raw, mapRef.handles));
  return result;
}

} // namespace tb::cli
