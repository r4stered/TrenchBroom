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

#include "cli/Fgd.h"

#include "cli/CliError.h"
#include "cli/GeometryJson.h"
#include "cli/Json.h"
#include "cli/MapRef.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityProperties.h"
#include "mdl/Map.h"
#include "mdl/PropertyDefinition.h"

#include "kd/optional_utils.h"
#include "kd/overload.h"

#include <fmt/format.h>

#include <string>
#include <variant>

namespace tb::cli
{
namespace
{

namespace PVT = mdl::PropertyValueTypes;

/** The type name an FGD uses for the given property value type. */
std::string_view typeName(const mdl::PropertyValueType& valueType)
{
  return std::visit(
    kdl::overload(
      [](const PVT::LinkTarget&) { return std::string_view{"target_source"}; },
      [](const PVT::LinkSource&) { return std::string_view{"target_destination"}; },
      [](const PVT::String&) { return std::string_view{"string"}; },
      [](const PVT::Boolean&) { return std::string_view{"boolean"}; },
      [](const PVT::Integer&) { return std::string_view{"integer"}; },
      [](const PVT::Float&) { return std::string_view{"float"}; },
      [](const PVT::Choice&) { return std::string_view{"choices"}; },
      [](const PVT::Flags&) { return std::string_view{"flags"}; },
      [](const PVT::Origin&) { return std::string_view{"origin"}; },
      [](const PVT::Input&) { return std::string_view{"input"}; },
      [](const PVT::Output&) { return std::string_view{"output"}; },
      [](const PVT::Color<RgbF>&) { return std::string_view{"color1"}; },
      [](const PVT::Color<RgbB>&) { return std::string_view{"color255"}; },
      [](const PVT::Color<Rgb>&) { return std::string_view{"color"}; },
      [](const PVT::Unknown&) { return std::string_view{"unknown"}; }),
    valueType);
}

/** The default value as a string, with floats in their shortest form ("1", not
 * "1.000000"). */
std::optional<std::string> defaultValue(const mdl::PropertyDefinition& definition)
{
  if (const auto* floatType = std::get_if<PVT::Float>(&definition.valueType))
  {
    return floatType->defaultValue | kdl::optional_transform([](const auto value) {
             return fmt::format("{}", value);
           });
  }
  return mdl::PropertyDefinition::defaultValue(definition);
}

Json propertyToJson(const mdl::PropertyDefinition& definition)
{
  auto result = Json::object();
  result.set("key", definition.key);
  result.set("type", typeName(definition.valueType));
  result.set("description", definition.shortDescription);
  if (!definition.longDescription.empty())
  {
    result.set("long_description", definition.longDescription);
  }
  if (const auto value = defaultValue(definition))
  {
    result.set("default", *value);
  }
  if (const auto* choice = std::get_if<PVT::Choice>(&definition.valueType))
  {
    auto choices = Json::array();
    for (const auto& option : choice->options)
    {
      auto entry = Json::object();
      entry.set("value", option.value);
      entry.set("description", option.description);
      choices.push(std::move(entry));
    }
    result.set("choices", std::move(choices));
  }
  if (definition.readOnly)
  {
    result.set("read_only", true);
  }
  return result;
}

Json spawnflagsToJson(const PVT::Flags& flags)
{
  auto result = Json::array();
  for (const auto& flag : flags.flags)
  {
    auto entry = Json::object();
    entry.set("value", flag.value);
    entry.set("name", flag.shortDescription);
    if (!flag.longDescription.empty())
    {
      entry.set("description", flag.longDescription);
    }
    entry.set("default", flags.isDefault(flag.value));
    result.push(std::move(entry));
  }
  return result;
}

} // namespace

Json fgd(const MapRef& mapRef, const std::string_view classname)
{
  const auto& definitions = mapRef.map.entityDefinitionManager();
  if (definitions.definitions().empty())
  {
    throw CliError{
      ErrorCode::UndefinedClass,
      fmt::format(
        "no entity definitions are loaded, so '{}' is undefined; configure an FGD with "
        "\"fgd\" in tbcli.json or --fgd",
        classname)};
  }

  const auto* definition = definitions.definition(classname);
  if (!definition)
  {
    throw CliError{
      ErrorCode::UndefinedClass,
      fmt::format("the entity definitions do not define the class '{}'", classname)};
  }

  auto properties = Json::array();
  auto spawnflags = Json::array();
  for (const auto& property : definition->propertyDefinitions)
  {
    const auto* flags = std::get_if<PVT::Flags>(&property.valueType);
    if (flags && property.key == mdl::EntityPropertyKeys::Spawnflags)
    {
      spawnflags = spawnflagsToJson(*flags);
    }
    else
    {
      properties.push(propertyToJson(property));
    }
  }

  auto result = Json::object();
  result.set("schema", "tbcli.fgd/1");
  result.set("rev", mapRef.rev);
  result.set("classname", definition->name);
  result.set(
    "type",
    mdl::getType(*definition) == mdl::EntityDefinitionType::Point ? "point" : "brush");
  result.set("description", definition->description);
  if (const auto* point = mdl::getPointEntityDefinition(definition))
  {
    result.set("size", toJson(point->bounds));
  }
  result.set("properties", std::move(properties));
  result.set("spawnflags", std::move(spawnflags));
  return result;
}

} // namespace tb::cli
