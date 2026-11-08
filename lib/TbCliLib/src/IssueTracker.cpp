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

#include "cli/IssueTracker.h"

#include "cli/CliValidators.h"
#include "cli/IssueTypes.h"
#include "cli/Ops.h"
#include "cli/Targets.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityLinkManager.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityNodeBase.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/PatchNode.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"
#include "kd/ranges/to.h"

#include "vm/bbox.h"

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace tb::cli
{
namespace
{

/**
 * What tells an issue apart from the other issues on its node. Unlike its key, it stays
 * the same when the node is edited. A face issue is told apart by its face's index, which
 * does not follow a face that an edit gives another index.
 */
struct IssueIdentity
{
  const IssueTypeDef* type = nullptr;
  std::optional<size_t> faceIndex;
  std::optional<std::string> propertyKey;

  bool operator==(const IssueIdentity&) const = default;
};

IssueIdentity identityOf(const VerifyIssue& issue)
{
  return {issue.type, issue.faceIndex, issue.propertyKey};
}

/** What an IssueTracker remembers of a node. */
struct NodeRecord
{
  std::vector<IssueIdentity> issues;
  /** For a brush, its bounds. */
  std::optional<vm::bbox3d> brushBounds;
  mdl::Node* parent = nullptr;
  /** For an entity, the entities linked to it or from it. */
  std::vector<mdl::Node*> linkedEntities;
};

/** Whether the node's serialized text includes its children: a group or an entity. */
bool textIncludesChildren(const mdl::Node& node)
{
  return dynamic_cast<const mdl::GroupNode*>(&node)
         || dynamic_cast<const mdl::EntityNode*>(&node);
}

/** The entities linked to or from the given node, if it is an entity. */
std::vector<mdl::Node*> linkedEntitiesOf(
  const mdl::Node& node, const mdl::EntityLinkManager& linkManager)
{
  auto result = std::vector<mdl::Node*>{};
  if (const auto* entityNode = dynamic_cast<const mdl::EntityNodeBase*>(&node))
  {
    for (const auto* links :
         {&linkManager.linksFrom(*entityNode), &linkManager.linksTo(*entityNode)})
    {
      for (const auto& [key, linkEnds] : *links)
      {
        for (const auto& linkEnd : linkEnds)
        {
          result.push_back(linkEnd.node);
        }
      }
    }
  }
  return result;
}

/** The point entities whose origins lie in any of the given boxes. */
std::vector<mdl::Node*> pointEntitiesIn(
  mdl::WorldNode& worldNode, const std::vector<vm::bbox3d>& regions)
{
  auto result = std::vector<mdl::Node*>{};
  worldNode.accept(
    kdl::overload(
      [](auto&& thisLambda, mdl::WorldNode& world) { world.visitChildren(thisLambda); },
      [](auto&& thisLambda, mdl::LayerNode& layer) { layer.visitChildren(thisLambda); },
      [](auto&& thisLambda, mdl::GroupNode& group) { group.visitChildren(thisLambda); },
      [&](mdl::EntityNode& entity) {
        const auto& origin = entity.entity().origin();
        if (
          !entity.hasChildren() && std::ranges::any_of(regions, [&](const auto& region) {
            return region.contains(origin);
          }))
        {
          result.push_back(&entity);
        }
      },
      [](mdl::BrushNode&) {},
      [](mdl::PatchNode&) {}));
  return result;
}

/** Nodes in the order they were added, each once. */
class NodeList
{
private:
  std::vector<mdl::Node*> m_nodes;
  std::unordered_set<const mdl::Node*> m_seen;

public:
  /** Adds the node unless it was added before. */
  void add(mdl::Node& node)
  {
    if (m_seen.insert(&node).second)
    {
      m_nodes.push_back(&node);
    }
  }

  bool contains(const mdl::Node& node) const { return m_seen.contains(&node); }

  const std::vector<mdl::Node*>& nodes() const { return m_nodes; }
};

} // namespace

struct IssueTracker::Impl
{
  mdl::Map& map;
  const VerifyConfig& config;
  std::vector<std::unique_ptr<mdl::Validator>> cliValidators;
  /** The validators of the issue types that are about nodes. */
  std::vector<const mdl::Validator*> validators;
  std::unordered_map<const mdl::Node*, NodeRecord> records;

  Impl(mdl::Map& map_, const VerifyConfig& config_)
    : map{map_}
    , config{config_}
    , cliValidators{makeCliValidators(materialCheckOf(map))}
  {
    auto all = map.worldNode().registeredValidators();
    for (const auto& validator : cliValidators)
    {
      all.push_back(validator.get());
    }
    for (const auto* validator : all)
    {
      // the collector refuses a validator that is not in the table
      const auto* type = findIssueType(*validator);
      if (!type || type->scope == IssueScope::Node)
      {
        validators.push_back(validator);
      }
    }
  }

  /** The record of the node, if it has one and was not created by the current op. */
  const NodeRecord* recordOf(const mdl::Node& node, const NodeList& created) const
  {
    const auto it = records.find(&node);
    return it != records.end() && !created.contains(node) ? &it->second : nullptr;
  }

  /** Collects the issues of the given node only, and remembers them with the node. */
  std::vector<VerifyIssue> checkAndRecord(mdl::Node& node, NodeIssueCollector& collector)
  {
    auto result = collector.collect(node);

    auto& record = records[&node];
    record.issues =
      result | std::views::transform(identityOf) | kdl::ranges::to<std::vector>();
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node);
    record.brushBounds =
      brushNode ? std::optional{brushNode->logicalBounds()} : std::nullopt;
    record.parent = node.parent();
    record.linkedEntities = linkedEntitiesOf(node, map.entityLinkManager());
    return result;
  }

  /** The nearest node that held the removed node and is still in the map, if any. */
  mdl::Node* holderInMap(const mdl::Node& removedNode) const
  {
    const auto& worldNode = map.worldNode();
    auto* node = parentOf(removedNode);
    while (node && !isInMap(*node, worldNode))
    {
      node = parentOf(*node);
    }
    return node;
  }

private:
  /** The parent of a node when it was last checked. */
  mdl::Node* parentOf(const mdl::Node& node) const
  {
    const auto it = records.find(&node);
    return it != records.end() ? it->second.parent : nullptr;
  }
};

