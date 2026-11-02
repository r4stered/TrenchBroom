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

#include "cli/NodePath.h"

#include "cli/CliError.h"
#include "cli/ParseUtils.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>
#include <vector>

namespace tb::cli
{
namespace
{

/** The name of each kind in a path segment. */
constexpr auto KindNames = std::array<std::pair<NodeKind, std::string_view>, 7>{{
  {NodeKind::World, WorldPath},
  {NodeKind::Layer, "layer"},
  {NodeKind::Group, "group"},
  {NodeKind::Entity, "entity"},
  {NodeKind::Brush, "brush"},
  {NodeKind::Patch, "patch"},
  {NodeKind::Face, "face"},
}};

std::string_view kindName(const NodeKind kind)
{
  return std::ranges::find(KindNames, kind, &std::pair<NodeKind, std::string_view>::first)
    ->second;
}

/** The kind that a path segment calls by the given name, except for the world. */
std::optional<NodeKind> kindFromName(const std::string_view name)
{
  const auto it =
    std::ranges::find(KindNames, name, &std::pair<NodeKind, std::string_view>::second);
  return it != KindNames.end() && it->first != NodeKind::World ? std::optional{it->first}
                                                               : std::nullopt;
}


std::string segment(const mdl::Node& node, const mdl::Node& parent)
{
  const auto kind = kindOf(node);
  const auto& siblings = parent.children();
  const auto end = std::ranges::find(siblings, &node);
  const auto index = std::count_if(
    siblings.begin(), end, [&](const auto* sibling) { return kindOf(*sibling) == kind; });
  return fmt::format("{}:{}", kindName(kind), index);
}

/** One `kind:index` or `kind:id:<id>` segment of a node path. */
struct Segment
{
  NodeKind kind = NodeKind::Layer;
  size_t number = 0;
  /** Whether `number` is a persistent id rather than an index. */
  bool byId = false;
};

CliError invalidPath(const std::string_view path, const std::string_view reason)
{
  return CliError{
    ErrorCode::InvalidPath,
    fmt::format("invalid node path '{}': {}", path, reason),
    std::string{path}};
}

CliError nodeNotFound(const std::string_view path, const std::string_view reason)
{
  return CliError{
    ErrorCode::NodeNotFound,
    fmt::format("no node at '{}': {}", path, reason),
    std::string{path}};
}

Segment parseSegment(const std::string_view text, const std::string_view path)
{
  const auto colon = text.find(':');
  if (colon == std::string_view::npos)
  {
    throw invalidPath(
      path, fmt::format("segment '{}' is not of the form kind:index", text));
  }

  const auto name = text.substr(0, colon);
  const auto kind = kindFromName(name);
  if (!kind)
  {
    throw invalidPath(
      path,
      fmt::format(
        "unknown node kind '{}', expected layer, group, entity, brush, patch or face",
        name));
  }

  auto result = Segment{*kind};
  auto number = text.substr(colon + 1);
  if (number.starts_with("id:"))
  {
    if (result.kind != NodeKind::Layer && result.kind != NodeKind::Group)
    {
      throw invalidPath(
        path, fmt::format("only layers and groups are addressed by id, not '{}'", text));
    }
    result.byId = true;
    number = number.substr(3);
  }

  const auto value = parseUnsigned(number);
  if (!value)
  {
    throw invalidPath(
      path,
      fmt::format(
        "segment '{}' must end in a non-negative integer {}",
        text,
        result.byId ? "id" : "index"));
  }
  result.number = *value;
  return result;
}

std::optional<mdl::IdType> persistentId(const mdl::Node& node)
{
  if (const auto* layerNode = dynamic_cast<const mdl::LayerNode*>(&node))
  {
    return layerNode->persistentId();
  }
  if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(&node))
  {
    return groupNode->persistentId();
  }
  return std::nullopt;
}

/** The group with the given persistent id anywhere below the given node. */
mdl::Node* findGroupById(const mdl::Node& node, const mdl::IdType id)
{
  for (auto* child : node.children())
  {
    if (
      const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(child);
      groupNode && groupNode->persistentId() == id)
    {
      return child;
    }
    if (auto* group = findGroupById(*child, id))
    {
      return group;
    }
  }
  return nullptr;
}

/** The child of `parent` that the segment names, if any. */
mdl::Node* findChild(const mdl::Node& parent, const Segment& segment)
{
  auto index = size_t{0};
  for (auto* child : parent.children())
  {
    if (kindOf(*child) != segment.kind)
    {
      continue;
    }
    if (segment.byId ? persistentId(*child) == segment.number : index == segment.number)
    {
      return child;
    }
    ++index;
  }
  return nullptr;
}


/** Adds the paths of the nodes below `parent`, whose path is `prefix` without '/'. */
void addChildPaths(
  const mdl::Node& parent,
  const std::string& prefix,
  std::unordered_map<const mdl::Node*, std::string>& paths)
{
  auto counts = std::array<size_t, KindNames.size()>{};
  for (const auto* child : parent.children())
  {
    const auto kind = kindOf(*child);
    auto path = fmt::format("{}{}:{}", prefix, kindName(kind), counts[size_t(kind)]++);
    addChildPaths(*child, path + "/", paths);
    paths.emplace(child, std::move(path));
  }
}

} // namespace

