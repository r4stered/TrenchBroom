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

#include "cli/IssueTypes.h"

#include "cli/CliValidators.h"
#include "cli/Json.h"
#include "mdl/EmptyBrushEntityValidator.h"
#include "mdl/EmptyGroupValidator.h"
#include "mdl/EmptyPropertyKeyValidator.h"
#include "mdl/EmptyPropertyValueValidator.h"
#include "mdl/InvalidUvScaleValidator.h"
#include "mdl/LinkSourceValidator.h"
#include "mdl/LinkTargetValidator.h"
#include "mdl/LongPropertyKeyValidator.h"
#include "mdl/LongPropertyValueValidator.h"
#include "mdl/MissingClassnameValidator.h"
#include "mdl/MissingDefinitionValidator.h"
#include "mdl/MissingModValidator.h"
#include "mdl/MixedBrushContentsValidator.h"
#include "mdl/NonIntegerVerticesValidator.h"
#include "mdl/PointEntityWithBrushesValidator.h"
#include "mdl/PropertyKeyWithDoubleQuotationMarksValidator.h"
#include "mdl/PropertyValueWithDoubleQuotationMarksValidator.h"
#include "mdl/SoftMapBoundsValidator.h"
#include "mdl/Validator.h"
#include "mdl/WorldBoundsValidator.h"
#include "mdl/WorldNodePathSeparatorValidator.h"

#include <algorithm>
#include <memory>
#include <typeinfo>

