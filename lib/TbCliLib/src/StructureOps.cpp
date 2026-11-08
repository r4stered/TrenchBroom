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

#include "cli/StructureOps.h"

#include "cli/CliError.h"
#include "cli/EditOps.h"
#include "cli/EditorScopes.h"
#include "cli/FileUtils.h"
#include "cli/GeometryJson.h"
#include "cli/Json.h"
#include "cli/LoadMap.h"
#include "cli/NodePath.h"
#include "cli/Ops.h"
#include "cli/Targets.h"
#include "mdl/GroupNode.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/LinkedGroupUtils.h"
#include "mdl/Map.h"
#include "mdl/Map_CopyPaste.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_NodeLocking.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"

#include "kd/ranges/to.h"
#include "kd/result.h"

#include "vm/vec.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tb::cli
{
namespace
{

/** The nodes of the Target argument `target`, each once. */
std::vector<mdl::Node*> targetNodes(const OpContext& context)
{
  auto result = std::vector<mdl::Node*>{};
  for (const auto& [node, faceIndex] : context.targets("target"))
  {
    result.push_back(node);
  }
  return result;
}

/** The given nodes without those inside others among them, which go along with those. */
std::vector<mdl::Node*> outermost(std::vector<mdl::Node*> nodes)
{
  const auto all = nodes;
  std::erase_if(nodes, [&](const auto* node) { return node->isDescendantOf(all); });
  return nodes;
}

/**
 * Moves the given nodes by the op's `offset`, if it has one other than 0, keeping their
 * materials aligned.
 *
 * @throw CliError `op_failed` if a brush would leave the world bounds
 */
void translateByOffset(OpContext& context, const std::vector<mdl::Node*>& nodes)
{
  const auto* offset = context.args.find("offset");
  if (!offset || nodes.empty())
  {
    return;
  }
  const auto delta = vec3FromJson(*offset);
  if (delta == vm::vec3d{0, 0, 0})
  {
    return;
  }

  auto& map = context.map;
  const auto success = [&]() {
    const auto selection = SelectionScope{map, nodes};
    const auto alignmentLock = AlignmentLockScope{map, true};
    return mdl::translateSelection(map, delta);
  }();

  if (!success)
  {
    throw opFailed(
      context.opName,
      "cannot move the new nodes by the offset; they would leave the world bounds");
  }
}

/**
 * The node that moves when the given node changes layer: a brush or patch of a brush
 * entity moves with its entity.
 */
mdl::Node* nodeMovingToLayer(mdl::Node& node)
{
  auto* parent = node.parent();
  const auto isGeometry =
    kindOf(node) == NodeKind::Brush || kindOf(node) == NodeKind::Patch;
  return isGeometry && kindOf(*parent) == NodeKind::Entity ? parent : &node;
}

/**
 * Whether the group is linked to a group that is not among the given ones, so that it
 * can be separated from it.
 */
bool isSeparable(
  mdl::WorldNode& worldNode,
  const mdl::GroupNode& groupNode,
  const std::vector<mdl::Node*>& groups)
{
  const auto linkedGroups =
    mdl::collectGroupsWithLinkId({&worldNode}, groupNode.linkId());
  return std::ranges::any_of(linkedGroups, [&](const auto* linkedGroup) {
    return std::ranges::find(groups, linkedGroup) == groups.end();
  });
}

/** The text of the fragment that `import` pastes, from `text` or from `file`. */
std::string fragmentText(const OpContext& context)
{
  if (const auto* text = context.args.find("text"))
  {
    return text->asString();
  }

  const auto path = std::filesystem::absolute(context.args["file"].asString());
  auto contents = readFile(path);
  if (!contents)
  {
    throw opFailed(context.opName, fmt::format("cannot read {}", path));
  }
  return std::move(*contents);
}

} // namespace

void duplicate(OpContext& context)
{
  const auto nodes = outermost(targetNodes(context));
  if (nodes.empty())
  {
    return;
  }

  auto originalParents = std::unordered_set<const mdl::Node*>{};
  for (const auto* node : nodes)
  {
    originalParents.insert(node->parent());
  }

  auto& map = context.map;
  const auto copies = [&]() {
    const auto scope = SelectionScope{map, nodes};
    mdl::duplicateSelectedNodes(map);
    return map.selection().nodes;
  }();
  if (copies.empty())
  {
    throw opFailed(context.opName, "cannot copy the targets");
  }

  // a copy of a brush of a brush entity comes in a new entity
  auto& created = context.outcome.created;
  for (auto* copy : copies)
  {
    auto* parent = copy->parent();
    const auto inNewEntity =
      kindOf(*parent) == NodeKind::Entity && !originalParents.contains(parent);
    appendOnce(created, inNewEntity ? parent : copy);
  }

  translateByOffset(context, copies);
}

void group(OpContext& context)
{
  const auto nodes = outermost(targetNodes(context));
  if (nodes.empty())
  {
    return;
  }

  auto& map = context.map;
  auto* groupNode = [&]() {
    const auto scope = SelectionScope{map, nodes};
    return mdl::groupSelectedNodes(map, context.args["name"].asString());
  }();
  if (!groupNode)
  {
    throw opFailed(context.opName, "cannot group the targets");
  }

  context.outcome.created.push_back(groupNode);
  context.outcome.modified = groupNode->children() | kdl::ranges::to<std::vector>();
}

void ungroup(OpContext& context)
{
  const auto groups = targetNodes(context);
  if (groups.empty())
  {
    return;
  }

  // the contents of the groups, except for groups that are ungrouped themselves
  auto contents = std::vector<mdl::Node*>{};
  for (auto* groupNode : groups)
  {
    for (auto* child : groupNode->children())
    {
      if (std::ranges::find(groups, child) == groups.end())
      {
        contents.push_back(child);
      }
    }
  }

  auto& map = context.map;
  {
    const auto scope = SelectionScope{map, groups};
    mdl::ungroupSelectedNodes(map);
  }

  const auto& worldNode = map.worldNode();
  for (auto* groupNode : groups)
  {
    if (isInMap(*groupNode, worldNode))
    {
      const auto path = nodePath(*groupNode);
      throw opFailed(context.opName, fmt::format("cannot ungroup {}", path), path);
    }
  }

  context.outcome.removed = groups;
  for (auto* node : contents)
  {
    if (isInMap(*node, worldNode))
    {
      context.outcome.modified.push_back(node);
    }
  }
}

void renameGroup(OpContext& context)
{
  const auto& name = context.args["name"].asString();
  auto groups = targetNodes(context);
  std::erase_if(groups, [&](const auto* node) { return node->name() == name; });
  if (groups.empty())
  {
    return;
  }

  auto& map = context.map;
  {
    const auto scope = SelectionScope{map, groups};
    mdl::renameSelectedGroups(map, name);
  }
  context.outcome.modified = groups;
}

void createLayer(OpContext& context)
{
  auto& map = context.map;
  auto layer = mdl::Layer{context.args["name"].asString()};

  // below the other layers, as the editor's layer list adds it
  const auto customLayers = map.worldNode().customLayersUserSorted();
  layer.setSortIndex(
    !customLayers.empty() ? customLayers.back()->layer().sortIndex() + 1 : 0);

  auto* layerNode = mdl::addNode(
    map, map.worldNode(), std::make_unique<mdl::LayerNode>(std::move(layer)));
  if (!layerNode)
  {
    throw opFailed(context.opName, "cannot add the layer to the map");
  }
  context.outcome.created.push_back(layerNode);
}

void renameLayer(OpContext& context)
{
  auto& layerNode = static_cast<mdl::LayerNode&>(*context.node("target").node);
  if (&layerNode == context.map.worldNode().defaultLayer())
  {
    throw opFailed(
      context.opName,
      "the default layer's name is not saved in the map, so it cannot be renamed",
      nodePath(layerNode));
  }

  const auto& name = context.args["name"].asString();
  if (layerNode.name() != name)
  {
    mdl::renameLayer(context.map, &layerNode, name);
    context.outcome.modified.push_back(&layerNode);
  }
}

void moveToLayer(OpContext& context)
{
  auto& layerNode = static_cast<mdl::LayerNode&>(*context.node("layer").node);

  auto nodes = std::vector<mdl::Node*>{};
  for (auto* node : outermost(targetNodes(context)))
  {
    if (const auto* groupNode = mdl::findContainingGroup(*node))
    {
      const auto path = nodePath(*node);
      throw opFailed(
        context.opName,
        fmt::format(
          "{} is inside the group {}; move the group instead",
          path,
          nodePath(*groupNode)),
        path);
    }
    if (
      auto* moving = nodeMovingToLayer(*node);
      mdl::findContainingLayer(*moving) != &layerNode)
    {
      appendOnce(nodes, moving);
    }
  }
  if (nodes.empty())
  {
    return;
  }

  auto& map = context.map;
  {
    const auto scope = SelectionScope{map, nodes};
    mdl::moveSelectedNodesToLayer(map, &layerNode);
  }

  for (auto* node : nodes)
  {
    if (mdl::findContainingLayer(*node) != &layerNode)
    {
      const auto path = nodePath(*node);
      throw opFailed(context.opName, fmt::format("cannot move {}", path), path);
    }
  }
  context.outcome.modified = nodes;
}

void setLayerOmitFromExport(OpContext& context)
{
  const auto omit = context.args["omit"].asBool();
  for (auto* node : targetNodes(context))
  {
    auto& layerNode = static_cast<mdl::LayerNode&>(*node);
    if (layerNode.layer().omitFromExport() != omit)
    {
      mdl::setOmitLayerFromExport(context.map, &layerNode, omit);
      context.outcome.modified.push_back(&layerNode);
    }
  }
}

void moveToEntity(OpContext& context)
{
  auto& entityNode = *context.node("entity").node;

  auto nodes = targetNodes(context);
  std::erase_if(nodes, [&](const auto* node) { return node->parent() == &entityNode; });
  if (nodes.empty())
  {
    return;
  }

  auto& map = context.map;
  const auto ancestors = ancestorsOf(nodes);
  const auto moved = mdl::moveToEntity(map, nodes, entityNode);
  mdl::deselectAll(map);
  if (!moved)
  {
    const auto path = nodePath(entityNode);
    throw opFailed(
      context.opName, fmt::format("cannot move the brushes into {}", path), path);
  }

  recordMoveOutcome(nodes, ancestors, map.worldNode(), context.outcome);
}

void lock(OpContext& context)
{
  auto layers = targetNodes(context);
  std::erase_if(layers, [](const auto* node) { return node->locked(); });
  if (layers.empty())
  {
    return;
  }

  mdl::lockNodes(context.map, layers);
  context.outcome.modified = layers;
}

void linkedDuplicate(OpContext& context)
{
  auto& map = context.map;
  for (auto* node : targetNodes(context))
  {
    auto* copy = [&]() {
      const auto scope = SelectionScope{map, {node}};
      return mdl::createLinkedDuplicate(map);
    }();
    if (!copy)
    {
      const auto path = nodePath(*node);
      throw opFailed(context.opName, fmt::format("cannot copy {}", path), path);
    }
    context.outcome.created.push_back(copy);
  }

  translateByOffset(context, context.outcome.created);
}

void separateLinked(OpContext& context)
{
  const auto groups = targetNodes(context);
  if (groups.empty())
  {
    return;
  }

  auto& map = context.map;
  for (const auto* node : groups)
  {
    if (!isSeparable(map.worldNode(), static_cast<const mdl::GroupNode&>(*node), groups))
    {
      const auto path = nodePath(*node);
      throw opFailed(
        context.opName,
        fmt::format("{} is not linked to any group other than the targets", path),
        path);
    }
  }

  {
    const auto scope = SelectionScope{map, groups};
    mdl::separateSelectedLinkedGroups(map);
  }
  context.outcome.modified = groups;
}

void importFragment(OpContext& context)
{
  const auto text = fragmentText(context);

  auto& map = context.map;
  auto problems = std::vector<LoadProblem>{};
  auto status = ProblemCollectingParserStatus{map.logger(), text, problems};
  auto pasted = mdl::pasteNodes(map, text, *context.node("parent").node, status);
  mdl::deselectAll(map);

  // the batch fails, so the half-pasted fragment is never written
  if (!problems.empty())
  {
    const auto& problem = problems.front();
    throw opFailed(
      context.opName,
      fmt::format(
        "the fragment does not parse exactly{}: {}",
        problem.line ? fmt::format(" (line {})", *problem.line) : "",
        problem.message),
      std::nullopt,
      problem.line);
  }
  if (!pasted)
  {
    throw opFailed(
      context.opName,
      fmt::format(
        "the fragment holds no groups, entities or brushes: {}",
        std::get<Error>(pasted.error()).msg));
  }

  context.outcome.created = pasted.value();
  translateByOffset(context, context.outcome.created);
}

void checkImportArgs(const Json& args)
{
  if ((args.find("file") != nullptr) == (args.find("text") != nullptr))
  {
    throw invalidArgument("import: give exactly one of \"file\" and \"text\"");
  }
}

void checkNameArg(const std::string_view opName, const Json& args)
{
  if (args["name"].asString().empty())
  {
    throw invalidArgument(fmt::format("{}: \"name\" must not be empty", opName));
  }
}

} // namespace tb::cli
