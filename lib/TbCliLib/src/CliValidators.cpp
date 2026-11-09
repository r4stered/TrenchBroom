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

#include "cli/CliValidators.h"

#include "cli/IssueTypes.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityNodeBase.h"
#include "mdl/EntityProperties.h"
#include "mdl/GroupNode.h"
#include "mdl/Issue.h"
#include "mdl/IssueType.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/NodeTree.h"
#include "mdl/PatchNode.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"
#include "kd/ranges/contains.h"

#include "vm/bbox.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <variant>
#include <vector>

namespace tb::cli
{
namespace
{

const auto MissingMaterialType = mdl::freeIssueType();
const auto FgdPropertyIssueType = mdl::freeIssueType();
const auto EntityInsideSolidType = mdl::freeIssueType();
const auto NoPlayerStartType = mdl::freeIssueType();
const auto DegenerateBrushType = mdl::freeIssueType();

/** The classes of the entities that a player can spawn at. */
constexpr auto PlayerStartClassnames = std::array<std::string_view, 3>{
  "info_player_start",
  "info_player_deathmatch",
  "info_player_coop",
};

/**
 * The spawnflags that the Quake engine itself reads, which FGDs often leave out: not on
 * easy, normal or hard, and not in deathmatch.
 */
constexpr auto EngineSpawnflags = 256 | 512 | 1024 | 2048;

/** The integer that the whole string spells, if it does. */
std::optional<int> parseInt(const std::string_view str)
{
  auto value = 0;
  const auto* end = str.data() + str.size();
  const auto [ptr, ec] = std::from_chars(str.data(), end, value);
  return ec == std::errc{} && ptr == end ? std::optional{value} : std::nullopt;
}

/** Whether the value is one of the choices, as a string or as the same integer. */
bool isChoice(
  const std::string& value,
  const std::vector<mdl::PropertyValueTypes::ChoiceOption>& options)
{
  const auto intValue = parseInt(value);
  return std::ranges::any_of(options, [&](const auto& option) {
    return option.value == value || (intValue && parseInt(option.value) == intValue);
  });
}

/** Why the value of the property with the given key violates its type, if it does. */
std::optional<std::string> typeViolation(
  const mdl::PropertyValueType& valueType,
  const std::string& key,
  const std::string& value)
{
  using namespace mdl::PropertyValueTypes;

  return std::visit(
    kdl::overload(
      [&](const Choice& choice) -> std::optional<std::string> {
        if (isChoice(value, choice.options))
        {
          return std::nullopt;
        }
        auto choices = std::vector<std::string>{};
        for (const auto& option : choice.options)
        {
          choices.push_back(option.value);
        }
        return fmt::format("is not one of the choices {}", fmt::join(choices, ", "));
      },
      [&](const Integer&) -> std::optional<std::string> {
        return parseInt(value) ? std::nullopt : std::optional{"is not an integer"};
      },
      [&](const Flags& flags) -> std::optional<std::string> {
        const auto intValue = parseInt(value);
        if (!intValue)
        {
          return "is not an integer";
        }
        auto defined = key == mdl::EntityPropertyKeys::Spawnflags ? EngineSpawnflags : 0;
        for (const auto& flag : flags.flags)
        {
          defined |= flag.value;
        }
        if (const auto undefined = *intValue & ~defined; undefined != 0)
        {
          return fmt::format("sets flags without a definition ({})", undefined);
        }
        return std::nullopt;
      },
      [](const auto&) -> std::optional<std::string> { return std::nullopt; }),
    valueType);
}

/** Whether the brush is a liquid, which is not solid: one of its materials is `*...`. */
bool isLiquid(const mdl::Brush& brush)
{
  return std::ranges::any_of(
    brush.faces(), [](const auto& face) { return face.materialName().starts_with('*'); });
}

// node trees

/** The world that the node belongs to. */
const mdl::WorldNode& worldOf(const mdl::Node& node)
{
  const auto* root = &node;
  while (root->parent())
  {
    root = root->parent();
  }
  return static_cast<const mdl::WorldNode&>(*root);
}

/**
 * Whether a solid world brush strictly contains the point. Looks the point up in the
 * world's node tree: checking every brush for every point entity took most of the time
 * of checking a large map.
 */
bool containsSolidWorldBrushAt(const mdl::WorldNode& worldNode, const vm::vec3d& point)
{
  const auto candidates = worldNode.nodeTree().find_containers(point);
  return std::ranges::any_of(candidates, [&](const mdl::Node* node) {
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node);
    // the brushes of an entity are not world brushes
    if (!brushNode || brushNode->entity() != &worldNode)
    {
      return false;
    }
    const auto& brush = brushNode->brush();
    return brushNode->logicalBounds().contains(point) && !isLiquid(brush)
           && brush.containsPointStrictly(point);
  });
}

/** Whether a player start entity is below the node. */
bool containsPlayerStart(const mdl::Node& node)
{
  return std::ranges::any_of(node.children(), [](const mdl::Node* child) {
    return child->accept(
      kdl::overload(
        [](const mdl::WorldNode&) { return false; },
        [](const mdl::LayerNode& layer) { return containsPlayerStart(layer); },
        [](const mdl::GroupNode& group) { return containsPlayerStart(group); },
        [](const mdl::EntityNode& entityNode) {
          return kdl::ranges::contains(
            PlayerStartClassnames, entityNode.entity().classname());
        },
        [](const mdl::BrushNode&) { return false; },
        [](const mdl::PatchNode&) { return false; }));
  });
}

} // namespace

