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

#pragma once

#include "cli/NodePath.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tb::mdl
{
class Map;
class Node;
class WorldNode;
} // namespace tb::mdl

namespace tb::cli
{

class Json;
struct OpOutcome;

/**
 * A node that the map library replaced with an identical copy, as it does with the
 * contents of linked groups when it updates them.
 */
struct NodeReplacement
{
  mdl::Node* replaced = nullptr;
  mdl::Node* copy = nullptr;
};

/** The kinds of node an op argument can accept, as `ops --json` names them. */
enum class TargetKind
{
  World,
  Layer,
  Group,
  /** an entity without brushes */
  PointEntity,
  /** an entity with brushes */
  BrushEntity,
  Brush,
  Patch,
  Face,
};

/** The name of a kind in `ops --json`, such as `brush_entity`. */
std::string_view toString(TargetKind kind);

/** The names of the given kinds, separated by commas, as in `layer, group`. */
std::string toString(const std::vector<TargetKind>& kinds);

/** The kind of a node or face. */
TargetKind targetKindOf(const NodeOrFace& nodeOrFace);

/** What an op argument that addresses nodes accepts. */
struct TargetRules
{
  /** The kinds the op accepts. */
  std::vector<TargetKind> kinds;
  /**
   * Whether a layer, group or brush entity stands for the brushes inside it, as it does
   * for brush and face ops.
   */
  bool expandsToBrushes = false;
  /**
   * Whether locked nodes are refused unless locked nodes may be edited. Only `lock`
   * accepts locked nodes, so that locking a locked layer changes nothing.
   */
  bool refusesLocked = true;
};

/**
 * Whether the given JSON is a well-formed target: a node path, a `$binding`, a
 * non-empty list of them, or `{"query": <ql>, "expect"?: <n> | "any"}`.
 */
bool isTarget(const Json& value);

/** Whether the given string names a binding, as in `$room`. */
bool isBindingName(std::string_view name);

/**
 * Whether the given JSON is a non-empty node path or a binding name; paths are checked
 * when they are resolved.
 */
bool isPathOrBinding(const Json& value);

/**
 * The query target `{"query": <ql>, "expect"?: <n> | "any"}` that a subcommand's
 * `--query` and `--expect` flags give.
 *
 * @throw CliError `invalid_argument` if `expect` is given without `query`, or is neither
 * a non-negative integer nor `any`
 */
Json queryTargetFromFlags(
  std::string_view opName,
  const std::optional<std::string>& query,
  const std::optional<std::string>& expect);

/** Whether the given node is the world or below it, i.e. not removed from the map. */
bool isInMap(const mdl::Node& node, const mdl::WorldNode& worldNode);

/** The given node and everything below it, depth first, in file order. */
std::vector<mdl::Node*> nodeAndDescendants(mdl::Node& node);

/** Appends the node unless the nodes contain it already. */
void appendOnce(std::vector<mdl::Node*>& nodes, mdl::Node* node);

/** The given nodes' ancestors, each once. */
std::vector<mdl::Node*> ancestorsOf(const std::vector<mdl::Node*>& nodes);

/**
 * Appends those of the given nodes that are no longer in the map to `removed`, each
 * once: the nodes an op replaced, or their former ancestors that it left empty.
 */
void appendRemovedNodes(
  const std::vector<mdl::Node*>& nodes,
  const mdl::WorldNode& worldNode,
  std::vector<mdl::Node*>& removed);

/**
 * Records that brushes moved out of the given former ancestors: the brushes and the
 * entities that lost some of them as modified, and the ancestors that the move left
 * empty as removed.
 */
void recordMoveOutcome(
  const std::vector<mdl::Node*>& moved,
  const std::vector<mdl::Node*>& formerAncestors,
  const mdl::WorldNode& worldNode,
  OpOutcome& outcome);

/** The binding names that the given target refers to. */
std::vector<std::string> bindingReferences(const Json& target);

/**
 * One element of a target. It is resolved against the input rev, unless it is a binding,
 * which names nodes that an earlier op of the batch creates.
 */
struct TargetElement
{
  /** The path or binding as it was given; unset for a query match. */
  std::optional<std::string> source;
  /** The node or face, unless this is a binding. */
  std::optional<NodeOrFace> node;
};

/** A target with its paths and queries resolved against the input rev. */
using ResolvedTarget = std::vector<TargetElement>;

/**
 * Resolves the targets of a batch: paths and queries up front against the input rev,
 * bindings when an op runs.
 */
class TargetResolver
{
private:
  mdl::Map& m_map;
  bool m_allowLocked;
  std::unordered_map<const mdl::Node*, std::string> m_inputPaths;
  std::map<std::string, std::vector<mdl::Node*>, std::less<>> m_bindings;
  std::unordered_map<const mdl::Node*, mdl::Node*> m_copies;

public:
  /**
   * Creates a resolver for the map in its current state, the input rev. Unless
   * `allowLocked` is set, locked nodes are dropped from query results and ops on locked
   * nodes fail.
   */
  TargetResolver(mdl::Map& map, bool allowLocked);

  /** The path of a node in the input rev, if it was in the map then. */
  std::optional<std::string> inputPath(const mdl::Node& node) const;

  /**
   * Resolves the paths and queries of a well-formed target (see isTarget) against the
   * input rev. Locked nodes are dropped from query results before the match count is
   * compared with `expect`.
   *
   * @throw CliError if a path does not resolve, or `expect_mismatch` if a query matches
   * a number of nodes other than its `expect`
   */
  ResolvedTarget resolve(const Json& target) const;

  /** Binds the given name to the nodes an op created, for the ops that follow. */
  void bind(const std::string& name, std::vector<mdl::Node*> nodes);

  /**
   * Records that the map library replaced nodes with identical copies, as it does with
   * the contents of linked groups when it updates them, so that targets naming the
   * replaced nodes reach their copies, and the copies have the input paths of the nodes
   * they replaced.
   *
   */
  void replace(const std::vector<NodeReplacement>& replacements);

  /** The node that stands for the given one now: its latest copy, if it was replaced. */
  mdl::Node* current(mdl::Node* node) const;

  /**
   * The nodes and faces a resolved target addresses now, each once and in order.
   * Replaced nodes are followed to their copies (see replace), bindings are replaced by
   * the nodes bound to them, and if the rules say so, layers,
   * groups and brush entities are replaced by the brushes inside them.
   *
   * @param label names the argument in error messages, as in `delete: target`
   * @throw CliError `target_deleted` if a node is no longer in the map,
   * `wrong_node_kind` if the rules do not accept a node or a node expands to no
   * brushes, or `locked` if a node is locked, the rules refuse locked nodes and locked
   * nodes are not allowed
   */
  std::vector<NodeOrFace> nodes(
    const ResolvedTarget& target, const TargetRules& rules, std::string_view label) const;
};

} // namespace tb::cli
