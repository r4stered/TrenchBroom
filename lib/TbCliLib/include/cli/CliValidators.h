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

#include "mdl/Validator.h"

#include <memory>
#include <vector>

namespace tb::mdl
{
class Map;
}

/*
 * The validators of verify's CLI checks: mistakes that agents commonly make and that the
 * editor's validators do not catch. They depend only on the map library, so that they
 * could move there.
 */
namespace tb::cli
{

/** Reports each face whose material was not found. */
class MissingMaterialValidator : public mdl::Validator
{
public:
  MissingMaterialValidator();

private:
  void doValidate(
    mdl::BrushNode& brushNode,
    std::vector<std::unique_ptr<mdl::Issue>>& issues) const override;
};

/**
 * Reports each property value that violates its entity definition: a value that is not
 * one of the choices, a value that is not an integer, or flags that are not an integer
 * or that set a bit without a flag. The bits of the Quake engine's skill and deathmatch
 * spawnflags count as flags. Empty values are left to the empty property value
 * validator.
 */
class FgdPropertyTypeValidator : public mdl::Validator
{
public:
  FgdPropertyTypeValidator();

private:
  void doValidate(
    mdl::EntityNodeBase& entityNode,
    std::vector<std::unique_ptr<mdl::Issue>>& issues) const override;
};

/**
 * Reports each point entity whose origin is strictly inside a world brush, that is, a
 * brush that does not belong to an entity. Liquid brushes, which have a face with a
 * material whose name starts with `*`, are not solid.
 */
class EntityInsideSolidValidator : public mdl::Validator
{
public:
  EntityInsideSolidValidator();

private:
  void doValidate(
    mdl::EntityNode& entityNode,
    std::vector<std::unique_ptr<mdl::Issue>>& issues) const override;
};

/**
 * Reports, on the world, a map without a player start: an `info_player_start`,
 * `info_player_deathmatch` or `info_player_coop` entity.
 */
class NoPlayerStartValidator : public mdl::Validator
{
public:
  NoPlayerStartValidator();

private:
  void doValidate(
    mdl::WorldNode& worldNode,
    std::vector<std::unique_ptr<mdl::Issue>>& issues) const override;
};

/**
 * Reports each brush whose volume is below `MinVolume` or that has a face whose area is
 * below `MinFaceArea`.
 */
class DegenerateBrushValidator : public mdl::Validator
{
public:
  /** In cubic units: less than a 1 unit cube, the smallest brush on the default grid. */
  static constexpr auto MinVolume = 1.0;
  /** In square units. */
  static constexpr auto MinFaceArea = 0.1;

  DegenerateBrushValidator();

private:
  void doValidate(
    mdl::BrushNode& brushNode,
    std::vector<std::unique_ptr<mdl::Issue>>& issues) const override;
};

/** Whether to check for missing materials. */
enum class MaterialCheck
{
  Off,
  On,
};

/**
 * Whether the map's materials can be checked: only if they could be loaded, which needs
 * a game path.
 */
MaterialCheck materialCheckOf(const mdl::Map& map);

/**
 * The validators of the CLI check types of the issue-type table, in its order. Missing
 * materials can only be told apart from found ones if materials could be loaded, so
 * the missing material validator is left out unless the material check is on.
 */
std::vector<std::unique_ptr<mdl::Validator>> makeCliValidators(
  MaterialCheck materialCheck);

} // namespace tb::cli
