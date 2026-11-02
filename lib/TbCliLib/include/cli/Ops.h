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
#include "cli/NodePath.h"
#include "cli/Targets.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tb::mdl
{
class Map;
class Node;
} // namespace tb::mdl

namespace tb::cli
{

/** The type of an op argument, which decides how it is validated, parsed and resolved. */
enum class ArgType
{
  /** a point `[x, y, z]`, written `x,y,z` as a flag */
  Vec3,
  /** any string */
  String,
  /**
   * one node: a node path, resolved against the input rev before the batch runs, or a
   * binding to exactly one node
   */
  NodePath,
  /**
   * the nodes an op acts on: a node path, a binding, a list of them, or a query
   * `{"query": <ql>, "expect": <n> | "any"}`; written --target <path>, or
   * --query <ql> --expect <n> as flags
   */
  Target,
  /** a binding name `$name` that the nodes the op creates are bound to */
  Binding,
};

/** One argument of an op. */
struct ArgDef
{
  std::string name;
  ArgType type = ArgType::String;
  std::string description;
  bool required = false;
  /** The value of an optional argument that is not given, if it has one. */
  std::optional<Json> defaultValue = std::nullopt;
  /** What a NodePath or Target argument accepts. */
  TargetRules rules = {};
};

/** Whether the argument addresses nodes: whether it is a NodePath or a Target. */
bool addressesNodes(const ArgDef& arg);

/** What an op did, as reported in its entry of the apply result. */
struct OpOutcome
{
  std::vector<mdl::Node*> created;
  std::vector<mdl::Node*> modified;
  /** Removed nodes; reported by their paths in the input rev. */
  std::vector<mdl::Node*> removed;
  std::vector<std::string> warnings;
};

/** What an op runs with. */
struct OpContext
{
  mdl::Map& map;
  /** The op's validated arguments, with defaults filled in. */
  const Json& args;
  /**
   * The nodes and faces of the NodePath and Target arguments by argument name, resolved
   * and checked against the argument's rules.
   */
  const std::map<std::string, std::vector<NodeOrFace>, std::less<>>& nodes;
  OpOutcome& outcome;
  /** Whether ops may edit locked nodes (--allow-locked). */
  bool allowLocked = false;

  /**
   * The node of the NodePath argument with the given name, which must be given or have
   * a default.
   */
  const NodeOrFace& node(std::string_view name) const;

  /** The nodes and faces of the Target argument with the given name, each once. */
  const std::vector<NodeOrFace>& targets(std::string_view name) const;
};

/**
 * The definition of an op. It is the single source of the op's batch parser, its
 * subcommand and flags, its `--help` text and its JSON Schema in `ops --json`.
 */
struct OpDef
{
  std::string name;
  std::string description;
  std::vector<ArgDef> args;
  /** Carries out the op. Throws CliError if the op fails. */
  std::function<void(OpContext&)> execute;
  /**
   * Checks the op's arguments beyond their types before the map is loaded, if set.
   * Throws CliError `invalid_argument` if they are malformed.
   */
  std::function<void(const Json& args)> checkArgs = nullptr;
};

/** The flags of an op's subcommand, as `(name, value)` with the leading dashes removed.
 */
using OpFlags = std::vector<std::pair<std::string, std::string>>;

/** An op of a batch with its validated arguments. */
struct Op
{
  const OpDef* def = nullptr;
  /** The arguments without `op`, with defaults filled in. */
  Json args;

  /** The name this op binds its created nodes to, if it has one. */
  std::optional<std::string> binding() const;

  /** The bindings this op's NodePath and Target arguments refer to. */
  std::vector<std::string> bindingReferences() const;
};

/** Every op, in the order `ops --json` and the help list them. */
const std::vector<OpDef>& opDefs();

/** The op with the given name, or nullptr. */
const OpDef* findOpDef(std::string_view name);

/**
 * The `tbcli.ops/1` result `{schema, ops: [{name, description, schema}], issue_types}`,
 * where each `schema` is a JSON Schema of the op's batch entry and `issue_types` is the
 * issue-type table that verify reports issues by.
 */
Json opsJson();

/** The help text of the options that every mutating command accepts. */
std::string_view mutatingOptionsHelp();

/** The `--help` text of an op's subcommand. */
std::string opHelp(const OpDef& def);

/**
 * Validates a batch: a JSON array of op objects `{"op": <name>, <args>...}`.
 *
 * @throw CliError `invalid_batch` if the batch is not an array of objects,
 * `invalid_argument` if an op is unknown or its arguments are missing, unknown or
 * malformed, if two ops bind the same name, or if an op refers to a binding that no
 * earlier op binds; both carry the op's index
 */
std::vector<Op> parseBatch(const Json& batch);

/**
 * Builds an op from the flags of its subcommand.
 *
 * A Target argument named `target` is given as `--target <path>`, or as
 * `--query <ql>` with an optional `--expect <n | any>`.
 *
 * @throw CliError `invalid_argument` if a flag is unknown, repeated or malformed, a
 * required one is missing, or a binding is referred to, since a single op has no
 * earlier op to bind it
 */
Op parseOpFlags(const OpDef& def, const OpFlags& flags);

} // namespace tb::cli
