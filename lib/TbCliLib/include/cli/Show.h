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
struct LoadedMap;

/**
 * The `tbcli.show/1` result `{schema, rev, node}` for the node or face at the given path.
 *
 * - brushes are written in full, with vertices and faces (see brushToJson)
 * - faces are written in full (see faceToJson)
 * - entities are written with all their properties, brush entities add the paths of
 *   their `brushes`
 * - the world is written as worldspawn and adds the paths of its `layers`
 * - layers and groups add the paths of their `children`; linked groups add `link_id`
 *   and the paths of their `siblings`
 * - a built room's carrier and the group that wraps it add `instance_of` (the room's
 *   name), `instances` (their number), its `params` and its `status`
 *
 * The lists of child paths (`brushes`, `layers`, `children`) are written as
 * `{items, total, truncated}`; `siblings` is a plain array, as in describe, since a
 * link set rarely has many instances. `raw` adds the plane points of faces.
 *
 * @throw CliError if the path does not resolve
 */
Json show(LoadedMap& loadedMap, std::string_view path, bool raw);

} // namespace tb::cli
