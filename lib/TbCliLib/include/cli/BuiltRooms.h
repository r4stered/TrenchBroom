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

#include "vm/vec.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mdl
{
class EntityNode;
class GroupNode;
class Node;
class WorldNode;
} // namespace tb::mdl

namespace tb::cli
{

class Json;
struct OpContext;

/**
 * Built rooms: sealed rectangular rooms that `add_room` lays out by relation, and
 * connections: straight corridors that `connect` builds between two of them. Each is
 * ordinary brushes in a carrier, a plain `func_group` brush entity whose `_tbcli_*` keys
 * hold its parameters, wrapped in a group `built_room:<name>` or `connection:<A>-<B>`.
 * The carrier identifies the room or connection; the group is optional. Positions are
 * not stored, they are where the carriers' brushes are.
 */

/** The side of another room that a room is placed on. */
enum class Relation
{
  EastOf,
  WestOf,
  NorthOf,
  SouthOf,
};

/** The names of the relations: `east_of`, `west_of`, `north_of` and `south_of`. */
std::span<const std::string_view> relationNames();

/**
 * Whether the JSON is a placement `{<relation>: <room name>, "gap": <n>}` with one of
 * relationNames().
 */
bool isPlacement(const Json& value);

/** A room's placement next to another room, as intent. */
struct Placement
{
  Relation relation = Relation::EastOf;
  std::string room;
  /** The distance between the two rooms' interiors. */
  double gap = 0.0;
};

/** The parameters of a built room, as its carrier stores them. */
struct RoomParams
{
  /** The size of the interior. */
  vm::vec3d size;
  /** The thickness of the walls, floor and ceiling. */
  double wall = 0.0;
  /** Unset if the room was placed at absolute coordinates. */
  std::optional<Placement> place;
};

/**
 * Whether the room's brushes still match what its parameters build, normalised for
 * translation and compared on geometry only: they must have the fingerprint that tbcli
 * stored in `_tbcli_fp` when it last built or cut them, or that of what the parameters
 * build where the room stands, with the openings of its connections cut out; and the
 * room must be as large as its size and walls make it. Moves and retexturing keep a
 * room rebuildable, also when a room it is joined to is moved alone or a connection is
 * detached; vertex edits, CSG, clipping, adding or removing a brush, or editing the size
 * or wall key by hand make it hand_edited.
 */
enum class RoomStatus
{
  Rebuildable,
  HandEdited,
};

/** A built room as the map holds it. */
struct BuiltRoom
{
  const mdl::EntityNode* carrier = nullptr;
  std::string name;
  /** Unset if the carrier's parameter keys are missing or malformed. */
  std::optional<RoomParams> params;
  RoomStatus status = RoomStatus::HandEdited;
  /** The group `built_room:<name>` that wraps the carrier, if it is still there. */
  const mdl::GroupNode* group = nullptr;
};

/** `{size, wall, place?}`, as describe and show write a room's parameters. */
Json paramsToJson(const RoomParams& params);

/** `rebuildable` or `hand_edited`. */
std::string_view toString(RoomStatus status);

/** The built rooms of the map, in file order, with their statuses. */
std::vector<BuiltRoom> collectBuiltRooms(const mdl::WorldNode& worldNode);

/**
 * The room among the given ones whose carrier, or the group that wraps it, the node is,
 * if there is one.
 */
const BuiltRoom* builtRoomOf(const mdl::Node& node, const std::vector<BuiltRoom>& rooms);

/**
 * `{name, kind, params, status, instances, group}`: `params` is `{size, wall, place?}`,
 * or null if the carrier's keys are malformed; `group` is null if it has none.
 * `instances` lists the room's group, or its carrier if it has no group. It is a list
 * because a room inside linked groups has one instance per linked copy; until linked
 * built rooms are recognised as one room, it always has one entry.
 */
Json builtRoomToJson(const BuiltRoom& room);

/**
 * `add_room {name, size, min | place, wall, as?}`: a sealed rectangular room whose
 * interior has the given size, with walls, floor and ceiling `wall` thick around it,
 * placed at `min` or next to another built room. Relative placement puts it `gap` away
 * from the other room's interior on the given side, centred on the other horizontal
 * axis, with the same floor height. The room is created in the default layer.
 *
 * @throw CliError `layout_overlap` if its walls would overlap another built room's or
 * a connection's brushes, `node_not_found` if the room to place it against does not
 * exist, `ambiguous_room_name` if several rooms have its name, `locked` if the default
 * layer is locked, without --allow-locked, and `op_failed` if the room to place it
 * against has no brushes or malformed parameter keys, or a room with the new name
 * exists already
 */
void addRoom(OpContext& context);

/**
 * Checks the arguments of `add_room`.
 *
 * @throw CliError `invalid_argument` if the name is not made of letters, digits and
 * underscores, a size or the wall thickness is not greater than 0, or not exactly one of
 * min and place is given
 */
void checkAddRoomArgs(const Json& args);

/**
 * `connect {from, to, width, height, offset, as?}`: a straight corridor between two
 * built rooms that stand side by side, apart along x or y. Its interior is `width` wide
 * and `height` tall, stands on the rooms' floor, and is centred on the wall that they
 * share, moved by `offset` towards greater coordinates. Its walls, floor and ceiling are
 * as thick as the thicker of the rooms' walls. Both rooms' walls are cut and split
 * around the opening, keeping their materials, and their fingerprints are updated so
 * that they stay rebuildable. The corridor's carrier keeps the parameters in
 * `_tbcli_from`, `_tbcli_to`, `_tbcli_width`, `_tbcli_height` and `_tbcli_offset`, and
 * is wrapped in a group `connection:<from>-<to>` in the default layer.
 *
 * @throw CliError `layout_corridor_does_not_fit` if the rooms do not stand side by side,
 * their walls touch, or the corridor reaches beyond the wall that they share,
 * `layout_floor_mismatch` if their floors are at different heights,
 * `layout_corridor_too_tall` if the corridor would reach above a room's ceiling,
 * `layout_overlap` if it would overlap another built room or connection,
 * `node_not_found` if a room does not exist, `locked` if a room or the default layer is
 * locked, without --allow-locked, `connect_cuts_linked_room` if a room is inside a
 * linked group, `hand_edited` if a room's brushes no longer match what its parameters
 * build, `ambiguous_room_name` if several rooms have a name, and `op_failed` if a
 * connection joins the rooms already, or a room has no brushes or malformed parameter
 * keys
 */
void connect(OpContext& context);

/**
 * Checks the arguments of `connect`.
 *
 * @throw CliError `invalid_argument` if a room name is not made of letters, digits and
 * underscores, both name the same room, or the width or height is not greater than 0
 */
void checkConnectArgs(const Json& args);

/**
 * `edit_room {room, size?, wall?, place?, force?}`: changes a built room's parameters
 * and rebuilds it and everything that depends on it. The room keeps the minimum of its
 * interior, where its brushes are now, unless it is placed against another room: then
 * it is placed against that room again with its new size, keeping how far it was moved
 * along the other room's wall. `place` places it against another room anew. Rooms that
 * are placed against a room whose interior moves or changes size are placed against it
 * again, and so on; a room that no longer stands where its placement puts it, at the
 * gap on that side and on the same floor, stays where it is and is listed under
 * `stale_placements` with the warning `stale_placement`. Every connection that joins a
 * room that moves or changes is rebuilt, and the walls of the rooms that it joins are cut
 * again. Rebuilt brushes take the materials of the old brushes on the same side.
 *
 * @throw CliError `hand_edited` if the brushes of a room or connection that would be
 * rebuilt no longer match what its parameters build, without `force`, `locked` if one
 * of them is locked, without --allow-locked, `node_not_found` and
 * `ambiguous_room_name` if the room or a room it is placed against or joined to is
 * missing or ambiguous, the layout errors of `add_room` and `connect`,
 * `connect_cuts_linked_room` if the walls of a room inside a linked group would be cut,
 * and `op_failed` if `place` names the room itself or a room placed against it, or a
 * room or connection has no brushes or malformed parameter keys
 */
void editRoom(OpContext& context);

/**
 * Checks the arguments of `edit_room`.
 *
 * @throw CliError `invalid_argument` if the room name is not made of letters, digits and
 * underscores, or a size or the wall thickness is not greater than 0
 */
void checkEditRoomArgs(const Json& args);

/**
 * `edit_connection {connection, width?, height?, offset?, force?}`: changes the
 * parameters of the connection `A-B` (or `B-A`), rebuilds its corridor and cuts the
 * walls of the two rooms again.
 *
 * @throw CliError as editRoom does
 */
void editConnection(OpContext& context);

/**
 * Checks the arguments of `edit_connection`.
 *
 * @throw CliError `invalid_argument` if the connection is not named by two different
 * room names joined by a dash, or the width or height is not greater than 0
 */
void checkEditConnectionArgs(const Json& args);

/**
 * `detach {room | connection}`: strips the `_tbcli_*` keys from the carrier of a built
 * room or connection, leaving plain brushes in a plain func_group that tbcli no longer
 * rebuilds. The wrapping group keeps its name, which does not make a built room, since
 * built rooms are identified by their carriers. The rooms that a detached connection
 * joined stay rebuildable, but rebuilding them closes the opening; a room joined to a
 * detached room cannot be rebuilt until the connection is detached too.
 *
 * @throw CliError `node_not_found` or `ambiguous_room_name` if the name means no room
 * or connection, or several, and `locked` if the carrier is locked, without
 * --allow-locked
 */
void detach(OpContext& context);

/**
 * Checks the arguments of `detach`.
 *
 * @throw CliError `invalid_argument` if not exactly one of room and connection is given,
 * or the name is malformed
 */
void checkDetachArgs(const Json& args);

} // namespace tb::cli