NodeKind kindOf(const mdl::Node& node)
{
  return node.accept(
    kdl::overload(
      [](const mdl::WorldNode&) { return NodeKind::World; },
      [](const mdl::LayerNode&) { return NodeKind::Layer; },
      [](const mdl::GroupNode&) { return NodeKind::Group; },
      [](const mdl::EntityNode&) { return NodeKind::Entity; },
      [](const mdl::BrushNode&) { return NodeKind::Brush; },
      [](const mdl::PatchNode&) { return NodeKind::Patch; }));
}

bool canContain(const NodeKind parent, const NodeKind kind)
{
  switch (parent)
  {
  case NodeKind::World:
    return kind == NodeKind::Layer;
  case NodeKind::Layer:
  case NodeKind::Group:
    return kind == NodeKind::Group || kind == NodeKind::Entity || kind == NodeKind::Brush
           || kind == NodeKind::Patch;
  case NodeKind::Entity:
    return kind == NodeKind::Brush || kind == NodeKind::Patch;
  case NodeKind::Brush:
    return kind == NodeKind::Face;
  case NodeKind::Patch:
  case NodeKind::Face:
    return false;
  }
  return false;
}

std::string nodePath(const mdl::Node& node)
{
  const auto* parent = node.parent();
  if (!parent)
  {
    return std::string{WorldPath};
  }

  const auto own = segment(node, *parent);
  return parent->parent() ? nodePath(*parent) + "/" + own : own;
}

std::unordered_map<const mdl::Node*, std::string> nodePaths(
  const mdl::WorldNode& worldNode)
{
  auto result = std::unordered_map<const mdl::Node*, std::string>{};
  result.emplace(&worldNode, WorldPath);
  addChildPaths(worldNode, "", result);
  return result;
}

std::string facePath(const mdl::BrushNode& brushNode, const size_t faceIndex)
{
  return fmt::format(
    "{}/{}:{}", nodePath(brushNode), kindName(NodeKind::Face), faceIndex);
}

NodeOrFace resolveNodePath(mdl::WorldNode& worldNode, const std::string_view path)
{
  if (path == WorldPath)
  {
    return {&worldNode, std::nullopt};
  }
  if (path.empty())
  {
    throw invalidPath(path, "the path is empty");
  }

  auto segments = std::vector<std::string_view>{};
  for (const auto text : path | std::views::split('/'))
  {
    segments.emplace_back(text.begin(), text.end());
  }

  mdl::Node* current = &worldNode;
  for (size_t i = 0; i < segments.size(); ++i)
  {
    const auto segment = parseSegment(segments[i], path);
    // the path up to and including this segment
    const auto prefix =
      path.substr(0, size_t(segments[i].data() - path.data()) + segments[i].size());

    if (i == 0 && segment.kind == NodeKind::Group && segment.byId)
    {
      current = findGroupById(worldNode, segment.number);
      if (!current)
      {
        throw nodeNotFound(
          path, fmt::format("there is no group with id {}", segment.number));
      }
      continue;
    }

    const auto currentKind = kindOf(*current);
    if (!canContain(currentKind, segment.kind))
    {
      throw invalidPath(
        path,
        currentKind == NodeKind::World
          ? fmt::format(
              "a path starts with a layer or group:id:<id>, not '{}'", segments[i])
          : fmt::format(
              "a {} cannot contain a {}", kindName(currentKind), kindName(segment.kind)));
    }

    if (segment.kind == NodeKind::Face)
    {
      if (i + 1 < segments.size())
      {
        throw invalidPath(path, "a face cannot contain anything");
      }
      const auto& brush = static_cast<const mdl::BrushNode&>(*current).brush();
      if (segment.number >= brush.faceCount())
      {
        throw nodeNotFound(
          path, fmt::format("{} has {} faces", nodePath(*current), brush.faceCount()));
      }
      return {current, segment.number};
    }

    current = findChild(*current, segment);
    if (!current)
    {
      throw nodeNotFound(path, fmt::format("{} does not exist", prefix));
    }
  }

  return {current, std::nullopt};
}

} // namespace tb::cli
