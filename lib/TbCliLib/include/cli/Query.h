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
#include "cli/NodePath.h"

#include "vm/bbox.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace tb::mdl
{
class Map;
}

namespace tb::cli
{

class Json;
struct LoadedMap;

/** How far apart two boxes may be and still count as touching or intersecting. */
inline constexpr auto SpatialTolerance = 0.01;

struct QueryOptions
{
  /** The TbQl query. Without one, every node but the world matches. */
  std::optional<std::string> query;
  Paging paging;
  /** Return only the total. */
  bool countOnly = false;
  /** Keep matches whose bounds lie inside this box. */
  std::optional<vm::bbox3d> within;
  /** Keep matches whose bounds overlap this box. */
  std::optional<vm::bbox3d> intersects;
  /** Keep matches whose bounds overlap the bounds of the node or face at this path. */
  std::optional<std::string> touching;
};

/**
 * The nodes or faces that a TbQl query matches, or every node but the world if there is
 * no query, in file order: a node precedes its descendants, and a brush its faces.
 */
std::vector<NodeOrFace> queryMatches(
  mdl::Map& map, const std::optional<std::string>& query);

/**
 * The `tbcli.query/1` result: `{schema, rev, items, total, truncated}`, or
 * `{schema, rev, total}` if only the count is asked for.
 *
 * The matches of a TbQl query, or of every node but the world if there is none, in file
 * order, filtered by the spatial filters, which are all box-based and tolerate
 * SpatialTolerance. `--touching` leaves out the node it names and that node's ancestors
 * and descendants. A face query matches faces, all others match nodes. `items` holds the
 * summaries of the matches from `offset` on, at most `limit` of them.
 *
 * Entities are summarized as boundedEntityToJson writes them.
 *
 * @throw CliError if the `touching` path does not resolve, or `wrong_node_kind` if it is
 * the world, which nothing can touch
 */
Json query(LoadedMap& loadedMap, const QueryOptions& options);

} // namespace tb::cli
