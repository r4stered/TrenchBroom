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
#include "cli/LoadMap.h"
#include "cli/NodePath.h"
#include "cli/Ops.h"
#include "cli/Targets.h"
#include "cli/Verify.h"
#include "mdl/Map.h"
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
Json newPaths(
  const std::vector<mdl::Node*>& nodes,
  const TargetResolver& targets,
  const std::unordered_map<const mdl::Node*, std::string>& paths)
{
  auto result = Json::array();
  auto seen = std::unordered_set<const mdl::Node*>{};
  for (auto* node : nodes)
  {
    const auto* current = targets.current(node);
    if (
      const auto it = paths.find(current);
      it != paths.end() && seen.insert(current).second)
    {
      result.push(it->second);
    }
  }
  return result;
}

/**
 * The given issues whose nodes are still in the map, with their paths in the new rev.
 */
Json issuesJson(
  const std::vector<VerifyIssue>& issues,
  const std::unordered_map<const mdl::Node*, std::string>& paths)
{
  auto result = Json::array();
  for (const auto& issue : issues)
  {
    if (paths.contains(issue.node))
    {
      result.push(issueToJson(issue, paths));
    }
  }
  return result;
}

} // namespace

BatchResult runBatch(
  mdl::Map& map,
  const std::vector<Op>& ops,
  const bool allowLocked,
  const VerifyConfig& verifyConfig)
{
  contract_pre(!map.isCommandCollationEnabled());

  auto targets = TargetResolver{map, allowLocked};

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
  auto result = BatchResult{Json::array(), Json::object()};
  for (size_t i = 0; i < ops.size(); ++i)
  {
    const auto& outcome = outcomes[i];

    // nodes that a batch creates and removes again have no path in either rev
    auto removed = Json::array();
    for (const auto* node : outcome.removed)
    {
      if (const auto path = targets.inputPath(*node))
      {
        removed.push(*path);
      }
    }

    auto warnings = Json::array();
    for (const auto& warning : outcome.warnings)
    {
      warnings.push(warning);
    }

    auto created = newPaths(outcome.created, targets, paths);
    // a binding whose nodes are all gone from the new rev is left out
    if (const auto binding = ops[i].binding(); binding && created.size() > 0)
    {
      const auto bindsOne = created.size() == 1 && !ops[i].def->bindsList;
      result.bindings.set(*binding, bindsOne ? created[0] : created);
    }

    auto opResult = Json::object();
    opResult.set("ok", true);
    opResult.set("created", std::move(created));
    opResult.set("modified", newPaths(outcome.modified, targets, paths));
    opResult.set("removed", std::move(removed));
    opResult.set("warnings", std::move(warnings));
    opResult.set("issues_introduced", issuesJson(introduced[i], paths));
    if (!outcome.instancesAffected.empty())
    {
      opResult.set(
        "instances_affected", newPaths(outcome.instancesAffected, targets, paths));
    }
    if (!outcome.stalePlacements.empty())
    {
      opResult.set("stale_placements", newPaths(outcome.stalePlacements, targets, paths));
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
  auto batch = runBatch(map, ops, options.allowLocked, verifyConfig);

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

} // namespace tb::cli
