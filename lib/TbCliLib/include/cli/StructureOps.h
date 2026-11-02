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

#include <string_view>

namespace tb::cli
{

class Json;
struct OpContext;

/**
 * The implementations of the ops that organise a map: duplicating, grouping, layers,
 * moving brushes between entities, locking, linked groups and importing fragments. Each
 * takes the arguments its definition in Ops.cpp declares, already validated, and throws
 * CliError if it fails.
 */

/**
 * `duplicate {target, offset}`: copies groups, entities, brushes and patches next to
 * them and moves the copies by the offset. A brush of a brush entity is copied with a
 * new entity around it, as in the editor. The copies are created; nodes inside another
 * target are copied with it.
 */
void duplicate(OpContext& context);

/**
 * `group {target, name}`: creates a group with the name in the group or layer of the
 * first target and moves the targets into it. A brush of a brush entity brings its
 * entity along, as in the editor. The group is created and the moved nodes are
 * modified.
 */
void group(OpContext& context);

/**
 * `ungroup {target}`: moves the contents of groups into the groups' parents and removes
 * the groups. A linked group is separated from its link set first. The groups are
 * removed and their former contents are modified.
 */
void ungroup(OpContext& context);

/** `rename_group {target, name}`: renames groups. */
void renameGroup(OpContext& context);

/**
 * `create_layer {name}`: creates a layer below the other layers. The layer is created.
 */
void createLayer(OpContext& context);

/**
 * `rename_layer {target, name}`: renames a custom layer.
 *
 * @throw CliError `op_failed` if the target is the default layer, whose name is not
 * saved
 */
void renameLayer(OpContext& context);

/**
 * `move_to_layer {target, layer}`: moves groups, entities, brushes and patches to the
 * layer. A brush of a brush entity moves with its entity, as in the editor. The moved
 * nodes are modified.
 *
 * @throw CliError `op_failed` if a target is inside a group, since only whole groups
 * can change layers
 */
void moveToLayer(OpContext& context);

/**
 * `set_layer_omit_from_export {target, omit}`: sets whether layers are left out when
 * the map is exported. Layers whose setting changes are modified.
 */
void setLayerOmitFromExport(OpContext& context);

/**
 * `move_to_entity {target, entity}`: moves brushes and patches into a brush entity. The
 * moved nodes and the entities that lost some of them are modified; an entity left
 * without brushes is removed.
 */
void moveToEntity(OpContext& context);

/**
 * `lock {target}`: locks layers, so that ops refuse to edit what is in them unless
 * --allow-locked is given. Layers that were not locked are modified.
 */
void lock(OpContext& context);

/**
 * `linked_duplicate {target, offset}`: creates a linked copy of each group, moved by the
 * offset. Edits inside one linked group reach its linked copies. The copies are
 * created.
 */
void linkedDuplicate(OpContext& context);

/**
 * `separate_linked {target}`: separates linked groups from the other groups of their
 * link set. Groups of the same link set that are separated together stay linked to each
 * other. The separated groups are modified.
 *
 * @throw CliError `op_failed` if a target is not linked to a group outside the targets
 */
void separateLinked(OpContext& context);

/**
 * `import {file | text, offset, parent}`: pastes the groups, entities, brushes and
 * patches of a Valve 220 map fragment into a layer or group, as the editor pastes, and
 * moves them by the offset. The worldspawn's brushes are pasted without the worldspawn
 * and layers without the layer. Brushes in the Standard format are converted to Valve
 * 220, as the editor does. The pasted top-level nodes are created.
 *
 * @throw CliError `op_failed` if the file cannot be read, or the fragment holds no
 * nodes or does not parse exactly, with the line of the first problem
 */
void importFragment(OpContext& context);

/**
 * Checks the arguments of `import`.
 *
 * @throw CliError `invalid_argument` unless exactly one of file and text is given
 */
void checkImportArgs(const Json& args);

/**
 * Checks the arguments of the ops that take a name: group, rename_group, create_layer
 * and rename_layer.
 *
 * @throw CliError `invalid_argument` if the name is empty
 */
void checkNameArg(std::string_view opName, const Json& args);

} // namespace tb::cli
