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

#include "cli/Json.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mdl
{
class Map;
} // namespace tb::mdl

namespace tb::cli
{

class LiveSession;
class NodeHandleTable;
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

/** What a batch of ops did to a map, with paths in the map's new state. */
struct BatchResult
{
  /**
   * Per op `{ok, created, modified, removed, warnings, issues_introduced,
   * instances_affected?, stale_placements?}`, as in the `tbcli.apply/1` result.
   */
  Json ops;
  /** The paths of the nodes that the ops bound, by binding name. */
  Json bindings;
  /** In a live session, the handles of the nodes that the ops bound, by binding name. */
  Json bindingHandles;
};

/**
 * Runs the given ops against the given map, which can be borrowed: resolves every
 * node path and query up front, runs the ops in order and collects what each one did,
 * as `apply` reports it (see there). Edits only the map in memory; neither writes nor
 * saves it. The map must have command collation disabled, since collated commands
 * delete the nodes that earlier ops of the batch replaced.
 *
 * `allowLocked` lets ops edit locked nodes and keeps locked nodes in query targets.
 * `issues_introduced` takes the severities and suppressions of `verifyConfig`. In a
 * live session (given its node handles), targets may name nodes by handle, and every
 * path in the result has its handle next to it (see handleKey).
 *
 * @throw CliError the error of the first failing op, with its `op_index`. The ops
 * before it have then already edited the map; undoing them is up to the caller.
 */
BatchResult runBatch(
  mdl::Map& map,
  const std::vector<Op>& ops,
  bool allowLocked,
  const VerifyConfig& verifyConfig,
  NodeHandleTable* handles);

/**
 * Runs the given ops against the loaded map with runBatch, all or nothing, and writes
 * the map once, atomically. Returns the `tbcli.apply/1` result
 * `{schema, rev, ops: [{ok, created, modified, removed, warnings, issues_introduced,
 * instances_affected?, stale_placements?}], bindings, warnings?}`, where
 * `rev` is the rev of the written (or, with `dryRun`, would-be) file, `created`,
 * `modified` and the bindings are paths in that rev, and `removed` are paths in the
 * input rev. When an op edits the inside of a linked group, the edit reaches the
 * group's linked copies: what changed in them is listed under `modified` and `removed`
 * too, and `instances_affected` lists the edited group and its copies (see
 * LinkedGroupSnapshot). `stale_placements` lists the carriers of built rooms that a
 * rebuild left in place because their placement could no longer be matched. The
 * top-level `warnings` is `["lossy_load"]` if the load was lossy.
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

/**
 * The undo step name of a batch that an agent runs in a live session:
 * `AI: <op1>, <op2> +N more`.
 */
std::string liveUndoName(const std::vector<Op>& ops);

/**
 * Runs the given ops against the open map of a live session with runBatch, all or
 * nothing, as one undo step named by liveUndoName that never merges with the steps
 * around it. The mapper's selection (nodes and faces) is kept, apart from the nodes
 * that the batch removed. Nothing is written, unless `output` is given without
 * `dryRun`: then the result is written there, and the open map is left as it was, as
 * with `dryRun`. Returns the `tbcli.apply/1` result as apply does, with
 * `"session": "live"` and the live rev after the batch, or with `output`, the rev of
 * the (would-be) output file, and with the handles of the session's nodes next to
 * their paths: `created_handles`, `modified_handles` and so on, and `bindings_handles`.
 * Targets may name nodes by handle as well as by path.
 *
 * `expectRev` may be an older rev of the session than the current one, as long as the
 * batch could not be told apart from one at the current rev: nothing that it names by
 * handle was touched since (see howTouched), and it names nothing by path, since paths
 * shift. Query targets are run against the current map and checked by their `expect`
 * only. Changes that only hide, show or select nodes don't count.
 *
 * @throw CliError `stale_rev` if the session's rev is not `expectRev` and the batch
 * conflicts with the changes since, naming the conflicts as `conflicts`
 * (`[{op_index, node, path?, reason}]`, with `reason` `path`, `removed`, `changed`,
 * `ancestor_changed` or `descendant_changed`) and the changes as `changes` (see
 * changeSummary); `stale_rev` without them if `expectRev` is not a rev of this session,
 * or older than its change log remembers; `busy` if a transaction is open; the error of
 * the first failing op, with its `op_index`; `write_failed` if the output cannot be
 * written. The open map is unchanged in any of these cases.
 */
Json applyLive(
  LiveSession& session,
  const std::vector<Op>& ops,
  const ApplyOptions& options,
  const VerifyConfig& verifyConfig);

/**
 * Undoes the agent's own most recent batch in a live session, through the editor's undo
 * history, so that the mapper can redo it. After that, the batch before it is the
 * agent's most recent one, as long as nothing else changes the map in between.
 * Returns `{"schema": "tbcli.undo/1", "session": "live", "rev", "undone"}`, with the
 * live rev after the undo and the name of the undone step.
 *
 * @throw CliError `undo_refused` if the most recent change to the map is not the
 * agent's own batch: the mapper edited, selected, undid or redid something since the
 * agent's last request, or there is nothing to undo; `busy` if a transaction is open.
 * The map is unchanged in either case.
 */
Json undoLive(LiveSession& session);

/**
 * Saves the open map of a live session as the editor's Save does: writes it to the
 * map's file atomically, after which the map has no unsaved changes. Given an output
 * path other than the map's file, writes a copy there instead and leaves the map's file
 * and its unsaved changes alone. A save does not change the map, so the live rev stays
 * as it was. Returns `{"schema": "tbcli.save/1", "session": "live", "rev", "path",
 * "modified"}`, with the absolute path written and whether the open map still has
 * unsaved changes.
 *
 * @throw CliError `write_failed` if the file cannot be written, which leaves it as it
 * was and the map's unsaved changes as they were; `invalid_argument` if the output path
 * names the map's own file; `busy` if a transaction is open.
 */
Json saveLive(LiveSession& session, const std::optional<std::filesystem::path>& output);

/**
 * The nodes that changed in a live session after the given counter of its rev, as far
 * as its change log has them, one entry per node, in the order of their latest changes:
 * `[{kind, path?, handle, origin, rev, undo?, redo?}]`. `kind` is `created` for a node
 * that is in the map now but was not then, `removed` for one that was in the map then
 * but is not now, which has no `path`, and `modified` for one that was and is. A node
 * that came and went again in between is left out. `origin` (`mapper` or `agent`), `rev`
 * and `undo` or `redo` (true if the change undid or redid an undo step) are those of the
 * node's latest change.
 */
Json changeSummary(LiveSession& session, uint64_t since);

/**
 * What changed in a live session's map since the given live rev:
 * `{"schema": "tbcli.changes/1", "session": "live", "rev", "since", "changes"}`, with
 * `changes` as changeSummary gives them.
 *
 * @throw CliError `invalid_argument` if `since` is no live rev, or a rev this session has
 * not reached yet; `stale_session` if it is from another session of the map;
 * `too_old` if it is older than the change log remembers
 */
Json changesLive(LiveSession& session, std::string_view since);

} // namespace tb::cli
