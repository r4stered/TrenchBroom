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

#include <memory>
#include <optional>
#include <string_view>
#include <typeindex>
#include <vector>

namespace tb::mdl
{
class Validator;
}

namespace tb::cli
{

class Json;

/** How bad an issue is; ordered from least to most severe. */
enum class Severity
{
  Info,
  Warning,
  Error,
};

std::string_view toString(Severity severity);

/** The severity with the given name (`info`, `warning` or `error`), if there is one. */
std::optional<Severity> parseSeverity(std::string_view name);

/** Where issues of a type come from. */
enum class IssueSource
{
  /** what the map reader skipped */
  LoadDiagnostics,
  /** a validator that the map library registers */
  Validator,
  /** a validator of tbcli's own checks */
  CliCheck,
};

/** What an issue is about. */
enum class IssueScope
{
  /** the node it is on */
  Node,
  /**
   * the whole map, so that it has no node and its key covers only its type and
   * message
   */
  Map,
};

/** A quick-fix of an issue type, which the `fix` op applies. */
struct QuickFixDef
{
  /** The stable snake_case id that the `fix` op and issue records name it by. */
  std::string_view id;
  /** The description of the validator's quick-fix, which identifies it. */
  std::string_view description;
};

/** One row of the issue-type table. */
struct IssueTypeDef
{
  /** The stable snake_case id that issues carry as their `type`. */
  std::string_view id;
  Severity severity;
  IssueSource source;
  /** For a validator type, the class of the validator that reports the issues. */
  std::optional<std::type_index> validator = std::nullopt;
  IssueScope scope = IssueScope::Node;
  /** For a CLI check type, creates the validator that reports the issues. */
  std::unique_ptr<mdl::Validator> (*createValidator)() = nullptr;
  /** The quick-fixes of the type's validator, in the validator's order. */
  std::vector<QuickFixDef> fixes = {};
};

/**
 * The issue-type table: every issue type verify reports, with its default severity, in
 * a fixed order. It is written out explicitly rather than derived from the map library's
 * runtime issue-type bits, so that ids stay stable and are part of `tbcli.verify/1`.
 */
const std::vector<IssueTypeDef>& issueTypes();

/** The issue type with the given id, or nullptr. */
const IssueTypeDef* findIssueType(std::string_view id);

/** The issue type of the issues that the given validator reports, or nullptr. */
const IssueTypeDef* findIssueType(const mdl::Validator& validator);

/** The quick-fix of the given type with the given id, or nullptr. */
const QuickFixDef* findQuickFix(const IssueTypeDef& type, std::string_view id);

/** The ids of the quick-fixes for issues of the given type, as a JSON array. */
Json fixIdsJson(const IssueTypeDef& type);

/** The issue-type table as `ops --json` lists it: `[{type, severity, fixes}]`. */
Json issueTypesJson();

} // namespace tb::cli