IssueTracker::IssueTracker(mdl::Map& map, const VerifyConfig& config)
  : m_impl{std::make_unique<Impl>(map, config)}
{
  auto collector = NodeIssueCollector{map, m_impl->validators};
  for (auto* node : nodeAndDescendants(m_impl->map.worldNode()))
  {
    m_impl->checkAndRecord(*node, collector);
  }
}

IssueTracker::~IssueTracker() = default;

std::vector<VerifyIssue> IssueTracker::introduced(const OpOutcome& outcome)
{
  auto& impl = *m_impl;
  auto& worldNode = impl.map.worldNode();

  // a copy that replaced a node in a linked group had the node's issues before
  for (const auto& [replaced, copy] : outcome.replaced)
  {
    if (const auto it = impl.records.find(replaced); it != impl.records.end())
    {
      auto record = it->second;
      record.parent = copy->parent();
      impl.records.insert_or_assign(copy, std::move(record));
    }
  }

  // the nodes whose issues are checked and reported, and those of them that are new
  auto changed = NodeList{};
  auto created = NodeList{};
  for (auto* node : outcome.created)
  {
    if (isInMap(*node, worldNode))
    {
      for (auto* each : nodeAndDescendants(*node))
      {
        changed.add(*each);
        created.add(*each);
      }
    }
  }
  for (auto* node : outcome.modified)
  {
    if (isInMap(*node, worldNode))
    {
      for (auto* each : nodeAndDescendants(*node))
      {
        changed.add(*each);
      }
    }
  }

  // the removed nodes with everything that was below them
  auto removed = std::vector<mdl::Node*>{};
  for (auto* node : outcome.removed)
  {
    std::ranges::copy(nodeAndDescendants(*node), std::back_inserter(removed));
  }

  // where a changed or removed brush is or was, a point entity may have come to be
  // inside a solid; the link ends of a changed or removed entity may be missing now
  auto regions = std::vector<vm::bbox3d>{};
  auto linked = std::vector<mdl::Node*>{};
  for (const auto* node : changed.nodes())
  {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node))
    {
      regions.push_back(brushNode->logicalBounds());
    }
    std::ranges::copy(
      linkedEntitiesOf(*node, impl.map.entityLinkManager()), std::back_inserter(linked));
  }
  for (const auto* node : changed.nodes())
  {
    if (const auto* record = impl.recordOf(*node, created))
    {
      if (record->brushBounds)
      {
        regions.push_back(*record->brushBounds);
      }
      std::ranges::copy(record->linkedEntities, std::back_inserter(linked));
    }
  }
  for (const auto* node : removed)
  {
    if (const auto* record = impl.recordOf(*node, created))
    {
      if (record->brushBounds)
      {
        regions.push_back(*record->brushBounds);
      }
      std::ranges::copy(record->linkedEntities, std::back_inserter(linked));
    }
  }
  if (!regions.empty())
  {
    for (auto* entity : pointEntitiesIn(worldNode, regions))
    {
      changed.add(*entity);
    }
  }
  for (auto* entity : linked)
  {
    if (isInMap(*entity, worldNode))
    {
      changed.add(*entity);
    }
  }

  // the groups and entities that hold changed or removed nodes are not modified, but
  // may have gained or lost issues, such as being empty; they are remembered again
  // without reporting them
  auto holders = NodeList{};
  const auto addHolders = [&](mdl::Node* node) {
    for (; node; node = node->parent())
    {
      if (textIncludesChildren(*node) && !changed.contains(*node))
      {
        holders.add(*node);
      }
    }
  };
  for (auto* node : changed.nodes())
  {
    addHolders(node->parent());
  }
  for (const auto* node : outcome.removed)
  {
    addHolders(impl.holderInMap(*node));
  }

  auto collector = NodeIssueCollector{impl.map, impl.validators};
  auto result = std::vector<VerifyIssue>{};
  for (auto* node : changed.nodes())
  {
    const auto* record = impl.recordOf(*node, created);
    auto before = record ? record->issues : std::vector<IssueIdentity>{};

    for (auto& issue : impl.checkAndRecord(*node, collector))
    {
      if (
        const auto it = std::ranges::find(before, identityOf(issue)); it != before.end())
      {
        before.erase(it);
      }
      else
      {
        // the lines of the new rev are not known until it is written
        issue.line = std::nullopt;
        result.push_back(std::move(issue));
      }
    }
  }
  for (auto* node : holders.nodes())
  {
    impl.checkAndRecord(*node, collector);
  }

  applyConfig(impl.map, impl.config, result);
  std::erase_if(result, [](const auto& issue) { return issue.suppressed; });
  return result;
}

} // namespace tb::cli
