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
#include <typeinfo>

namespace tb::cli
{
namespace
{

template <typename T>
IssueTypeDef validatorType(const std::string_view id, const Severity severity)
{
  return {id, severity, IssueSource::Validator, std::type_index{typeid(T)}};
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
    validatorType<mdl::MissingClassnameValidator>("missing_classname", Error),
    validatorType<mdl::MissingDefinitionValidator>("missing_definition", Warning),
    validatorType<mdl::MissingModValidator>("missing_mod", Warning),
    validatorType<mdl::EmptyGroupValidator>("empty_group", Warning),
    validatorType<mdl::EmptyBrushEntityValidator>("empty_brush_entity", Warning),
    validatorType<mdl::PointEntityWithBrushesValidator>(
      "point_entity_with_brushes", Warning),
    validatorType<mdl::LinkSourceValidator>("link_source", Warning),
    validatorType<mdl::LinkTargetValidator>("link_target", Warning),
    validatorType<mdl::NonIntegerVerticesValidator>("non_integer_vertices", Info),
    validatorType<mdl::MixedBrushContentsValidator>("mixed_brush_contents", Warning),
    validatorType<mdl::WorldBoundsValidator>("world_bounds", Error),
    validatorType<mdl::SoftMapBoundsValidator>("soft_map_bounds", Info),
    validatorType<mdl::EmptyPropertyKeyValidator>("empty_property_key", Warning),
    validatorType<mdl::EmptyPropertyValueValidator>("empty_property_value", Warning),
    validatorType<mdl::LongPropertyKeyValidator>("long_property_key", Warning),
    validatorType<mdl::LongPropertyValueValidator>("long_property_value", Warning),
    validatorType<mdl::PropertyKeyWithDoubleQuotationMarksValidator>(
      "property_key_with_double_quotation_marks", Warning),
    validatorType<mdl::PropertyValueWithDoubleQuotationMarksValidator>(
      "property_value_with_double_quotation_marks", Warning),
    validatorType<mdl::WorldNodePathSeparatorValidator>(
      "world_node_path_separator", Warning),
    validatorType<mdl::InvalidUvScaleValidator>("invalid_uv_scale", Warning),
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

Json fixIdsJson(const IssueTypeDef&)
{
  // quick-fix ids come with the fix op
  return Json::array();
}

Json issueTypesJson()
{
  auto result = Json::array();
  for (const auto& type : issueTypes())
  {
    auto entry = Json::object();
    entry.set("type", type.id);
    entry.set("severity", toString(type.severity));
    entry.set("fixes", fixIdsJson(type));
    result.push(std::move(entry));
  }
  return result;
}

} // namespace tb::cli
