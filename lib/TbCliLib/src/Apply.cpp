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

#include "cli/Apply.h"

#include "cli/CliError.h"
#include "cli/FileUtils.h"
#include "cli/IssueTracker.h"
#include "cli/Json.h"
#include "cli/LinkedGroups.h"
#include "cli/LiveSession.h"
#include "cli/LoadMap.h"
#include "cli/NodeHandles.h"
#include "cli/NodePath.h"
#include "cli/Ops.h"
#include "cli/Targets.h"
#include "cli/Verify.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/CommandProcessor.h"
#include "mdl/Map.h"
#include "mdl/Map_Selection.h"
#include "mdl/Selection.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"

#include "kd/contracts.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace tb::cli
{
namespace
{

/** The NodePath and Target arguments of an op, resolved up front, by argument name. */
using ResolvedArgs = std::map<std::string, ResolvedTarget, std::less<>>;

/** The nodes of an op's NodePath and Target arguments, by argument name. */
using ArgNodes = std::map<std::string, std::vector<NodeOrFace>, std::less<>>;

/** Resolves the paths and queries of an op against the input rev. */
ResolvedArgs resolveArgs(const TargetResolver& targets, const Op& op)
{
  auto result = ResolvedArgs{};
  for (const auto& arg : op.def->args)
  {
    if (const auto* value = op.args.find(arg.name); value && addressesNodes(arg))
    {
      result.emplace(arg.name, targets.resolve(*value));
    }
  }
  return result;
}

/** The nodes an op addresses now, checked against the rules of its arguments. */
ArgNodes argNodes(const TargetResolver& targets, const Op& op, const ResolvedArgs& args)
{
  auto result = ArgNodes{};
  for (const auto& arg : op.def->args)
  {
    const auto it = args.find(arg.name);
    if (it == args.end())
    {
      continue;
    }

    auto nodes =
      targets.nodes(it->second, arg.rules, fmt::format("{}: {}", op.def->name, arg.name));
    if (arg.type == ArgType::NodePath && nodes.size() != 1)
    {
      const auto& value = op.args[arg.name].asString();
      throw CliError{
        ErrorCode::WrongNodeKind,
        fmt::format(
          "{}: {} {} names {} nodes, but must name one",
          op.def->name,
          arg.name,
          value,
          nodes.size()),
        value};
    }
    result.emplace(arg.name, std::move(nodes));
  }
  return result;
}

/** The map as the editor would save it. */
std::string serialize(const mdl::Map& map)
{
  auto stream = std::ostringstream{};
  map.saveTo(stream);
  return std::move(stream).str();
}

CliError lossyLoad(const std::vector<LoadProblem>& problems)
{
  return CliError{
    ErrorCode::LossyLoad,
    fmt::format(
      "the map did not load exactly ({}{}), so writing it could lose content; pass "
      "--allow-lossy to write anyway, or --dry-run to preview the edit",
      problems.front().message,
      problems.size() > 1 ? fmt::format(" and {} more", problems.size() - 1) : "")};
}

/**
 * The paths in the new rev of those of the given nodes that are still in the map, or
 * whose copies are (see TargetResolver::replace), each once.
 */
NodePathList newPaths(
  const std::vector<mdl::Node*>& nodes,
  const TargetResolver& targets,
  const std::unordered_map<const mdl::Node*, std::string>& paths,
  NodeHandleTable* handles)
{
  auto result = NodePathList{handles};
  auto seen = std::unordered_set<const mdl::Node*>{};
  for (auto* node : nodes)
  {
    const auto* current = targets.current(node);
    if (
      const auto it = paths.find(current);
      it != paths.end() && seen.insert(current).second)
    {
      result.push(it->second, *current);
    }
  }
  return result;
}

/**
 * The given issues whose nodes are still in the map, with their paths in the new rev.
 */
Json issuesJson(
  const std::vector<VerifyIssue>& issues,
  const std::unordered_map<const mdl::Node*, std::string>& paths,
  NodeHandleTable* handles)
{
  auto result = Json::array();
  for (const auto& issue : issues)
  {
    if (paths.contains(issue.node))
    {
      result.push(issueToJson(issue, paths, handles));
    }
  }
  return result;
}

/** Turns command collation off while it lives, and restores it after. */
class CollationOff
{
private:
  mdl::Map& m_map;
  bool m_wasEnabled;

public:
  explicit CollationOff(mdl::Map& map)
    : m_map{map}
    , m_wasEnabled{map.isCommandCollationEnabled()}
  {
    m_map.setIsCommandCollationEnabled(false);
  }

  ~CollationOff() { m_map.setIsCommandCollationEnabled(m_wasEnabled); }

  CollationOff(const CollationOff&) = delete;
  CollationOff& operator=(const CollationOff&) = delete;
};

/** The mapper's selection, nodes and faces, to restore after a batch. */
class SavedSelection
{
private:
  std::vector<mdl::Node*> m_nodes;
  std::vector<mdl::BrushFaceHandle> m_faces;

public:
  explicit SavedSelection(const mdl::Map& map)
    : m_nodes{map.selection().nodes}
    , m_faces{map.selection().brushFaces}
  {
  }

  /** Selects the saved nodes and faces again, apart from those no longer in the map. */
  void restore(mdl::Map& map) const
  {
    const auto& world = map.worldNode();
    const auto inMap = [&](const mdl::Node& node) { return node.isDescendantOf(world); };

    mdl::deselectAll(map);

    auto nodes = std::vector<mdl::Node*>{};
    std::ranges::copy_if(
      m_nodes, std::back_inserter(nodes), [&](const auto* node) { return inMap(*node); });
    if (!nodes.empty())
    {
      mdl::selectNodes(map, nodes);
    }

    auto faces = std::vector<mdl::BrushFaceHandle>{};
    std::ranges::copy_if(m_faces, std::back_inserter(faces), [&](const auto& face) {
      return inMap(face.node()) && face.faceIndex() < face.node().brush().faceCount();
    });
    if (!faces.empty())
    {
      mdl::selectBrushFaces(map, faces);
    }
  }
};

/** Where a rev given by the agent is in a live session's history. */
enum class RevPlace
{
  /** not a live rev */
  NotALiveRev,
  /** a rev of another session */
  OtherSession,
  /** a rev of this session that the map has not reached */
  Future,
  /** older than the change log remembers */
  TooOld,
  /** a rev whose changes since the change log has */
  Logged,
};

/** Where the given rev is in the session's history, and its counter if it has one. */
std::pair<RevPlace, uint64_t> placeRev(LiveSession& session, const std::string_view rev)
{
  const auto parsed = parseLiveRev(rev);
  if (!parsed)
  {
    return {RevPlace::NotALiveRev, 0};
  }
  if (parsed->epoch != session.epoch())
  {
    return {RevPlace::OtherSession, 0};
  }

  const auto& changeLog = session.changeLog();
  const auto place = parsed->counter > changeLog.counter() ? RevPlace::Future
                     : !changeLog.covers(parsed->counter)  ? RevPlace::TooOld
                                                           : RevPlace::Logged;
  return {place, parsed->counter};
}

/**
 * Checks that the ops may run although the map moved on from the rev the agent expects:
 * no node that they name by handle was touched since, and they name none by path, which
 * may have shifted. Query targets are run against the current map, and only checked
 * against their `expect`.
 *
 * @throw CliError `stale_rev` naming the conflicting targets, with the changes since the
 * expected rev; or, without them, if the expected rev is not one of this session's or
 * older than the change log remembers
 */
void checkExpectedRev(
  LiveSession& session, const std::vector<Op>& ops, const std::string& expectRev)
{
  if (expectRev == session.rev())
  {
    return;
  }

  const auto staleRev = [&](const std::string_view reason) {
    return CliError{
      ErrorCode::StaleRev,
      fmt::format(
        "the map's rev is {}, not the expected {}{}; read it again before editing",
        session.rev(),
        expectRev,
        reason)};
  };

  const auto [place, since] = placeRev(session, expectRev);
  if (place != RevPlace::Logged && place != RevPlace::TooOld)
  {
    throw staleRev(", which is not a rev of this session");
  }
  if (place == RevPlace::TooOld)
  {
    throw staleRev(", which is older than the editor's change log remembers");
  }

  const auto changed = session.changeLog().changedSince(since);
  auto& handles = session.handles();
  auto conflicts = Json::array();
  for (size_t i = 0; i < ops.size(); ++i)
  {
    for (const auto& arg : ops[i].def->args)
    {
      const auto* value = ops[i].args.find(arg.name);
      if (!value || !addressesNodes(arg))
      {
        continue;
      }

      for (const auto& reference : nodeReferences(*value))
      {
        auto conflict = Json::object();
        conflict.set("op_index", i);
        conflict.set("node", reference);
        if (!isNodeHandle(reference))
        {
          // the mapper's edits may have shifted the path
          conflict.set("reason", "path");
        }
        else if (const auto serialNumber = handles.serialNumber(reference))
        {
          if (const auto* node = handles.findNode(*serialNumber))
          {
            const auto touch = howTouched(*node, changed);
            if (!touch)
            {
              continue;
            }
            conflict.set("path", nodePath(*node));
            conflict.set("reason", toString(*touch));
          }
          else if (changed.contains(*serialNumber))
          {
            conflict.set("reason", "removed");
          }
          else
          {
            // gone before the expected rev, which resolving the handle reports
            continue;
          }
        }
        else
        {
          // resolving the handle reports what is wrong with it
          continue;
        }
        conflicts.push(std::move(conflict));
      }
    }
  }

  if (conflicts.size() > 0)
  {
    throw staleRev(
      fmt::format(
        ", and {} of the batch's targets {} changed since or named by "
        "path (see conflicts and changes)",
        conflicts.size(),
        conflicts.size() == 1 ? "was" : "were"))
      .withConflicts(std::move(conflicts), changeSummary(session, since));
  }
}

void throwIfBusy(const mdl::Map& map)
{
  if (map.commandProcessor().isTransactionActive())
  {
    // the editor host waits for the mapper's transaction before it runs a request
    throw CliError{
      ErrorCode::Busy, "the mapper is in the middle of an action; retry shortly"};
  }
}

} // namespace

BatchResult runBatch(
  mdl::Map& map,
  const std::vector<Op>& ops,
  const bool allowLocked,
  const VerifyConfig& verifyConfig,
  NodeHandleTable* handles)
{
  contract_pre(!map.isCommandCollationEnabled());

  auto targets = TargetResolver{map, allowLocked, handles};

  // resolve every node path and query before anything changes
  auto resolved = std::vector<ResolvedArgs>{};
  for (size_t i = 0; i < ops.size(); ++i)
  {
    resolved.push_back(attributeToOp(i, [&]() { return resolveArgs(targets, ops[i]); }));
  }

  auto issueTracker = IssueTracker{map, verifyConfig};
  auto outcomes = std::vector<OpOutcome>(ops.size());
  auto introduced = std::vector<std::vector<VerifyIssue>>(ops.size());
  for (size_t i = 0; i < ops.size(); ++i)
  {
    attributeToOp(i, [&]() {
      const auto nodes = argNodes(targets, ops[i], resolved[i]);
      auto& outcome = outcomes[i];
      auto context =
        OpContext{ops[i].def->name, map, ops[i].args, nodes, outcome, allowLocked};
      const auto linkedGroups = LinkedGroupSnapshot{map.worldNode()};
      ops[i].def->execute(context);
      linkedGroups.recordLinkedUpdates(map.worldNode(), outcome);
      targets.replace(outcome.replaced);
      if (const auto binding = ops[i].binding())
      {
        targets.bind(*binding, outcome.created);
      }
      introduced[i] = issueTracker.introduced(outcome);
    });
  }

  const auto paths = nodePaths(map.worldNode());
  auto result = BatchResult{Json::array(), Json::object(), Json::object()};
  for (size_t i = 0; i < ops.size(); ++i)
  {
    const auto& outcome = outcomes[i];

    // nodes that a batch creates and removes again have no path in either rev
    auto removed = NodePathList{handles};
    for (const auto* node : outcome.removed)
    {
      if (const auto path = targets.inputPath(*node))
      {
        removed.push(*path, *node);
      }
    }

    auto warnings = Json::array();
    for (const auto& warning : outcome.warnings)
    {
      warnings.push(warning);
    }

    auto created = newPaths(outcome.created, targets, paths, handles);
    // a binding whose nodes are all gone from the new rev is left out
    if (const auto binding = ops[i].binding(); binding && created.size() > 0)
    {
      const auto bindsOne = created.size() == 1 && !ops[i].def->bindsList;
      const auto bound = [&](const Json& list) { return bindsOne ? list[0] : list; };
      result.bindings.set(*binding, bound(created.paths()));
      if (handles)
      {
        result.bindingHandles.set(*binding, bound(created.handles()));
      }
    }

    auto opResult = Json::object();
    opResult.set("ok", true);
    std::move(created).setOn(opResult, "created");
    newPaths(outcome.modified, targets, paths, handles).setOn(opResult, "modified");
    std::move(removed).setOn(opResult, "removed");
    opResult.set("warnings", std::move(warnings));
    opResult.set("issues_introduced", issuesJson(introduced[i], paths, handles));
    if (!outcome.instancesAffected.empty())
    {
      newPaths(outcome.instancesAffected, targets, paths, handles)
        .setOn(opResult, "instances_affected");
    }
    if (!outcome.stalePlacements.empty())
    {
      newPaths(outcome.stalePlacements, targets, paths, handles)
        .setOn(opResult, "stale_placements");
    }
    result.ops.push(std::move(opResult));
  }
  return result;
}

Json apply(
  LoadedMap& loadedMap,
  const std::vector<Op>& ops,
  const ApplyOptions& options,
  const VerifyConfig& verifyConfig)
{
  if (options.expectRev && *options.expectRev != loadedMap.rev)
  {
    throw CliError{
      ErrorCode::StaleRev,
      fmt::format(
        "the map's rev is {}, not the expected {}; read it again before editing",
        loadedMap.rev,
        *options.expectRev)};
  }

  const auto lossy = !loadedMap.loadProblems.empty();
  if (lossy && !options.allowLossy && !options.dryRun)
  {
    throw lossyLoad(loadedMap.loadProblems);
  }

  auto& map = *loadedMap.map;
  // collated commands delete the nodes that the earlier one replaced, which ops of the
  // batch still refer to
  map.setIsCommandCollationEnabled(false);
  auto batch = runBatch(map, ops, options.allowLocked, verifyConfig, nullptr);

  const auto bytes = serialize(map);
  if (!options.dryRun)
  {
    const auto target =
      options.output ? std::filesystem::absolute(*options.output) : map.path();
    if (const auto error = writeFileAtomically(target, bytes))
    {
      throw CliError{
        ErrorCode::WriteFailed,
        fmt::format("cannot write {}, nothing was written: {}", target, *error)};
    }
  }

  auto result = Json::object();
  result.set("schema", "tbcli.apply/1");
  result.set("rev", computeRev(bytes));
  result.set("ops", std::move(batch.ops));
  result.set("bindings", std::move(batch.bindings));
  if (lossy)
  {
    auto warnings = Json::array();
    warnings.push("lossy_load");
    result.set("warnings", std::move(warnings));
  }
  return result;
}

std::string batchLabel(const std::vector<Op>& ops)
{
  if (ops.empty())
  {
    return "empty batch";
  }
  auto result = std::string{ops[0].def->name};
  if (ops.size() > 1)
  {
    result += fmt::format(", {}", ops[1].def->name);
  }
  if (ops.size() > 2)
  {
    result += fmt::format(" +{} more", ops.size() - 2);
  }
  return result;
}

std::string liveUndoName(const std::vector<Op>& ops)
{
  return "AI: " + batchLabel(ops);
}

Json applyLive(
  LiveSession& session,
  const std::vector<Op>& ops,
  const ApplyOptions& options,
  const VerifyConfig& verifyConfig)
{
  auto& map = session.map();
  throwIfBusy(map);
  if (options.expectRev)
  {
    checkExpectedRev(session, ops, *options.expectRev);
  }

  auto& history = session.agentHistory();
  const auto revBefore = session.rev();
  const auto stepBefore = map.commandProcessor().undoCommandSerialNumber();
  auto agentChanges = ChangeLog::AgentChanges{session.changeLog()};

  const auto selection = SavedSelection{map};
  // collated commands delete the nodes that the earlier one replaced, which ops of the
  // batch still refer to
  const auto collationOff = CollationOff{map};
  map.startTransaction(liveUndoName(ops), mdl::TransactionScope::LongRunning);
  map.markTransactionAsCollationBarrier();
  // whether the history knows how this request ended
  auto recorded = false;

  try
  {
    // ops select what they edit, and a selected face would soft-lock linked copies
    mdl::deselectAll(map);
    auto batch =
      runBatch(map, ops, options.allowLocked, verifyConfig, &session.handles());
    selection.restore(map);

    auto rev = std::string{};
    auto storedBatch = std::optional<uint64_t>{};
    if (options.output)
    {
      // as in file mode, with the rev of the (would-be) output file
      const auto bytes = serialize(map);
      const auto target = std::filesystem::absolute(*options.output);
      if (!options.dryRun)
      {
        if (const auto error = writeFileAtomically(target, bytes))
        {
          throw CliError{
            ErrorCode::WriteFailed,
            fmt::format("cannot write {}, nothing was written: {}", target, *error)};
        }
      }
      map.cancelTransaction();
      agentChanges.discard();
      rev = computeRev(bytes);
    }
    else if (options.dryRun)
    {
      map.cancelTransaction();
      agentChanges.discard();
      rev = session.rev();
    }
    else
    {
      if (!map.commitTransaction())
      {
        throw CliError{ErrorCode::OpFailed, "the editor could not commit the batch"};
      }
      // an empty batch stores no undo step
      const auto stepAfter = map.commandProcessor().undoCommandSerialNumber();
      storedBatch = stepAfter != stepBefore ? stepAfter : std::nullopt;
      rev = session.rev();
    }
    history.requestDone(revBefore, session.rev(), storedBatch);
    recorded = true;

    auto result = Json::object();
    result.set("schema", "tbcli.apply/1");
    result.set("session", "live");
    result.set("rev", std::move(rev));
    result.set("ops", std::move(batch.ops));
    result.set("bindings", std::move(batch.bindings));
    result.set(handleKey("bindings", true), std::move(batch.bindingHandles));
    return result;
  }
  catch (...)
  {
    // also the transactions of an op that failed halfway
    while (map.commandProcessor().isTransactionActive())
    {
      map.cancelTransaction();
    }
    agentChanges.discard();
    if (!recorded)
    {
      history.requestDone(revBefore, session.rev(), std::nullopt);
    }
    throw;
  }
}

Json undoLive(LiveSession& session)
{
  auto& map = session.map();
  throwIfBusy(map);

  auto& history = session.agentHistory();
  const auto batch = history.mostRecentBatch(session.rev());
  const auto agentChanges = ChangeLog::AgentChanges{session.changeLog()};
  const auto step = map.commandProcessor().undoCommandSerialNumber();
  if (!step)
  {
    throw CliError{ErrorCode::UndoRefused, "there is nothing to undo"};
  }
  if (step != batch)
  {
    throw CliError{
      ErrorCode::UndoRefused,
      fmt::format(
        "the most recent undo step, '{}', is not your own most recent batch: the map or "
        "the selection changed without you since your last request. tbcli undo only "
        "undoes your own most recent batch; edit the map back with apply instead, or "
        "ask the mapper to undo",
        *map.undoCommandName())};
  }

  const auto name = *map.undoCommandName();
  map.undoCommand();
  if (map.commandProcessor().undoCommandSerialNumber() == step)
  {
    throw CliError{
      ErrorCode::OpFailed, fmt::format("the editor could not undo '{}'", name)};
  }
  history.batchUndone(session.rev());

  auto result = Json::object();
  result.set("schema", "tbcli.undo/1");
  result.set("session", "live");
  result.set("rev", session.rev());
  result.set("undone", name);
  return result;
}

Json saveLive(LiveSession& session, const std::optional<std::filesystem::path>& output)
{
  auto& map = session.map();
  throwIfBusy(map);

  if (output && isSameFile(*output, map.path()))
  {
    throw invalidArgument(
      "the output file is the map's own file; run tbcli save without -o to save the "
      "map");
  }

  // not the editor's own Save, which opens Save As or a message box on failure
  const auto path = output ? std::filesystem::absolute(*output) : map.path();
  const auto saved = output ? map.saveTo(path) : map.save();
  if (saved.is_error())
  {
    throw CliError{
      ErrorCode::WriteFailed,
      fmt::format(
        "{} was not saved, and the file is as it was: {}",
        output ? "the copy" : "the map",
        std::get<Error>(saved.error()).msg)};
  }

  auto result = Json::object();
  result.set("schema", "tbcli.save/1");
  result.set("session", "live");
  result.set("rev", session.rev());
  result.set("path", path.string());
  result.set("modified", map.modified());
  return result;
}

std::vector<NodeChange> summarizeChanges(LiveSession& session, const uint64_t since)
{
  auto changes = session.changeLog().changesSince(since);

  // whether each node was in the map at `since`, and its latest change
  auto wasInMap = std::unordered_map<uint64_t, bool>{};
  auto latest = std::unordered_map<uint64_t, const Change*>{};
  for (const auto& change : changes)
  {
    wasInMap.try_emplace(change.serialNumber, change.kind != ChangeKind::Created);
    latest[change.serialNumber] = &change;
  }

  auto& handles = session.handles();
  auto result = std::vector<NodeChange>{};
  for (auto& change : changes)
  {
    if (latest[change.serialNumber] != &change)
    {
      continue;
    }

    auto* node = handles.findNode(change.serialNumber);
    const auto wasThere = wasInMap[change.serialNumber];
    if (!wasThere && !node)
    {
      continue;
    }

    const auto kind = !wasThere ? ChangeKind::Created
                      : node    ? ChangeKind::Modified
                                : ChangeKind::Removed;
    result.push_back(NodeChange{kind, node, std::move(change)});
  }
  return result;
}

Json changeSummary(LiveSession& session, const uint64_t since)
{
  auto& handles = session.handles();
  auto result = Json::array();
  for (const auto& [kind, node, change] : summarizeChanges(session, since))
  {
    auto entry = Json::object();
    entry.set("kind", toString(kind));
    if (node)
    {
      entry.set("path", nodePath(*node));
    }
    entry.set("handle", handles.handle(change.serialNumber));
    entry.set("origin", toString(change.origin));
    entry.set("rev", formatLiveRev(session.epoch(), change.counter));
    if (change.via == ChangeVia::Undo)
    {
      entry.set("undo", true);
    }
    else if (change.via == ChangeVia::Redo)
    {
      entry.set("redo", true);
    }
    result.push(std::move(entry));
  }
  return result;
}

Json changesLive(LiveSession& session, const std::string_view since)
{
  const auto [place, counter] = placeRev(session, since);
  if (place == RevPlace::NotALiveRev)
  {
    throw invalidArgument(
      fmt::format(
        "--since takes a live rev, live-<epoch>:<counter>, as reads in a live session "
        "report it, not '{}'",
        since));
  }
  if (place == RevPlace::OtherSession)
  {
    throw CliError{
      ErrorCode::StaleSession,
      fmt::format(
        "the rev {} is from an earlier session of this map, which was reopened or "
        "reloaded since; read the map again",
        since)};
  }

  if (place == RevPlace::Future)
  {
    throw invalidArgument(
      fmt::format(
        "the map has not reached the rev {} yet; its rev is {}", since, session.rev()));
  }
  if (place == RevPlace::TooOld)
  {
    throw CliError{
      ErrorCode::TooOld,
      fmt::format(
        "the rev {} is older than the editor's change log remembers; read the map again",
        since)};
  }

  auto result = Json::object();
  result.set("schema", "tbcli.changes/1");
  result.set("session", "live");
  result.set("rev", session.rev());
  result.set("since", std::string{since});
  result.set("changes", changeSummary(session, counter));
  return result;
}

} // namespace tb::cli
