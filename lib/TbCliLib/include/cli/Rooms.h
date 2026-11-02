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

#include "cli/CappedList.h"

#include "vm/bbox.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>

namespace tb::cli
{

class Json;
struct Config;
struct LoadedMap;

/** The default of `--split-ratio`. */
inline constexpr auto DefaultSplitRatio = 0.5;

struct RoomsOptions
{
  Paging paging;
  /**
   * τ: a portal is a constriction if the open area around it is less than τ times the
   * cross-section of the space on either side.
   */
  double splitRatio = DefaultSplitRatio;
  /** List the unmerged cells and their portals instead of the rooms. */
  bool cells = false;
  /** List only the room with this id, or only its cells. */
  std::optional<size_t> room;
  /** List only the rooms, or cells, whose bounds lie inside this box. */
  std::optional<vm::bbox3d> within;
  /** Add the cell portals' polygons to each opening. */
  bool raw = false;
  /** Read the `.bsp`, `.prt` and sidecar of `verify --compile --artifacts` from here. */
  std::optional<std::filesystem::path> fromArtifacts;
  /** Replaces qbsp's timeout from the configuration. */
  std::optional<std::chrono::milliseconds> timeout;
};

/**
 * The `tbcli.rooms/1` result `{schema, rev, heuristic: true, method: {name:
 * "constriction", split_ratio}, summary: {rooms, openings, components}, rooms: {items,
 * total, truncated}, openings: [...]}`, or with `options.cells`, `{..., summary, cells:
 * {items, total, truncated}, portals: [...]}`.
 *
 * Runs qbsp only, with the configured compiler and timeout, on the map's bytes in a
 * temporary directory, or reads the artifacts of `verify --compile --artifacts`. qbsp
 * runs twice: first with its defaults, to find a leak as verify does, then with
 * `-nodetail -forceprt1`, which makes detail brushes and liquids structural, so that
 * every leaf is its own cell and a liquid's surface is a portal. The cells are the vis
 * leafs of that portal file, with the bounds and contents of their BSP leafs; sky cells
 * are left out.
 *
 * Cells joined by a portal merge into one room unless the portal is cut: where the
 * contents change, or where the portal is a constriction. A portal is a constriction if
 * the portals beside it in its plane cover less than the split ratio of the cross-section
 * of the cells of either room that lie against the plane; it is judged against the
 * rooms merged so far, the most open portals first. Afterwards each minor room joins
 * its neighbouring room of the same contents with the lowest leaf: a room that is only a
 * passage (a doorway: no deeper than it is wide, and open where it meets its
 * neighbours), or that is too small for a player (32 x 32 x 56). Minor rooms with no
 * other neighbour merge with each other.
 *
 * A room is `{id, bounds, volume, leaves, contents, entities, key_entities, openings}`;
 * bounds and volume are those of its leafs' bounding boxes. Ids are ordered by the
 * lowest leaf of each room, and point entities belong to the room of the leaf that holds
 * their origin. `openings` lists the ids of the room's openings.
 *
 * An opening is the coplanar merge of the cut portals between two rooms: portals in one
 * plane that share part of an edge merge, and separate gaps stay separate openings. It
 * is `{id, kind?, rooms: [a, b], center, normal, bounds, area, width, height, brushes,
 * gated_by}`, with `polygons` (the cell portals) under `options.raw`. `kind` is
 * `liquid_surface` where the contents change, and absent otherwise. `rooms` are
 * ascending, and `normal` points from the first room into the second. `center` is the
 * centroid of the portals. `width` and `height` are the extents of the portals in the
 * plane, horizontally and along the plane's slope (along x and y for a horizontal
 * plane). `brushes` are the world brushes (of worldspawn, func_group and func_detail and
 * its variants, which `-nodetail` builds into the world) whose bounds touch the
 * opening's boundary within 0.01; `gated_by` are the other brush entities, such as
 * doors, whose bounds overlap the portals. Openings are numbered by their rooms, then by
 * their first portal in the file. The openings of the listed rooms are listed in full,
 * unpaged.
 *
 * The summary counts the rooms, the openings and the connected components of the whole
 * map, unfiltered. Components join rooms through their openings and through teleporters:
 * a trigger_teleport joins the rooms its bounds overlap to the room of each
 * info_teleport_destination it targets.
 *
 * @throw CliError `map_leaks` with the `leak` issue if the map leaks, `compile_failed`
 * if qbsp fails or writes no usable output, `stale_artifact` if the artifacts are
 * missing, leaked, of another rev or of clusters rather than leafs, or as `compile`
 * does
 */
Json rooms(LoadedMap& loadedMap, const Config& config, const RoomsOptions& options);

} // namespace tb::cli
