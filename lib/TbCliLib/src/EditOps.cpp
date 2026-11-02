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
#include "cli/IssueTypes.h"
#include "cli/Ops.h"
#include "cli/Targets.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Issue.h"
#include "mdl/IssueQuickFix.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"

#include "kd/ranges/to.h"
#include "kd/string_utils.h"
#include "kd/vector_utils.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <ranges>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tb::cli
{
namespace
{

vm::vec3d toVec3(const Json& json)
{
  return {json[0].asNumber(), json[1].asNumber(), json[2].asNumber()};
}

/** Appends the node unless the nodes contain it already. */
void appendOnce(std::vector<mdl::Node*>& nodes, mdl::Node* node)
{
  if (std::ranges::find(nodes, node) == nodes.end())
  {
    nodes.push_back(node);
  }
}

/** The given nodes' ancestors, each once. */
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

/** The validator that the map registers for the given issue type. */
const mdl::Validator& registeredValidatorOf(
  const mdl::WorldNode& worldNode, const IssueTypeDef& type)
{
  for (const auto* validator : worldNode.registeredValidators())
  {
    if (findIssueType(*validator) == &type)
    {
      return *validator;
    }
  }
  throw CliError{
    ErrorCode::InternalError,
    fmt::format("fix: the map has no validator for issue type \"{}\"", type.id)};
}

/** The validator's quick-fix that the given table entry describes. */
const mdl::IssueQuickFix& quickFixOf(
  const mdl::Validator& validator, const QuickFixDef& fix)
{
  for (const auto* quickFix : validator.quickFixes())
  {
    if (quickFix->description() == fix.description)
    {
      return *quickFix;
    }
  }
  throw CliError{
    ErrorCode::InternalError,
    fmt::format(
      "fix: the validator '{}' has no quick-fix '{}'",
      validator.description(),
      fix.description)};
}

/**
 * The issues that the validator finds on the given nodes and faces and the nodes inside
 * them, each once. Issues on locked nodes are left out unless `allowLocked` is set.
 */
std::vector<std::unique_ptr<mdl::Issue>> issuesOf(
  const mdl::Validator& validator,
  const std::vector<NodeOrFace>& scopes,
  const bool allowLocked)
{
  auto result = std::vector<std::unique_ptr<mdl::Issue>>{};
  const auto validate = [&](mdl::Node& node, const std::optional<size_t> faceIndex) {
    if (!allowLocked && node.locked())
    {
      return;
    }
    auto issues = std::vector<std::unique_ptr<mdl::Issue>>{};
    validator.validate(node, issues);
    for (auto& issue : issues)
    {
      const auto* faceIssue = dynamic_cast<const mdl::BrushFaceIssue*>(issue.get());
      if (!faceIndex || (faceIssue && faceIssue->faceIndex() == *faceIndex))
      {
        result.push_back(std::move(issue));
      }
    }
  };

  // whole nodes first, so that a face inside one of them is not fixed twice
  auto visited = std::unordered_set<const mdl::Node*>{};
  for (const auto& [node, faceIndex] : scopes)
  {
    if (!faceIndex)
    {
      for (auto* each : nodeAndDescendants(*node))
      {
        if (visited.insert(each).second)
        {
          validate(*each, std::nullopt);
        }
      }
    }
  }
  for (const auto& [node, faceIndex] : scopes)
  {
    if (faceIndex && !visited.contains(node))
    {
      validate(*node, faceIndex);
    }
  }
  return result;
}

/** What a quick-fix may change, recorded before it runs. */
struct FixScope
{
  /** The nodes with issues, each once. */
  std::vector<mdl::Node*> issueNodes;
  /** The children of the nodes with issues, with their parents. */
  std::vector<std::pair<mdl::Node*, mdl::Node*>> childParents;
  /** The ancestors of the nodes with issues, which a fix may leave empty. */
  std::vector<mdl::Node*> ancestors;
};

FixScope fixScopeOf(const std::vector<std::unique_ptr<mdl::Issue>>& issues)
{
  auto result = FixScope{};
  for (const auto& issue : issues)
  {
    appendOnce(result.issueNodes, &issue->node());
  }
  for (auto* node : result.issueNodes)
  {
    for (auto* child : node->children())
    {
      result.childParents.emplace_back(child, node);
    }
  }
  result.ancestors = ancestorsOf(result.issueNodes);
  return result;
}

/**
 * The nodes the issue browser selects for the given issues, on which quick-fixes act:
 * none if any issue is on a node that cannot be selected, such as the world.
 */
std::vector<mdl::Node*> selectionFor(
  const std::vector<std::unique_ptr<mdl::Issue>>& issues)
{
  auto result = std::vector<mdl::Node*>{};
  for (const auto& issue : issues)
  {
    if (!issue->addSelectableNodes(result))
    {
      return {};
    }
  }
  kdl::vec_sort_and_remove_duplicates(result);
  return result;
}

/**
 * Records what a quick-fix changed: the nodes with issues that are still in the map and
 * the children it moved as modified, and the nodes with issues and their ancestors that
 * are gone as removed.
 */
void recordFixOutcome(
  const FixScope& scope, const mdl::WorldNode& worldNode, OpOutcome& outcome)
{
  for (auto* node : scope.issueNodes)
  {
    appendOnce(isInMap(*node, worldNode) ? outcome.modified : outcome.removed, node);
  }
  for (const auto& [child, parent] : scope.childParents)
  {
    if (isInMap(*child, worldNode) && child->parent() != parent)
    {
      appendOnce(outcome.modified, child);
    }
  }
  for (auto* ancestor : scope.ancestors)
  {
    if (!isInMap(*ancestor, worldNode))
    {
      appendOnce(outcome.removed, ancestor);
    }
  }
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

  const auto ancestors = ancestorsOf(toRemove);

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

void checkFixArgs(const Json& args)
{
  const auto& typeId = args["issue_type"].asString();
  const auto* type = findIssueType(typeId);
  if (!type)
  {
    throw CliError{
      ErrorCode::InvalidArgument,
      fmt::format(
        "fix: unknown issue type \"{}\"; tbcli ops --json lists the issue types",
        typeId)};
  }

  const auto& fixId = args["fix"].asString();
  if (!findQuickFix(*type, fixId))
  {
    const auto fixIds = type->fixes | std::views::transform([](const auto& fix) {
                          return fmt::format("\"{}\"", fix.id);
                        })
                        | kdl::ranges::to<std::vector>();
    throw CliError{
      ErrorCode::InvalidArgument,
      fixIds.empty() ? fmt::format("fix: issue type \"{}\" has no quick-fixes", typeId)
                     : fmt::format(
                         "fix: unknown fix \"{}\" for issue type \"{}\", which has {}",
                         fixId,
                         typeId,
                         kdl::str_join(fixIds, ", "))};
  }
}

void fixIssues(OpContext& context)
{
  const auto& type = *findIssueType(context.args["issue_type"].asString());
  const auto& fixDef = *findQuickFix(type, context.args["fix"].asString());

  auto& map = context.map;
  auto& worldNode = map.worldNode();
  const auto& validator = registeredValidatorOf(worldNode, type);
  const auto& quickFix = quickFixOf(validator, fixDef);

  const auto scopes = context.args.find("target")
                        ? context.targets("target")
                        : std::vector<NodeOrFace>{{&worldNode, std::nullopt}};
  const auto issues = issuesOf(validator, scopes, context.allowLocked);
  if (issues.empty())
  {
    context.outcome.warnings.emplace_back("nothing_to_fix");
    return;
  }

  const auto scope = fixScopeOf(issues);
  mdl::deselectAll(map);
  mdl::selectNodes(map, selectionFor(issues));
  quickFix.apply(map, issues | std::views::transform([](const auto& issue) {
                        return static_cast<const mdl::Issue*>(issue.get());
                      }) | kdl::ranges::to<std::vector>());
  mdl::deselectAll(map);

  recordFixOutcome(scope, worldNode, context.outcome);
}

} // namespace tb::cli