MissingMaterialValidator::MissingMaterialValidator()
  : Validator{MissingMaterialType, "Missing material"}
{
}

void MissingMaterialValidator::doValidate(
  mdl::BrushNode& brushNode, std::vector<std::unique_ptr<mdl::Issue>>& issues) const
{
  const auto& brush = brushNode.brush();
  for (size_t i = 0; i < brush.faceCount(); ++i)
  {
    const auto& face = brush.face(i);
    if (face.material() == nullptr)
    {
      issues.push_back(
        std::make_unique<mdl::BrushFaceIssue>(
          MissingMaterialType,
          brushNode,
          i,
          fmt::format("Material '{}' not found", face.materialName())));
    }
  }
}

FgdPropertyTypeValidator::FgdPropertyTypeValidator()
  : Validator{FgdPropertyIssueType, "Property value violates its entity definition"}
{
}

void FgdPropertyTypeValidator::doValidate(
  mdl::EntityNodeBase& entityNode, std::vector<std::unique_ptr<mdl::Issue>>& issues) const
{
  for (const auto& property : entityNode.entity().properties())
  {
    if (property.value().empty())
    {
      continue;
    }
    if (const auto* definition = mdl::propertyDefinition(&entityNode, property.key()))
    {
      if (
        const auto reason =
          typeViolation(definition->valueType, property.key(), property.value()))
      {
        issues.push_back(
          std::make_unique<mdl::EntityPropertyIssue>(
            FgdPropertyIssueType,
            entityNode,
            property.key(),
            fmt::format(
              "{} property '{}' value '{}' {}",
              entityNode.name(),
              property.key(),
              property.value(),
              *reason)));
      }
    }
  }
}

EntityInsideSolidValidator::EntityInsideSolidValidator()
  : Validator{EntityInsideSolidType, "Point entity inside a world brush"}
{
}

void EntityInsideSolidValidator::doValidate(
  mdl::EntityNode& entityNode, std::vector<std::unique_ptr<mdl::Issue>>& issues) const
{
  if (
    entityNode.entity().pointEntity()
    && containsSolidWorldBrushAt(worldOf(entityNode), entityNode.entity().origin()))
  {
    issues.push_back(
      std::make_unique<mdl::Issue>(
        EntityInsideSolidType,
        entityNode,
        fmt::format("{} origin is inside a world brush", entityNode.name())));
  }
}

NoPlayerStartValidator::NoPlayerStartValidator()
  : Validator{NoPlayerStartType, "No player start"}
{
}

void NoPlayerStartValidator::doValidate(
  mdl::WorldNode& worldNode, std::vector<std::unique_ptr<mdl::Issue>>& issues) const
{
  if (!containsPlayerStart(worldNode))
  {
    issues.push_back(
      std::make_unique<mdl::Issue>(
        NoPlayerStartType,
        worldNode,
        fmt::format(
          "Map has no player start ({})", fmt::join(PlayerStartClassnames, ", "))));
  }
}

DegenerateBrushValidator::DegenerateBrushValidator()
  : Validator{DegenerateBrushType, "Degenerate brush"}
{
}

void DegenerateBrushValidator::doValidate(
  mdl::BrushNode& brushNode, std::vector<std::unique_ptr<mdl::Issue>>& issues) const
{
  const auto& brush = brushNode.brush();
  if (const auto brushVolume = brush.volume(); brushVolume < MinVolume)
  {
    issues.push_back(
      std::make_unique<mdl::Issue>(
        DegenerateBrushType,
        brushNode,
        fmt::format("Brush volume {:.3g} is below {}", brushVolume, MinVolume)));
    return;
  }

  for (size_t i = 0; i < brush.faceCount(); ++i)
  {
    if (const auto area = brush.face(i).area(); area < MinFaceArea)
    {
      issues.push_back(
        std::make_unique<mdl::Issue>(
          DegenerateBrushType,
          brushNode,
          fmt::format("Face {} area {:.3g} is below {}", i, area, MinFaceArea)));
      return;
    }
  }
}

MaterialCheck materialCheckOf(const mdl::Map& map)
{
  return map.gamePath().empty() ? MaterialCheck::Off : MaterialCheck::On;
}

bool isMaterialMissing(const mdl::Map& map, const mdl::BrushFace& face)
{
  return materialCheckOf(map) == MaterialCheck::On && face.material() == nullptr;
}

std::vector<std::unique_ptr<mdl::Validator>> makeCliValidators(
  const MaterialCheck materialCheck)
{
  const auto missingMaterial = std::type_index{typeid(MissingMaterialValidator)};

  auto result = std::vector<std::unique_ptr<mdl::Validator>>{};
  for (const auto& type : issueTypes())
  {
    if (
      type.source == IssueSource::CliCheck
      && (materialCheck == MaterialCheck::On || type.validator != missingMaterial))
    {
      result.push_back(type.createValidator());
    }
  }
  return result;
}

} // namespace tb::cli
