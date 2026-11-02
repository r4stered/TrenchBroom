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
#include "cli/IssueTypes.h"
#include "cli/Json.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace tb::mdl
{
class Node;
}

namespace tb::cli
{

struct LoadedMap;
struct VerifyConfig;

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
  std::optional<size_t> faceIndex;
  std::optional<size_t> line;
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
 * registers. Applies the severities and suppressions of the given configuration.
 *
 * @throw CliError if a validator is not in the issue-type table
 */
IssueReport collectIssues(LoadedMap& loadedMap, const VerifyConfig& config);

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
 * leaves out suppressed issues.
 *
 * @throw CliError if the target path does not resolve
 */
VerifyResult verify(
  LoadedMap& loadedMap, const VerifyConfig& config, const VerifyOptions& options);

} // namespace tb::cli