namespace tb::cli
{
namespace
{

// the quick-fixes that several of the map library's validators offer
constexpr auto DeleteObjects = QuickFixDef{"delete_objects", "Delete Objects"};
constexpr auto DeleteProperty = QuickFixDef{"delete_property", "Delete Property"};
constexpr auto ReplaceDoubleQuotes =
  QuickFixDef{"replace_double_quotes", "Replace \" with '"};

/** A type whose issues the validator of class T reports. */
template <typename T>
IssueTypeDef typeOfValidator(
  const IssueSource source,
  const std::string_view id,
  const Severity severity,
  const IssueScope scope)
{
  return {id, severity, source, std::type_index{typeid(T)}, scope};
}

template <typename T>
IssueTypeDef validatorType(
  const std::string_view id, const Severity severity, std::vector<QuickFixDef> fixes = {})
{
  auto result =
    typeOfValidator<T>(IssueSource::Validator, id, severity, IssueScope::Node);
  result.fixes = std::move(fixes);
  return result;
}

template <typename T>
IssueTypeDef cliCheckType(
  const std::string_view id,
  const Severity severity,
  const IssueScope scope = IssueScope::Node)
{
  auto result = typeOfValidator<T>(IssueSource::CliCheck, id, severity, scope);
  result.createValidator = []() -> std::unique_ptr<mdl::Validator> {
    return std::make_unique<T>();
  };
  return result;
}

} // namespace

std::string_view toString(const Severity severity)
{
  switch (severity)
  {
  case Severity::Info:
    return "info";
  case Severity::Warning:
    return "warning";
  case Severity::Error:
    return "error";
  }
  return "error";
}

std::optional<Severity> parseSeverity(const std::string_view name)
{
  for (const auto severity : {Severity::Info, Severity::Warning, Severity::Error})
  {
    if (toString(severity) == name)
    {
      return severity;
    }
  }
  return std::nullopt;
}

const std::vector<IssueTypeDef>& issueTypes()
{
  using enum Severity;
  static const auto types = std::vector<IssueTypeDef>{
    {"load_error", Error, IssueSource::LoadDiagnostics},
    validatorType<mdl::MissingClassnameValidator>(
      "missing_classname", Error, {DeleteObjects}),
    validatorType<mdl::MissingDefinitionValidator>(
      "missing_definition", Warning, {DeleteObjects}),
    validatorType<mdl::MissingModValidator>(
      "missing_mod", Warning, {{"remove_mod", "Remove Mod"}}),
    validatorType<mdl::EmptyGroupValidator>("empty_group", Warning, {DeleteObjects}),
    validatorType<mdl::EmptyBrushEntityValidator>(
      "empty_brush_entity", Warning, {DeleteObjects}),
    validatorType<mdl::PointEntityWithBrushesValidator>(
      "point_entity_with_brushes",
      Warning,
      {{"move_brushes_to_world", "Move Brushes to World"}}),
    validatorType<mdl::LinkSourceValidator>("link_source", Warning, {DeleteProperty}),
    validatorType<mdl::LinkTargetValidator>("link_target", Warning, {DeleteProperty}),
    validatorType<mdl::NonIntegerVerticesValidator>(
      "non_integer_vertices", Info, {{"snap_vertices", "Snap Vertices"}}),
    validatorType<mdl::MixedBrushContentsValidator>("mixed_brush_contents", Warning),
    validatorType<mdl::WorldBoundsValidator>("world_bounds", Error, {DeleteObjects}),
    validatorType<mdl::SoftMapBoundsValidator>("soft_map_bounds", Info, {DeleteObjects}),
    validatorType<mdl::EmptyPropertyKeyValidator>(
      "empty_property_key", Warning, {DeleteProperty}),
    validatorType<mdl::EmptyPropertyValueValidator>(
      "empty_property_value", Warning, {DeleteProperty}),
    validatorType<mdl::LongPropertyKeyValidator>(
      "long_property_key", Warning, {DeleteProperty}),
    validatorType<mdl::LongPropertyValueValidator>(
      "long_property_value",
      Warning,
      {DeleteProperty, {"truncate_property_value", "Truncate Property Values"}}),
    validatorType<mdl::PropertyKeyWithDoubleQuotationMarksValidator>(
      "property_key_with_double_quotation_marks",
      Warning,
      {DeleteProperty, ReplaceDoubleQuotes}),
    validatorType<mdl::PropertyValueWithDoubleQuotationMarksValidator>(
      "property_value_with_double_quotation_marks",
      Warning,
      {DeleteProperty, ReplaceDoubleQuotes}),
    validatorType<mdl::WorldNodePathSeparatorValidator>(
      "world_node_path_separator",
      Warning,
      {{"replace_backslashes", "Replace \\ with /"}}),
    validatorType<mdl::InvalidUvScaleValidator>(
      "invalid_uv_scale", Warning, {{"reset_uv_scale", "Reset UV Scale"}}),
    cliCheckType<MissingMaterialValidator>("missing_material", Warning),
    cliCheckType<FgdPropertyTypeValidator>("fgd_property_type", Warning),
    cliCheckType<EntityInsideSolidValidator>("entity_inside_solid", Error),
    cliCheckType<NoPlayerStartValidator>("no_player_start", Error, IssueScope::Map),
    cliCheckType<DegenerateBrushValidator>("degenerate_brush", Warning),
    {"leak", Error, IssueSource::Compiler},
    {.id = "compiler",
     .severity = Warning,
     .source = IssueSource::Compiler,
     .severityPerIssue = true},
    {"compile_fatal", Error, IssueSource::Compiler},
  };
  return types;
}

const IssueTypeDef* findIssueType(const std::string_view id)
{
  const auto& types = issueTypes();
  const auto it = std::ranges::find(types, id, &IssueTypeDef::id);
  return it != types.end() ? &*it : nullptr;
}

const IssueTypeDef* findIssueType(const mdl::Validator& validator)
{
  const auto& types = issueTypes();
  const auto it = std::ranges::find(
    types, std::optional{std::type_index{typeid(validator)}}, &IssueTypeDef::validator);
  return it != types.end() ? &*it : nullptr;
}

const QuickFixDef* findQuickFix(const IssueTypeDef& type, const std::string_view id)
{
  const auto it = std::ranges::find(type.fixes, id, &QuickFixDef::id);
  return it != type.fixes.end() ? &*it : nullptr;
}

Json fixIdsJson(const IssueTypeDef& type)
{
  auto result = Json::array();
  for (const auto& fix : type.fixes)
  {
    result.push(fix.id);
  }
  return result;
}

Json issueTypesJson()
{
  auto result = Json::array();
  for (const auto& type : issueTypes())
  {
    auto entry = Json::object();
    entry.set("type", type.id);
    entry.set(
      "severity", type.severityPerIssue ? Json{nullptr} : Json{toString(type.severity)});
    entry.set("fixes", fixIdsJson(type));
    result.push(std::move(entry));
  }
  return result;
}

} // namespace tb::cli
