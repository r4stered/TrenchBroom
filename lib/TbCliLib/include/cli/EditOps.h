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

namespace tb::cli
{

class Json;
struct OpContext;

/**
 * The implementations of the ops that Ops.cpp defines. Each takes the arguments its
 * definition declares, already validated, and throws CliError if it fails.
 */

/**
 * `create_box {min, max, material, parent}`: an axis-aligned box brush in a layer,
 * group or brush entity. Warns `missing_material` if the map's materials can be checked
 * and the material is not among them.
 */
void createBox(OpContext& context);

/**
 * `create_point_entity {classname, origin, properties, allow_undefined, parent}`: a point
 * entity in a layer or group.
 *
 * @throw CliError `undefined_class` if the FGD does not define the classname as a point
 * class and undefined classes are not allowed
 */
void createPointEntity(OpContext& context);

/**
 * Checks the arguments of `create_point_entity`.
 *
 * @throw CliError `invalid_argument` if the classname is empty or worldspawn, or a
 * property key is empty, classname or origin
 */
void checkCreatePointEntityArgs(const Json& args);

/**
 * `create_brush_entity {classname, brushes, properties, allow_undefined}`: a brush
 * entity in the group or layer of the first brush, with the brushes moved into it. The
 * properties of the entities the brushes leave are not carried over. Those entities are
 * reported as modified, or as removed if they are left without brushes.
 *
 * @throw CliError `undefined_class` if the FGD does not define the classname as a brush
 * class and undefined classes are not allowed
 */
void createBrushEntity(OpContext& context);

/**
 * Checks the arguments of `create_brush_entity`.
 *
 * @throw CliError `invalid_argument` if the classname is empty or worldspawn, or a
 * property key is empty or classname
 */
void checkCreateBrushEntityArgs(const Json& args);

/**
 * `move_to_world {target}`: moves the brushes of brush entities, or the given brushes of
 * brush entities, into the group or layer that holds their entity. The entities are
 * reported as modified, or as removed if they are left without brushes.
 */
void moveToWorld(OpContext& context);

/**
 * `set_property {target, key, value}`: sets a property of entities and the world.
 * Entities that have the value already are not modified.
 */
void setProperty(OpContext& context);

/**
 * Checks the arguments of `set_property`.
 *
 * @throw CliError `invalid_argument` if the key is empty or classname, the key is origin
 * and the value is not three numbers, or the key is angle and the value is not a number
 */
void checkSetPropertyArgs(const Json& args);

/**
 * `remove_property {target, key}`: removes a property from entities and the world. If a
 * target does not have the property, the op warns `property_not_found`.
 */
void removeProperty(OpContext& context);

/**
 * Checks the arguments of `remove_property`.
 *
 * @throw CliError `invalid_argument` if the key is empty or classname
 */
void checkRemovePropertyArgs(const Json& args);

/**
 * `rename_property {target, from, to}`: renames a property of entities and the world.
 * If a target does not have the property, the op warns `property_not_found`.
 *
 * @throw CliError `op_failed` if a target has a property with the new key already
 */
void renameProperty(OpContext& context);

/**
 * Checks the arguments of `rename_property`.
 *
 * @throw CliError `invalid_argument` if a key is empty or classname, or both keys are
 * the same
 */
void checkRenamePropertyArgs(const Json& args);

/**
 * `set_spawnflag {target, flag, on}`: sets or clears a spawnflag of entities, given by
 * its value or by its name in each entity's definition.
 *
 * @throw CliError `op_failed` if a target's definition has no spawnflag with the given
 * name
 */
void setSpawnflag(OpContext& context);

/**
 * `delete {target}`: removes groups, entities, brushes and patches. An entity or group
 * left empty is removed with them and is reported as removed after the targets.
 */
void deleteNode(OpContext& context);

/**
 * `fix {issue_type, fix, target?}`: applies the quick-fix of the map library's validator
 * to the issues of the type on the target and the nodes inside it, or to every issue of
 * the type in the map. Issues on locked nodes are left out unless locked nodes may be
 * edited; suppressions do not apply. Fixing nothing is not an error; the op then warns
 * `nothing_to_fix`.
 */
void fixIssues(OpContext& context);

/**
 * Checks the arguments of `fix`.
 *
 * @throw CliError `invalid_argument` if the issue type is unknown, or has no quick-fix
 * with the given id
 */
void checkFixArgs(const Json& args);

} // namespace tb::cli
