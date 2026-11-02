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


#include "cli/TransformOps.h"

#include "cli/CliError.h"
#include "cli/GeometryJson.h"
#include "cli/Ops.h"
#include "cli/ParseUtils.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"

#include "vm/bbox.h"
#include "vm/scalar.h"
#include "vm/util.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <utility>
#include <vector>

namespace tb::cli
{
namespace
{

/**
 * The nodes a transform moves: the targets, with a layer standing for the nodes in it,
 * each once, and without the nodes inside other targets, which move with them.
 */
std::vector<mdl::Node*> nodesToTransform(const std::vector<NodeOrFace>& targets)
{
  auto result = std::vector<mdl::Node*>{};
  const auto add = [&](mdl::Node* node) {
    if (std::ranges::find(result, node) == result.end())
    {
      result.push_back(node);
    }
  };

  for (const auto& [node, faceIndex] : targets)
  {
    if (kindOf(*node) == NodeKind::Layer)
    {
      std::ranges::for_each(node->children(), add);
    }
    else
    {
      add(node);
    }
  }

  const auto all = result;
  std::erase_if(result, [&](const auto* node) { return node->isDescendantOf(all); });
  return result;
}

/**
 * The entities that hold the given nodes, without those among them, with their entities
 * before a transform: a transform also changes an entity whose brushes all move.
 */
std::vector<std::pair<mdl::EntityNode*, mdl::Entity>> containingEntities(
  const std::vector<mdl::Node*>& nodes)
{
  auto result = std::vector<std::pair<mdl::EntityNode*, mdl::Entity>>{};
  for (auto* node : nodes)
  {
    auto* entityNode = dynamic_cast<mdl::EntityNode*>(node->parent());
    if (
      entityNode && std::ranges::find(nodes, entityNode) == nodes.end()
      && std::ranges::find(result, entityNode, [](const auto& p) { return p.first; })
           == result.end())
    {
      result.emplace_back(entityNode, entityNode->entity());
    }
  }
  return result;
}

/** The axis that a well-formed axis argument names. */
vm::axis::type axisOf(const Json& axis)
{
  return *parseAxis(axis.asString());
}

/** The given center, or else the center of the bounds. */
vm::vec3d centerOf(const OpContext& context, const vm::bbox3d& bounds)
{
  const auto* center = context.args.find("center");
  return center ? vec3FromJson(*center) : bounds.center();
}

/**
 * Selects the given nodes and sets the editor's alignment lock for its lifetime, and
 * then clears the selection and restores the alignment lock.
 */
class TransformScope
{
private:
  mdl::Map& m_map;
  bool m_oldAlignmentLock;

public:
  TransformScope(mdl::Map& map, const std::vector<mdl::Node*>& nodes, const bool uvLock)
    : m_map{map}
    , m_oldAlignmentLock{map.editorContext().alignmentLock()}
  {
    mdl::deselectAll(m_map);
    mdl::selectNodes(m_map, nodes);
    m_map.editorContext().setAlignmentLock(uvLock);
  }

  ~TransformScope()
  {
    m_map.editorContext().setAlignmentLock(m_oldAlignmentLock);
    mdl::deselectAll(m_map);
  }

  TransformScope(const TransformScope&) = delete;
  TransformScope& operator=(const TransformScope&) = delete;
};

/**
 * Selects the nodes the op transforms and calls `transform` with the map and the bounds
 * of the selection, then clears the selection. Reports the transformed nodes and the
 * entities that changed with them as modified.
 */
void transformTargets(
  OpContext& context,
  const std::function<bool(mdl::Map&, const vm::bbox3d& bounds)>& transform)
{
  const auto nodes = nodesToTransform(context.targets("target"));
  if (nodes.empty())
  {
    return;
  }

  const auto entities = containingEntities(nodes);
  const auto success = [&]() {
    auto& map = context.map;
    const auto scope = TransformScope{map, nodes, context.args["uv_lock"].asBool()};
    const auto bounds = map.selectionBounds();
    if (!bounds)
    {
      throw CliError{
        ErrorCode::OpFailed,
        fmt::format("{}: the targets have no bounds to transform", context.opName)};
    }
    return transform(map, *bounds);
  }();

  if (!success)
  {
    throw CliError{
      ErrorCode::OpFailed,
      fmt::format(
        "{}: cannot transform the targets; a brush would become invalid or leave the "
        "world bounds",
        context.opName)};
  }

  auto& modified = context.outcome.modified;
  std::ranges::copy(nodes, std::back_inserter(modified));
  for (const auto& [entityNode, entity] : entities)
  {
    if (entityNode->entity() != entity)
    {
      modified.push_back(entityNode);
    }
  }
}

} // namespace

void translate(OpContext& context)
{
  const auto delta = vec3FromJson(context.args["delta"]);
  transformTargets(
    context, [&](auto& map, const auto&) { return mdl::translateSelection(map, delta); });
}

void rotate(OpContext& context)
{
  const auto axis = vm::vec3d::axis(axisOf(context.args["axis"]));
  const auto angle = vm::to_radians(context.args["degrees"].asNumber());
  transformTargets(context, [&](auto& map, const auto& bounds) {
    return mdl::rotateSelection(map, centerOf(context, bounds), axis, angle);
  });
}

void scale(OpContext& context)
{
  if (const auto* to = context.args.find("to"))
  {
    const auto newBounds = bboxFromJson(*to);
    transformTargets(context, [&](auto& map, const auto& bounds) {
      return mdl::scaleSelection(map, bounds, newBounds);
    });
    return;
  }

  const auto factors = vec3FromJson(context.args["factors"]);
  transformTargets(context, [&](auto& map, const auto& bounds) {
    return mdl::scaleSelection(map, centerOf(context, bounds), factors);
  });
}

void checkScaleArgs(const Json& args)
{
  const auto* to = args.find("to");
  const auto* factors = args.find("factors");
  if (!to == !factors)
  {
    throw invalidArgument("scale: give either \"to\" or \"factors\"");
  }

  if (to)
  {
    if (args.find("center"))
    {
      throw invalidArgument(
        "scale: \"center\" only applies to \"factors\"; \"to\" gives the new bounds");
    }
    const auto bounds = bboxFromJson(*to);
    for (size_t i = 0; i < 3; ++i)
    {
      if (!(bounds.min[i] < bounds.max[i]))
      {
        throw invalidArgument(
          fmt::format(
            "scale: \"to\" must have min less than max on every axis, not {}",
            to->dump()));
      }
    }
  }
  else if (std::ranges::any_of(factors->asArray(), [](const auto& factor) {
             return factor.asNumber() == 0;
           }))
  {
    throw invalidArgument(
      fmt::format("scale: \"factors\" must not be 0, not {}", factors->dump()));
  }
}

void flip(OpContext& context)
{
  const auto axis = axisOf(context.args["axis"]);
  transformTargets(context, [&](auto& map, const auto& bounds) {
    return mdl::flipSelection(map, centerOf(context, bounds), axis);
  });
}

} // namespace tb::cli
