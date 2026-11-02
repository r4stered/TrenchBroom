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

#include "cli/CsgOps.h"

#include "cli/CliError.h"
#include "cli/Json.h"
#include "cli/NodePath.h"
#include "cli/Ops.h"
#include "cli/Targets.h"
#include "mdl/Brush.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Map.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"

#include "kd/result.h"

#include "vm/constants.h"
#include "vm/scalar.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tb::cli
{
namespace
{

/** The brushes of a Target argument whose rules accept only brushes. */
std::vector<mdl::BrushNode*> brushesOf(
  const OpContext& context, const std::string_view argName)
{
  auto result = std::vector<mdl::BrushNode*>{};
  for (const auto& [node, faceIndex] : context.targets(argName))
  {
    result.push_back(static_cast<mdl::BrushNode*>(node));
  }
  return result;
}

std::vector<mdl::Node*> asNodes(const std::vector<mdl::BrushNode*>& brushes)
{
  return std::vector<mdl::Node*>{brushes.begin(), brushes.end()};
}

/** Selects the given nodes, and clears the selection when it goes out of scope. */
class SelectionScope
{
private:
  mdl::Map& m_map;

public:
  SelectionScope(mdl::Map& map, const std::vector<mdl::Node*>& nodes)
    : m_map{map}
  {
    mdl::deselectAll(m_map);
    mdl::selectNodes(m_map, nodes);
  }

  ~SelectionScope() { mdl::deselectAll(m_map); }

  SelectionScope(const SelectionScope&) = delete;
  SelectionScope& operator=(const SelectionScope&) = delete;
};

/** Sets the editor's UV lock, and restores it when it goes out of scope. */
class UvLockScope
{
private:
  mdl::Map& m_map;
  bool m_oldUvLock;

public:
  UvLockScope(mdl::Map& map, const bool uvLock)
    : m_map{map}
    , m_oldUvLock{map.editorContext().uvLock()}
  {
    m_map.editorContext().setUvLock(uvLock);
  }

  ~UvLockScope() { m_map.editorContext().setUvLock(m_oldUvLock); }

  UvLockScope(const UvLockScope&) = delete;
  UvLockScope& operator=(const UvLockScope&) = delete;
};

/** The selected brushes, which a CSG command selects when it is done, in file order. */
std::vector<mdl::Node*> selectedBrushesInFileOrder(mdl::Map& map)
{
  auto order = std::unordered_map<const mdl::Node*, size_t>{};
  for (const auto* node : nodeAndDescendants(map.worldNode()))
  {
    order.emplace(node, order.size());
  }

  auto result = asNodes(map.selection().brushes);
  std::ranges::sort(result, {}, [&](const auto* node) { return order.at(node); });
  return result;
}

/** An `op_failed` error about the node at the given path. */
CliError opFailed(std::string message, std::string path)
{
  return CliError{ErrorCode::OpFailed, std::move(message), std::move(path)};
}

/**
 * Runs a CSG command of the map library with the given brushes selected, and records
 * what it did: the brushes it selects when it is done as created, and the brushes it
 * replaced and the entities and groups it left empty as removed.
 *
 * @param replaced the brushes that the command replaces
 * @param action what the command does, for messages, as in `hollow`
 * @throw CliError `op_failed` if the command fails, or leaves one of the brushes it
 * should replace in the map
 */
void runCsgCommand(
  OpContext& context,
  const std::vector<mdl::Node*>& replaced,
  const std::string_view action,
  const std::function<bool(mdl::Map&)>& command)
{
  auto& map = context.map;
  const auto& worldNode = map.worldNode();
  const auto ancestors = ancestorsOf(replaced);

  auto created = std::vector<mdl::Node*>{};
  {
    const auto scope = SelectionScope{map, replaced};
    if (!command(map))
    {
      throw CliError{
        ErrorCode::OpFailed,
        fmt::format("{}: cannot {} the brushes", context.opName, action)};
    }
    created = selectedBrushesInFileOrder(map);
  }

  // the editor leaves brushes that it cannot handle as they are
  for (const auto* node : replaced)
  {
    if (isInMap(*node, worldNode))
    {
      const auto path = nodePath(*node);
      throw opFailed(fmt::format("{}: cannot {} {}", context.opName, action, path), path);
    }
  }

  auto& outcome = context.outcome;
  outcome.created = std::move(created);
  appendRemovedNodes(replaced, worldNode, outcome.removed);
  appendRemovedNodes(ancestors, worldNode, outcome.removed);
}

/**
 * Fails unless at least two brushes are given; an op that combines brushes needs that
 * many.
 */
void checkCombinable(
  const std::string_view opName, const std::vector<mdl::BrushNode*>& brushes)
{
  if (brushes.size() == 1)
  {
    const auto path = nodePath(*brushes.front());
    throw opFailed(
      fmt::format(
        "{}: {} is the only brush, but {} needs two or more", opName, path, opName),
      path);
  }
}

/** The brush's vertices, sorted, so that they compare equal however they are ordered. */
std::vector<vm::vec3d> sortedVertices(const mdl::BrushNode& brushNode)
{
  auto result = brushNode.brush().vertexPositions();
  std::ranges::sort(result);
  return result;
}

/** Whether a coordinate of a vertex is not a multiple of the grid. */
bool isOffGrid(const std::vector<vm::vec3d>& vertices, const double grid)
{
  return std::ranges::any_of(vertices, [&](const auto& vertex) {
    for (size_t i = 0; i < 3; ++i)
    {
      if (
        std::abs(vertex[i] - vm::snap(vertex[i], grid))
        > vm::constants<double>::almost_zero())
      {
        return true;
      }
    }
    return false;
  });
}

} // namespace

void hollow(OpContext& context)
{
  const auto brushes = brushesOf(context, "target");
  if (brushes.empty())
  {
    return;
  }

  const auto& thickness = context.args["thickness"];
  for (const auto* brushNode : brushes)
  {
    auto inside = brushNode->brush();
    if (!inside.expand(context.map.worldBounds(), -thickness.asNumber(), true))
    {
      const auto path = nodePath(*brushNode);
      throw opFailed(
        fmt::format(
          "hollow: {} cannot be hollowed with walls {} units thick; it is probably too "
          "thin",
          path,
          thickness.dump()),
        path);
    }
  }

  runCsgCommand(context, asNodes(brushes), "hollow", [&](auto& map) {
    return mdl::csgHollow(map, thickness.asNumber());
  });
}

void checkHollowArgs(const Json& args)
{
  checkPositiveArg("hollow", "thickness", args);
}

void subtract(OpContext& context)
{
  const auto cutters = brushesOf(context, "cutters");
  const auto from = brushesOf(context, "from");

  // only the brushes that a cutter overlaps are cut, and a cutter does not cut itself,
  // for example when from is the layer that holds it
  auto minuends = std::vector<mdl::BrushNode*>{};
  std::ranges::copy_if(from, std::back_inserter(minuends), [&](const auto* brush) {
    return std::ranges::find(cutters, brush) == cutters.end()
           && std::ranges::any_of(cutters, [&](const auto* cutter) {
                return brush->brush().intersects(cutter->brush());
              });
  });
  if (minuends.empty())
  {
    context.outcome.warnings.emplace_back("nothing_cut");
    return;
  }

  const auto keepCutters = context.args["keep_cutters"].asBool();
  auto replaced = asNodes(minuends);
  if (!keepCutters)
  {
    std::ranges::copy(cutters, std::back_inserter(replaced));
  }

  runCsgCommand(context, replaced, "subtract", [&](auto& map) {
    return mdl::csgSubtract(map, minuends, cutters, keepCutters);
  });
}

void merge(OpContext& context)
{
  const auto brushes = brushesOf(context, "target");
  if (brushes.empty())
  {
    return;
  }
  checkCombinable("merge", brushes);

  runCsgCommand(context, asNodes(brushes), "merge", [](auto& map) {
    return mdl::csgConvexMerge(map);
  });
}

void intersect(OpContext& context)
{
  const auto brushes = brushesOf(context, "target");
  if (brushes.empty())
  {
    return;
  }
  checkCombinable("intersect", brushes);

  runCsgCommand(context, asNodes(brushes), "intersect", [](auto& map) {
    // the editor removes the brushes if they share no volume
    if (!mdl::csgIntersect(map) || map.selection().brushes.empty())
    {
      throw CliError{
        ErrorCode::OpFailed,
        "intersect: the brushes share no volume, so their intersection is empty"};
    }
    return true;
  });
}

void snapVertices(OpContext& context)
{
  const auto brushes = brushesOf(context, "target");
  if (brushes.empty())
  {
    return;
  }

  auto& map = context.map;
  const auto& grid = context.args["grid"];
  const auto cannotSnap = [&](const mdl::BrushNode& brushNode) {
    const auto path = nodePath(brushNode);
    return opFailed(
      fmt::format(
        "snap_vertices: cannot snap the vertices of {} to a grid of {}",
        path,
        grid.dump()),
      path);
  };

  auto oldVertices = std::vector<std::vector<vm::vec3d>>{};
  for (const auto* brushNode : brushes)
  {
    if (!brushNode->brush().canSnapVertices(map.worldBounds(), grid.asNumber()))
    {
      throw cannotSnap(*brushNode);
    }
    oldVertices.push_back(sortedVertices(*brushNode));
  }

  {
    // the UV stay aligned to the faces, as with the transform ops' default
    const auto uvLock = UvLockScope{map, true};
    const auto selection = SelectionScope{map, asNodes(brushes)};
    if (!mdl::snapVertices(map, grid.asNumber()))
    {
      throw CliError{ErrorCode::OpFailed, "snap_vertices: cannot snap the vertices"};
    }
  }

  for (size_t i = 0; i < brushes.size(); ++i)
  {
    const auto newVertices = sortedVertices(*brushes[i]);
    if (newVertices != oldVertices[i])
    {
      context.outcome.modified.push_back(brushes[i]);
    }
    else if (isOffGrid(newVertices, grid.asNumber()))
    {
      // the editor logs brushes that it fails to snap, and leaves them as they are
      throw cannotSnap(*brushes[i]);
    }
  }
}

void checkSnapVerticesArgs(const Json& args)
{
  checkPositiveArg("snap_vertices", "grid", args);
}

} // namespace tb::cli
