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
#include "cli/Json.h"
#include "cli/LoadMap.h"
#include "cli/NodePath.h"
#include "cli/Ops.h"
#include "mdl/Map.h"
#include "mdl/WorldNode.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <map>
#include <sstream>
#include <unordered_map>

namespace tb::cli
{
namespace
{

using ResolvedNodes = std::map<std::string, NodeOrFace, std::less<>>;

/** Resolves the node path arguments of an op against the input rev. */
ResolvedNodes resolveNodes(mdl::WorldNode& worldNode, const Op& op)
{
  auto result = ResolvedNodes{};
  for (const auto& arg : op.def->args)
  {
    if (arg.type != ArgType::NodePath)
    {
      continue;
    }
    if (const auto* value = op.args.find(arg.name))
    {
      result.emplace(arg.name, resolveNodePath(worldNode, value->asString()));
    }
  }
  return result;
}

/** Fails if an earlier op of the batch removed a node that this op addresses. */
void checkNotDeleted(
  const mdl::WorldNode& worldNode, const Op& op, const ResolvedNodes& nodes)
{
  for (const auto& [name, nodeOrFace] : nodes)
  {
    if (!nodeOrFace.node->isDescendantOf(worldNode) && nodeOrFace.node != &worldNode)
    {
      const auto& path = op.args[name].asString();
      throw CliError{
        ErrorCode::TargetDeleted,
        fmt::format(
          "{}: {} was removed by an earlier op of the batch", op.def->name, path),
        path};
    }
  }
}

/** The map as the editor would save it. */
std::string serialize(const mdl::Map& map)
{
  auto stream = std::ostringstream{};
  map.saveTo(stream);
  return std::move(stream).str();
}

/** The paths in the new rev of those of the given nodes that are still in the map. */
Json newPaths(
  const std::vector<mdl::Node*>& nodes,
  const std::unordered_map<const mdl::Node*, std::string>& paths)
{
  auto result = Json::array();
  for (const auto* node : nodes)
  {
    if (const auto it = paths.find(node); it != paths.end())
    {
      result.push(it->second);
    }
  }
  return result;
}

} // namespace

Json apply(LoadedMap& loadedMap, const std::vector<Op>& ops, const ApplyOptions& options)
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

  auto& map = *loadedMap.map;
  auto& worldNode = map.worldNode();
  const auto inputPaths = nodePaths(worldNode);

  // resolve every node path before anything changes
  auto resolved = std::vector<ResolvedNodes>{};
  for (size_t i = 0; i < ops.size(); ++i)
  {
    resolved.push_back(
      attributeToOp(i, [&]() { return resolveNodes(worldNode, ops[i]); }));
  }

  auto outcomes = std::vector<OpOutcome>(ops.size());
  for (size_t i = 0; i < ops.size(); ++i)
  {
    attributeToOp(i, [&]() {
      checkNotDeleted(worldNode, ops[i], resolved[i]);
      auto context = OpContext{map, ops[i].args, resolved[i], outcomes[i]};
      ops[i].def->execute(context);
    });
  }

  const auto paths = nodePaths(worldNode);
  auto opResults = Json::array();
  auto bindings = Json::object();
  for (size_t i = 0; i < ops.size(); ++i)
  {
    const auto& outcome = outcomes[i];

    auto removed = Json::array();
    for (const auto* node : outcome.removed)
    {
      removed.push(inputPaths.at(node));
    }

    auto warnings = Json::array();
    for (const auto& warning : outcome.warnings)
    {
      warnings.push(warning);
    }

    auto created = newPaths(outcome.created, paths);
    // a binding whose nodes are all gone from the new rev is left out
    if (const auto binding = ops[i].binding(); binding && created.size() > 0)
    {
      bindings.set(*binding, created.size() == 1 ? created[0] : created);
    }

    auto opResult = Json::object();
    opResult.set("ok", true);
    opResult.set("created", std::move(created));
    opResult.set("modified", newPaths(outcome.modified, paths));
    opResult.set("removed", std::move(removed));
    opResult.set("warnings", std::move(warnings));
    opResults.push(std::move(opResult));
  }

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
  result.set("ops", std::move(opResults));
  result.set("bindings", std::move(bindings));
  return result;
}

} // namespace tb::cli
