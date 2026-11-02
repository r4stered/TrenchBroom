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
 * translation and compared on geometry only.
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

/** The built rooms of the map, in file order. */
std::vector<BuiltRoom> collectBuiltRooms(const mdl::WorldNode& worldNode);

/**
 * The built room of a carrier or of the group that wraps it, or nothing if the node is
 * neither.
 */
std::optional<BuiltRoom> builtRoomOf(const mdl::Node& node);

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
 * exist, `locked` if the default layer is locked, without --allow-locked,
 * and `op_failed` if several rooms have that name, the room to place it against has no
 * brushes or malformed parameter keys, or a room with the new name exists already
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
 * linked group, `hand_edited` if a room's brushes no longer match what tbcli built, and
 * `op_failed` if a connection joins the rooms already, several rooms have a name, or a
 * room has no brushes or malformed parameter keys
 */
void connect(OpContext& context);

/**
 * Checks the arguments of `connect`.
 *
 * @throw CliError `invalid_argument` if a room name is not made of letters, digits and
 * underscores, both name the same room, or the width or height is not greater than 0
 */
void checkConnectArgs(const Json& args);

} // namespace tb::cli
