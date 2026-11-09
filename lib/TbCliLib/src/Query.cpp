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

#include "cli/Query.h"

#include "cli/CappedList.h"
#include "cli/CliError.h"
#include "cli/Json.h"
#include "cli/MapContents.h"
#include "cli/MapRef.h"
#include "cli/NodeHandles.h"
#include "cli/NodeJson.h"
#include "cli/NodePath.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"
#include "ql/Query.h"

#include "kd/optional_utils.h"
#include "kd/overload.h"

#include <fmt/format.h>

#include <algorithm>
#include <functional>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace tb::cli
{
namespace
{

void collectDescendants(mdl::Node& node, std::vector<NodeOrFace>& matches)
{
  for (auto* child : node.children())
  {
    matches.push_back({child, std::nullopt});
    collectDescendants(*child, matches);
  }
}

/** Sorts the matches into file order, faces by their index within their brush. */
void sortInFileOrder(std::vector<NodeOrFace>& matches, mdl::WorldNode& worldNode)
{
  auto nodes = std::vector<NodeOrFace>{{&worldNode, std::nullopt}};
  collectDescendants(worldNode, nodes);

  auto fileOrder = std::unordered_map<const mdl::Node*, size_t>{};
  for (size_t i = 0; i < nodes.size(); ++i)
  {
    fileOrder[nodes[i].node] = i;
  }

  std::ranges::sort(matches, std::less{}, [&](const auto& match) {
    // a node precedes its faces
    return std::pair{
      fileOrder.at(match.node), match.faceIndex ? *match.faceIndex + 1 : size_t{0}};
  });
}

std::optional<vm::bbox3d> contentsBounds(const mdl::Node& node)
{
  return summarize(collectContents(node)).bounds;
}

/**
 * The bounds the spatial filters test: the summarized bounds of a world, layer or
 * group's contents, or the logical bounds of any other node. Unset for a world, layer or
 * group with nothing in it.
 */
std::optional<vm::bbox3d> nodeBounds(const mdl::Node& node)
{
  return node.accept(
    kdl::overload(
      [](const mdl::WorldNode& worldNode) { return contentsBounds(worldNode); },
      [](const mdl::LayerNode& layerNode) { return contentsBounds(layerNode); },
      [](const mdl::GroupNode& groupNode) { return contentsBounds(groupNode); },
      [](const mdl::EntityNode& entityNode) {
        return std::optional{entityNode.logicalBounds()};
      },
      [](const mdl::BrushNode& brushNode) {
        return std::optional{brushNode.logicalBounds()};
      },
      [](const mdl::PatchNode& patchNode) {
        return std::optional{patchNode.logicalBounds()};
      }));
}

std::optional<vm::bbox3d> matchBounds(const NodeOrFace& match)
{
  if (match.faceIndex)
  {
    const auto& brushNode = static_cast<const mdl::BrushNode&>(*match.node);
    return brushNode.brush().face(*match.faceIndex).bounds();
  }
  return nodeBounds(*match.node);
}

/** Whether the match is the reference itself, or contains it or is contained by it. */
bool isRelated(const NodeOrFace& match, const NodeOrFace& reference)
{
  if (match.node == reference.node)
  {
    // a face is unrelated to its brush's other faces
    return !match.faceIndex || !reference.faceIndex
           || match.faceIndex == reference.faceIndex;
  }
  return match.node->isAncestorOf(*reference.node)
         || reference.node->isAncestorOf(*match.node);
}

Json matchToJson(const NodeOrFace& match, NodeHandleTable* handles)
{
  return match.faceIndex
           ? faceSummaryToJson(
               static_cast<const mdl::BrushNode&>(*match.node), *match.faceIndex, handles)
           : nodeSummaryToJson(*match.node, handles);
}

/** The node or face that `--touching` compares with. */
NodeOrFace resolveTouchingReference(
  mdl::WorldNode& worldNode, const std::string& path, NodeHandleTable* handles)
{
  const auto reference = resolveNodeReference(worldNode, path, handles);
  if (reference.node == &worldNode)
  {
    // the world contains everything, so nothing could touch it
    throw CliError{
      ErrorCode::WrongNodeKind,
      fmt::format("--touching needs a node inside the world, not '{}'", path),
      path};
  }
  return reference;
}

} // namespace

std::vector<NodeOrFace> queryMatches(
  mdl::Map& map, const std::optional<std::string>& queryText)
{
  auto matches = std::vector<NodeOrFace>{};
  if (!queryText)
  {
    collectDescendants(map.worldNode(), matches);
    return matches;
  }

  std::visit(
    kdl::overload(
      [&](const std::vector<mdl::Node*>& nodes) {
        for (auto* node : nodes)
        {
          matches.push_back({node, std::nullopt});
        }
      },
      [&](const std::vector<mdl::BrushFaceHandle>& handles) {
        for (const auto& handle : handles)
        {
          matches.push_back({&handle.node(), handle.faceIndex()});
        }
      }),
    ql::evaluateQuery(map, ql::parseQuery(*queryText)));

  // the query returns nodes in no particular order
  sortInFileOrder(matches, map.worldNode());
  return matches;
}

Json query(const MapRef& mapRef, const QueryOptions& options)
{
  auto& map = mapRef.map;

  // resolve the reference first so that an invalid path fails regardless of the query
  const auto touching =
    options.touching | kdl::optional_transform([&](const auto& path) {
      return resolveTouchingReference(map.worldNode(), path, mapRef.handles);
    });
  const auto touchingBounds = touching | kdl::optional_and_then(matchBounds);

  auto matches = queryMatches(map, options.query);
  std::erase_if(matches, [&](const auto& match) {
    const auto bounds = matchBounds(match);
    if (!bounds)
    {
      return options.within || options.intersects || options.touching;
    }
    if (options.within && !options.within->expand(SpatialTolerance).contains(*bounds))
    {
      return true;
    }
    if (
      options.intersects
      && !options.intersects->expand(SpatialTolerance).intersects(*bounds))
    {
      return true;
    }
    if (touching)
    {
      return !touchingBounds || isRelated(match, *touching)
             || !touchingBounds->expand(SpatialTolerance).intersects(*bounds);
    }
    return false;
  });

  auto result = Json::object();
  result.set("schema", "tbcli.query/1");
  result.set("rev", mapRef.rev);
  if (options.countOnly)
  {
    result.set("total", matches.size());
    return result;
  }

  setPage(result, matches, options.paging, [&](const auto& match) {
    return matchToJson(match, mapRef.handles);
  });
  return result;
}

} // namespace tb::cli
