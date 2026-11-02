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

#include "cli/EntityJson.h"

#include "cli/GeometryJson.h"
#include "cli/Json.h"
#include "cli/NodePath.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

#include <charconv>
#include <optional>
#include <variant>

namespace tb::cli
{
namespace
{

std::optional<int> parseInt(const std::string& str)
{
  auto value = 0;
  const auto* end = str.data() + str.size();
  const auto [ptr, ec] = std::from_chars(str.data(), end, value);
  return ec == std::errc{} && ptr == end ? std::optional{value} : std::nullopt;
}

const mdl::PropertyValueTypes::Flags* flagsDefinition(
  const mdl::EntityDefinition* definition)
{
  const auto* propertyDefinition =
    mdl::getPropertyDefinition(definition, mdl::EntityPropertyKeys::Spawnflags);
  return propertyDefinition
           ? std::get_if<mdl::PropertyValueTypes::Flags>(&propertyDefinition->valueType)
           : nullptr;
}

/** Each set bit by its name in the definition, or as its value if it has none. */
Json decodeSpawnflags(const int spawnflags, const mdl::EntityDefinition* definition)
{
  const auto* flags = flagsDefinition(definition);

  auto result = Json::array();
  for (auto bit = 0; bit < 31; ++bit)
  {
    const auto value = 1 << bit;
    if ((spawnflags & value) == 0)
    {
      continue;
    }

    const auto* flag = flags ? flags->flag(value) : nullptr;
    if (flag && !flag->shortDescription.empty())
    {
      result.push(flag->shortDescription);
    }
    else
    {
      result.push(value);
    }
  }
  return result;
}

} // namespace

Json entityToJson(const mdl::EntityNodeBase& node)
{
  const auto& entity = node.entity();

  auto result = Json::object();
  result.set("path", nodePath(node));
  result.set("classname", entity.classname());

  node.accept(
    kdl::overload(
      [](const mdl::WorldNode&) {},
      [](const mdl::LayerNode&) {},
      [](const mdl::GroupNode&) {},
      [&](const mdl::EntityNode& entityNode) {
        if (entityNode.hasChildren())
        {
          result.set("aabb", toJson(entityNode.logicalBounds()));
        }
        else
        {
          result.set("origin", toJson(entity.origin()));
        }
      },
      [](const mdl::BrushNode&) {},
      [](const mdl::PatchNode&) {}));

  auto properties = Json::object();
  for (const auto& property : entity.properties())
  {
    properties.set(property.key(), property.value());
  }
  result.set("properties", std::move(properties));

  result.set("definition", entity.definition() ? "found" : "missing");

  if (const auto* spawnflags = entity.property(mdl::EntityPropertyKeys::Spawnflags))
  {
    if (const auto value = parseInt(*spawnflags); value && *value > 0)
    {
      result.set("spawnflags", decodeSpawnflags(*value, entity.definition()));
    }
  }

  return result;
}

} // namespace tb::cli
