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

#include "cli/Targets.h"

#include "cli/CliError.h"
#include "cli/Json.h"
#include "cli/NodeHandles.h"
#include "cli/Ops.h"
#include "cli/ParseUtils.h"
#include "cli/Query.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/WorldNode.h"

#include "kd/contracts.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <set>
#include <utility>

namespace tb::cli
{
namespace
{

constexpr auto KindNames = std::array<std::pair<TargetKind, std::string_view>, 8>{{
  {TargetKind::World, "world"},
  {TargetKind::Layer, "layer"},
  {TargetKind::Group, "group"},
  {TargetKind::PointEntity, "point_entity"},
  {TargetKind::BrushEntity, "brush_entity"},
  {TargetKind::Brush, "brush"},
  {TargetKind::Patch, "patch"},
  {TargetKind::Face, "face"},
}};

/** Whether the given JSON is an `expect` count: a non-negative integer or "any". */
bool isExpect(const Json& value)
{
  if (value.isString())
  {
    return value.asString() == "any";
  }
  return value.isNumber() && value.asNumber() >= 0.0
         && std::trunc(value.asNumber()) == value.asNumber();
}

bool isQueryTarget(const Json& value)
{
  if (!value.isObject())
  {
    return false;
  }

  const auto* query = value.find("query");
  const auto* expect = value.find("expect");
  return query && query->isString() && (!expect || isExpect(*expect))
         && value.size() == (expect ? 2u : 1u);
}

/** A target element as written: a node path or a binding name. */
struct PathOrBinding
{
  std::string text;
  bool isBinding = false;
};

/** The element, unless it is not a string, empty, or a malformed binding name. */
std::optional<PathOrBinding> parsePathOrBinding(const Json& value)
{
  if (!value.isString() || value.asString().empty())
  {
    return std::nullopt;
  }

  const auto& text = value.asString();
  const auto isBinding = text.starts_with('$');
  if (isBinding && !isBindingName(text))
  {
    return std::nullopt;
  }
  return PathOrBinding{text, isBinding};
}

bool isExpandable(const TargetKind kind)
{
  return kind == TargetKind::Layer || kind == TargetKind::Group
         || kind == TargetKind::BrushEntity;
}

void collectBrushes(mdl::Node& node, std::vector<NodeOrFace>& brushes)
{
  for (auto* child : node.children())
  {
    if (kindOf(*child) == NodeKind::Brush)
    {
      brushes.push_back({child, std::nullopt});
    }
    collectBrushes(*child, brushes);
  }
}

} // namespace

std::string_view toString(const TargetKind kind)
{
  return std::ranges::find(
           KindNames, kind, &std::pair<TargetKind, std::string_view>::first)
    ->second;
}

std::string toString(const std::vector<TargetKind>& kinds)
{
  auto result = std::string{};
  for (const auto kind : kinds)
  {
    result += (result.empty() ? "" : ", ") + std::string{toString(kind)};
  }
  return result;
}

TargetKind targetKindOf(const NodeOrFace& nodeOrFace)
{
  if (nodeOrFace.faceIndex)
  {
    return TargetKind::Face;
  }

  switch (kindOf(*nodeOrFace.node))
  {
  case NodeKind::World:
    return TargetKind::World;
  case NodeKind::Layer:
    return TargetKind::Layer;
  case NodeKind::Group:
    return TargetKind::Group;
  case NodeKind::Entity:
    return nodeOrFace.node->hasChildren() ? TargetKind::BrushEntity
                                          : TargetKind::PointEntity;
  case NodeKind::Brush:
    return TargetKind::Brush;
  case NodeKind::Patch:
    return TargetKind::Patch;
  case NodeKind::Face:
    break;
  }
  contract_assert(false);
  return TargetKind::World;
}

bool isBindingName(const std::string_view name)
{
  const auto isDigit = [](const char c) { return c >= '0' && c <= '9'; };
  return name.size() >= 2 && name.front() == '$' && isWordChar(name[1])
         && !isDigit(name[1]) && std::ranges::all_of(name.substr(2), isWordChar);
}

bool isPathOrBinding(const Json& value)
{
  return parsePathOrBinding(value).has_value();
}

Json queryTargetFromFlags(
  const std::string_view opName,
  const std::optional<std::string>& query,
  const std::optional<std::string>& expect)
{
  if (!query)
  {
    throw CliError{
      ErrorCode::InvalidArgument, fmt::format("{}: --expect requires --query", opName)};
  }

  auto result = Json::object();
  result.set("query", *query);
  if (expect)
  {
    if (*expect == "any")
    {
      result.set("expect", *expect);
    }
    else if (const auto count = parseUnsigned(*expect))
    {
      result.set("expect", *count);
    }
    else
    {
      throw CliError{
        ErrorCode::InvalidArgument,
        fmt::format(
          "{}: --expect must be a non-negative integer or any, not '{}'",
          opName,
          *expect)};
    }
  }
  return result;
}

bool isInMap(const mdl::Node& node, const mdl::WorldNode& worldNode)
{
  return &node == &worldNode || node.isDescendantOf(worldNode);
}

std::vector<mdl::Node*> nodeAndDescendants(mdl::Node& node)
{
  auto result = std::vector<mdl::Node*>{&node};
  for (auto* child : node.children())
  {
    std::ranges::move(nodeAndDescendants(*child), std::back_inserter(result));
  }
  return result;
}

void appendOnce(std::vector<mdl::Node*>& nodes, mdl::Node* node)
{
  if (std::ranges::find(nodes, node) == nodes.end())
  {
    nodes.push_back(node);
  }
}

std::vector<mdl::Node*> ancestorsOf(const std::vector<mdl::Node*>& nodes)
{
  auto result = std::vector<mdl::Node*>{};
  for (auto* node : nodes)
  {
    for (auto* parent = node->parent(); parent; parent = parent->parent())
    {
      appendOnce(result, parent);
    }
  }
  return result;
}

void appendRemovedNodes(
  const std::vector<mdl::Node*>& nodes,
  const mdl::WorldNode& worldNode,
  std::vector<mdl::Node*>& removed)
{
  for (auto* node : nodes)
  {
    if (!isInMap(*node, worldNode))
    {
      appendOnce(removed, node);
    }
  }
}

bool isTarget(const Json& value)
{
  if (value.isArray())
  {
    return value.size() > 0 && std::ranges::all_of(value.asArray(), isPathOrBinding);
  }
  return isPathOrBinding(value) || isQueryTarget(value);
}

void recordMoveOutcome(
  const std::vector<mdl::Node*>& moved,
  const std::vector<mdl::Node*>& formerAncestors,
  const mdl::WorldNode& worldNode,
  OpOutcome& outcome)
{
  for (auto* node : moved)
  {
    appendOnce(outcome.modified, node);
  }
  for (auto* ancestor : formerAncestors)
  {
    if (isInMap(*ancestor, worldNode) && kindOf(*ancestor) == NodeKind::Entity)
    {
      appendOnce(outcome.modified, ancestor);
    }
  }
  appendRemovedNodes(formerAncestors, worldNode, outcome.removed);
}

std::vector<std::string> bindingReferences(const Json& target)
{
  auto result = std::vector<std::string>{};
  const auto add = [&](const Json& element) {
    if (const auto parsed = parsePathOrBinding(element); parsed && parsed->isBinding)
    {
      result.push_back(parsed->text);
    }
  };

  if (target.isArray())
  {
    std::ranges::for_each(target.asArray(), add);
  }
  else
  {
    add(target);
  }
  return result;
}

std::vector<std::string> nodeReferences(const Json& target)
{
  auto result = std::vector<std::string>{};
  const auto add = [&](const Json& element) {
    if (const auto parsed = parsePathOrBinding(element); parsed && !parsed->isBinding)
    {
      result.push_back(parsed->text);
    }
  };

  if (target.isArray())
  {
    std::ranges::for_each(target.asArray(), add);
  }
  else
  {
    add(target);
  }
  return result;
}

TargetResolver::TargetResolver(
  mdl::Map& map, const bool allowLocked, NodeHandleTable* handles)
  : m_map{map}
  , m_allowLocked{allowLocked}
  , m_handles{handles}
  , m_inputPaths{nodePaths(map.worldNode())}
{
}

std::optional<std::string> TargetResolver::inputPath(const mdl::Node& node) const
{
  const auto it = m_inputPaths.find(&node);
  return it != m_inputPaths.end() ? std::optional{it->second} : std::nullopt;
}

ResolvedTarget TargetResolver::resolve(const Json& target) const
{
  auto result = ResolvedTarget{};
  const auto addPathOrBinding = [&](const Json& element) {
    auto parsed = parsePathOrBinding(element);
    contract_assert(parsed.has_value());
    auto node =
      parsed->isBinding
        ? std::nullopt
        : std::optional{resolveNodeReference(m_map.worldNode(), parsed->text, m_handles)};
    result.push_back(TargetElement{std::move(parsed->text), node});
  };

  if (target.isArray())
  {
    std::ranges::for_each(target.asArray(), addPathOrBinding);
    return result;
  }
  if (target.isString())
  {
    addPathOrBinding(target);
    return result;
  }

  const auto& query = target["query"].asString();
  for (const auto& match : queryMatches(m_map, query))
  {
    if (m_allowLocked || !match.node->locked())
    {
      result.push_back({std::nullopt, match});
    }
  }

  if (const auto* expect = target.find("expect"); expect && expect->isNumber())
  {
    if (const auto expected = size_t(expect->asNumber()); result.size() != expected)
    {
      throw CliError{
        ErrorCode::ExpectMismatch,
        fmt::format(
          "the query '{}' matched {} {}, not the expected {}{}",
          query,
          result.size(),
          result.size() == 1 ? "node" : "nodes",
          expected,
          m_allowLocked ? "" : " (locked nodes are left out unless --allow-locked)")};
    }
  }
  return result;
}

void TargetResolver::bind(const std::string& name, std::vector<mdl::Node*> nodes)
{
  m_bindings[name] = std::move(nodes);
}

void TargetResolver::replace(const std::vector<NodeReplacement>& replacements)
{
  for (const auto& [replaced, copy] : replacements)
  {
    m_copies[replaced] = copy;
    if (const auto path = inputPath(*replaced))
    {
      m_inputPaths.emplace(copy, *path);
    }
  }
}

mdl::Node* TargetResolver::current(mdl::Node* node) const
{
  for (auto it = m_copies.find(node); it != m_copies.end(); it = m_copies.find(node))
  {
    node = it->second;
  }
  return node;
}

std::vector<NodeOrFace> TargetResolver::nodes(
  const ResolvedTarget& target,
  const TargetRules& rules,
  const std::string_view label) const
{
  const auto& worldNode = m_map.worldNode();

  auto result = std::vector<NodeOrFace>{};
  auto seen = std::set<std::pair<const mdl::Node*, std::optional<size_t>>>{};
  for (const auto& element : target)
  {
    const auto name = [&](const NodeOrFace& nodeOrFace) {
      if (element.source)
      {
        return *element.source;
      }
      const auto path = inputPath(*nodeOrFace.node);
      contract_assert(path.has_value());
      return nodeOrFace.faceIndex
               ? fmt::format("{}/face:{}", *path, *nodeOrFace.faceIndex)
               : *path;
    };

    auto elementNodes = std::vector<NodeOrFace>{};
    if (element.node)
    {
      elementNodes.push_back({current(element.node->node), element.node->faceIndex});
    }
    else
    {
      const auto it = m_bindings.find(*element.source);
      contract_assert(it != m_bindings.end());
      for (auto* node : it->second)
      {
        elementNodes.push_back({current(node), std::nullopt});
      }
    }

    for (const auto& nodeOrFace : elementNodes)
    {
      const auto* node = nodeOrFace.node;
      if (!isInMap(*node, worldNode))
      {
        throw CliError{
          ErrorCode::TargetDeleted,
          fmt::format(
            "{} {} was removed by an earlier op of the batch", label, name(nodeOrFace)),
          name(nodeOrFace)};
      }

      auto expanded = std::vector<NodeOrFace>{};
      if (rules.expandsToBrushes && isExpandable(targetKindOf(nodeOrFace)))
      {
        collectBrushes(*nodeOrFace.node, expanded);
        if (expanded.empty())
        {
          throw CliError{
            ErrorCode::WrongNodeKind,
            fmt::format("{} {} contains no brushes to act on", label, name(nodeOrFace)),
            name(nodeOrFace)};
        }
      }
      else
      {
        expanded.push_back(nodeOrFace);
      }

      for (const auto& each : expanded)
      {
        const auto kind = targetKindOf(each);
        if (std::ranges::find(rules.kinds, kind) == rules.kinds.end())
        {
          throw CliError{
            ErrorCode::WrongNodeKind,
            fmt::format(
              "{} {} is a {}, but must be one of: {}",
              label,
              name(nodeOrFace),
              toString(kind),
              toString(rules.kinds)),
            name(nodeOrFace)};
        }
        if (rules.refusesLocked && !m_allowLocked && each.node->locked())
        {
          throw CliError{
            ErrorCode::Locked,
            fmt::format(
              "{} {} {}; pass --allow-locked to edit it anyway",
              label,
              name(nodeOrFace),
              each.node == nodeOrFace.node ? "is locked" : "contains locked nodes"),
            name(nodeOrFace)};
        }
        if (seen.emplace(each.node, each.faceIndex).second)
        {
          result.push_back(each);
        }
      }
    }
  }
  return result;
}

} // namespace tb::cli
