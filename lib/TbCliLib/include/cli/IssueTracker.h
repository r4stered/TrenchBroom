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

#include "cli/Verify.h"

#include <memory>
#include <vector>

namespace tb::cli
{

struct LoadedMap;
struct OpOutcome;
struct VerifyConfig;

/**
 * Finds the issues that each op of a batch introduces. It remembers the issues on every
 * node, and after each op, runs the validators and CLI checks again on the nodes the op
 * changed only.
 */
class IssueTracker
{
private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

public:
  /**
   * Collects the issues of every node of the map in its current state, the input rev.
   * This costs about as much as the in-process stages of verify.
   *
   * @throw CliError if a validator is not in the issue-type table
   */
  IssueTracker(LoadedMap& loadedMap, const VerifyConfig& config);
  ~IssueTracker();

  IssueTracker(const IssueTracker&) = delete;
  IssueTracker& operator=(const IssueTracker&) = delete;

  /**
   * After an op, the issues on the nodes it created or modified and the nodes inside
   * them that were not on those nodes before the op. Every issue on a created node is
   * new. An issue on a modified node is new unless the node had an issue of the same type
   * on the same face or property before; keys cannot tell, since editing a node changes
   * the keys of all its issues. A copy that replaced a node when a linked group was
   * updated counts as that node.
   *
   * Some issues depend on other nodes, so these are checked as well: point entities
   * whose origins lie where a changed or removed brush is or was, since whether an entity
   * is inside a solid depends on the brushes around it, and the entities linked to a
   * changed or removed entity before or after the op, since a link's other end may now
   * be missing.
   *
   * Map-wide issue types are left out; they stay in verify. The configuration's
   * severities apply, and suppressed issues are left out. The issues have no line, as
   * the lines of the new rev are not known until it is written. Their keys are what
   * verify gives them in the new rev, unless a later op of the batch edits their node or
   * an identical node with the same issue comes before theirs in the map (see
   * VerifyIssue::key).
   */
  std::vector<VerifyIssue> introduced(const OpOutcome& outcome);
};

} // namespace tb::cli
