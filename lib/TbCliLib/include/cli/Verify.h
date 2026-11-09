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

#include "cli/CappedList.h"
#include "cli/Compile.h"
#include "cli/IssueTypes.h"
#include "cli/Json.h"
#include "cli/MapRef.h"

#include "vm/vec.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::mdl
{
class BrushNode;
class Map;
class Node;
class Validator;
} // namespace tb::mdl

namespace tb::cli
{

class NodeHandleTable;
struct VerifyConfig;

/** Where a leaking map's hole is, as the `leak` issue reports it. */
struct LeakDetails
{
  /**
   * The trail from the occupant entity to the void, reduced to its endpoints and
   * turning points; empty if qbsp wrote no pointfile that could be read.
   */
  std::vector<vm::vec3d> trail;
  /** The brushes nearest where the trail leaves the map, nearest first. */
  std::vector<mdl::BrushNode*> nearestBrushes;
};

/** One finding of a verify stage. */
struct VerifyIssue
{
  const IssueTypeDef* type = nullptr;
  /** The type's severity, or the one tbcli.json sets for the type. */
  Severity severity = Severity::Error;
  /**
   * A short hash of the type and the serialized text of the node (or, for issues
   * without a node, of the message), which identifies the issue across revs while the
   * node is unchanged.
   */
  std::string key;
  std::string message;
  /** The node the issue is on, if any. */
  mdl::Node* node = nullptr;
  /** Set if the issue is on a face of `node`, which is then a brush. */
  std::optional<size_t> faceIndex = std::nullopt;
  /** Set if the issue is about a property of `node`, which is then an entity. */
  std::optional<std::string> propertyKey = std::nullopt;
  std::optional<size_t> line = std::nullopt;
  /** For an issue a compiler reported, the compiler's name, such as `qbsp`. */
  std::optional<std::string> tool = std::nullopt;
  /**
   * Whether the issue was mapped to a node after a lossy load, so that the map the
   * compiler saw holds content that the loaded map lacks.
   */
  bool unreliableMapping = false;
  /** Set for a `leak` issue, whose node is the occupant entity, if qbsp named one. */
  std::optional<LeakDetails> leak = std::nullopt;
  /** Whether a `verify.ignore` entry suppresses the issue. */
  bool suppressed = false;
};

/** One step of the verify pipeline. */
struct VerifyStage
{
  std::string name;
  /** Whether the stage found any issue that is not suppressed. */
  bool hasIssues = false;
  double ms = 0.0;
  /** The issue types that the stage could not check, such as missing materials. */
  std::vector<const IssueTypeDef*> unchecked = {};
  /** Whether the stage's tool exited with a non-zero code or was stopped. */
  bool failed = false;
  /** Set if the stage did not run, as the reason, such as `leak`. */
  std::optional<std::string> skipReason = std::nullopt;
};

/** What the in-process verify stages found in a whole map. */
struct IssueReport
{
  /** Every issue, suppressed or not, in file order. */
  std::vector<VerifyIssue> issues;
  std::vector<VerifyStage> stages;
};

/**
 * Runs the in-process verify stages on the whole map: load diagnostics, which turn
 * what the reader skipped into `load_error` issues, then every validator the map
 * registers, then the CLI checks. Given compile options, then compiles the bytes of the
 * map's source, which must be given too, and turns the compilers' logs into `compiler`
 * issues, a leak into one `leak` issue and a failed compiler into a `compile_fatal`
 * issue. Applies the severities and suppressions of the given configuration. The load
 * diagnostics stage takes the source's load time, or none without a source.
 *
 * A compiler message that names a line (`[line N]`) is put on the innermost node
 * containing that line; one that names a classname and origin is put on the first
 * entity of that class whose bounds contain the origin. Other messages have no node.
 * The `leak` issue is put on the occupant entity that qbsp's `Reached occupant` lines
 * name, and replaces the `compiler` issues of those lines. It lists the three brushes
 * nearest where the leak trail leaves the map.
 *
 * @throw CliError if a validator is not in the issue-type table, or as `compile` does
 */
IssueReport collectIssues(
  const MapRef& mapRef,
  const VerifyConfig& config,
  const std::optional<CompileOptions>& compileOptions = std::nullopt,
  const std::optional<MapSource>& source = std::nullopt);

/**
 * The issue record `{type, severity, key, message, node, line, origin? | bounds?,
 * tool?, unreliable_mapping?, trail?, nearest_brushes?, fixes}`, with the node's path
 * taken from the given paths, which must contain it. In a live session (given its node
 * handles), `node_handle` and `nearest_brushes_handles` go next to the paths.
 */
Json issueToJson(
  const VerifyIssue& issue,
  const std::unordered_map<const mdl::Node*, std::string>& paths,
  NodeHandleTable* handles);

/**
 * The record of the `leak` issue that verify reports for the given compile, which must
 * have leaked, with the severity that the configuration sets for `leak`.
 */
Json leakIssueToJson(
  const MapRef& mapRef, const VerifyConfig& config, const CompileResult& compileResult);

/**
 * Collects the issues of single nodes as verify does, with their keys. Keys tell
 * otherwise alike issues apart by the order in which they are collected.
 */
class NodeIssueCollector
{
private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

public:
  /**
   * @throw CliError if a validator is not in the issue-type table
   */
  NodeIssueCollector(mdl::Map& map, std::vector<const mdl::Validator*> validators);
  ~NodeIssueCollector();

  NodeIssueCollector(const NodeIssueCollector&) = delete;
  NodeIssueCollector& operator=(const NodeIssueCollector&) = delete;

  /** The issues the validators find on the given node only, not below it. */
  std::vector<VerifyIssue> collect(mdl::Node& node);
};

/**
 * Applies the severities of the given configuration to the issues, and marks the issues
 * that it suppresses.
 */
void applyConfig(
  mdl::Map& map, const VerifyConfig& config, std::vector<VerifyIssue>& issues);

/**
 * Sets `counts: {<type>: n, ..., suppressed: n}` and `by_severity: {error, warning,
 * info}` on `object` for the given issues: the unsuppressed issues per type (types
 * without issues are left out) and per severity, and the number of suppressed issues.
 */
void setIssueCounts(Json& object, const std::vector<VerifyIssue>& issues);

struct VerifyOptions
{
  Paging paging;
  /** List only issues of these types; all types if empty. */
  std::vector<const IssueTypeDef*> types;
  /** List only issues of at least this severity. */
  std::optional<Severity> minSeverity;
  /**
   * List only issues on the node or face at this path, or on the nodes a TbQl query
   * matches, or inside them.
   */
  std::optional<std::string> target;
  /** The severity at which verify fails (exit 4). */
  std::optional<Severity> failOn;
  /** If set, verify also compiles the map (`--compile`). */
  std::optional<CompileOptions> compile;
};

struct VerifyResult
{
  Json json;
  /** Whether `failOn` is set and an unsuppressed issue is at or above it. */
  bool failed = false;
};

/**
 * The `tbcli.verify/1` result `{schema, rev, ok, stages, counts, by_severity, issues:
 * {items, total, truncated}}`.
 *
 * `ok` is whether no unsuppressed issue is at or above `failOn`, or `error` without it.
 * The counts cover the whole map; the filters and paging apply only to `issues`, which
 * leaves out suppressed issues. Compiling (`options.compile`) needs the map's source
 * (see collectIssues).
 *
 * @throw CliError if the target path does not resolve
 */
VerifyResult verify(
  const MapRef& mapRef,
  const VerifyConfig& config,
  const VerifyOptions& options,
  const std::optional<MapSource>& source);

} // namespace tb::cli
