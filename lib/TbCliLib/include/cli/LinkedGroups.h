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

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace tb::mdl
{
class GroupNode;
class Node;
class WorldNode;
} // namespace tb::mdl

namespace tb::cli
{

struct OpOutcome;

/**
 * The linked groups of a map and the nodes inside them, recorded before an op runs.
 *
 * When an op edits the inside of a linked group, the map library updates the group's
 * linked copies by replacing everything inside them with copies of the edited group's
 * contents. A snapshot finds what that update did, so that the op can report it.
 */
class LinkedGroupSnapshot
{
public:
  /** A node inside a linked group, as it was when the snapshot was taken. */
  struct Entry
  {
    mdl::Node* node = nullptr;
    std::string linkId;
    /** The index of the entry of the node's parent, unless it is the group itself. */
    std::optional<size_t> parent;
  };

  /** A linked group with the nodes inside it, depth first, in file order. */
  struct Instance
  {
    mdl::GroupNode* group = nullptr;
    std::vector<Entry> entries;
  };

private:
  /** Every group that is linked to another group. */
  std::vector<Instance> m_instances;

public:
  /** Records the linked groups of the map in its current state. */
  explicit LinkedGroupSnapshot(const mdl::WorldNode& worldNode);

  /**
   * Adds to the outcome of the op that ran since the snapshot was taken what the linked
   * update did to the linked copies of the groups the op edited:
   *
   * - `instancesAffected`: every linked group whose link set had its contents changed,
   *   the edited group as well as its copies, including copies inside copies;
   * - `modified`: the nodes in the copies that correspond to the op's modified nodes, and
   *   those that correspond to its created nodes;
   * - `removed`: the nodes that the copies lost, as the edited group lost theirs;
   * - `replaced`: every node in the copies with the copy that replaced it.
   *
   * In the edited group, a node that is still there corresponds to itself. A linked copy
   * that the update refilled is laid out as the edited group, so its nodes correspond
   * where the edited group's do. Failing that, a copy corresponds to the first node with
   * its link id under the node that its parent corresponds to. Nodes inside a created or
   * removed node are not listed, as the op does not list them either.
   */
  void recordLinkedUpdates(const mdl::WorldNode& worldNode, OpOutcome& outcome) const;
};

} // namespace tb::cli
