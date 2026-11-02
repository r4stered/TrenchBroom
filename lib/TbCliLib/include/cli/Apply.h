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

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tb::cli
{

class Json;
struct LoadedMap;
struct Op;
struct VerifyConfig;

/** The options that every mutating command accepts. */
struct ApplyOptions
{
  /** Refuse to run unless the map's rev is this one. */
  std::optional<std::string> expectRev;
  /** Return the result without writing anything. */
  bool dryRun = false;
  /** Write the result here instead of over the map. */
  std::optional<std::filesystem::path> output;
  /** Let ops edit locked nodes, and keep locked nodes in query targets. */
  bool allowLocked = false;
  /** Write even though the load was lossy. A dry run never writes, so it needs none. */
  bool allowLossy = false;
};

/**
 * Runs the given ops against the loaded map, all or nothing, and writes the map once,
 * atomically. Returns the `tbcli.apply/1` result
 * `{schema, rev, ops: [{ok, created, modified, removed, warnings, issues_introduced,
 * instances_affected?}], bindings, warnings?}`, where
 * `rev` is the rev of the written (or, with `dryRun`, would-be) file, `created`,
 * `modified` and the bindings are paths in that rev, and `removed` are paths in the
 * input rev. When an op edits the inside of a linked group, the edit reaches the
 * group's linked copies: what changed in them is listed under `modified` and `removed`
 * too, and `instances_affected` lists the edited group and its copies (see
 * LinkedGroupSnapshot). The top-level `warnings` is `["lossy_load"]` if the load was
 * lossy.
 * `issues_introduced` lists the issues an op introduced (see IssueTracker) whose nodes
 * are in the new rev, with the severities and suppressions of `verifyConfig`.
 *
 * Node paths and query targets are resolved against the input rev before the first op
 * runs, so that earlier ops cannot shift what later ops address; bindings name the nodes
 * that earlier ops created. Before each op runs, its targets are checked against the
 * kinds the op accepts and against locks.
 *
 * @throw CliError `stale_rev` if the map's rev is not `expectRev`; `lossy_load` if the
 * load was lossy and neither `allowLossy` nor `dryRun` is set; the error of the
 * first failing op, with its `op_index`; `write_failed` if the write cannot be
 * confirmed. Nothing is written in any of these cases.
 */
Json apply(
  LoadedMap& loadedMap,
  const std::vector<Op>& ops,
  const ApplyOptions& options,
  const VerifyConfig& verifyConfig);

} // namespace tb::cli
