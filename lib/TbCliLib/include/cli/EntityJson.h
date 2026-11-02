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

namespace tb::mdl
{
class EntityNodeBase;
}

namespace tb::cli
{

class Json;

/**
 * An entity in full: `{path, classname, origin | aabb, properties, definition,
 * spawnflags?}`.
 *
 * Point entities have an `origin` and brush entities an `aabb`; worldspawn has
 * neither. `properties` holds only the properties actually set, in file order.
 * `definition` is `found` or `missing`. If spawnflags are set, `spawnflags` lists each
 * set bit by the name the entity definition gives it, or as its value if it has none.
 */
Json entityToJson(const mdl::EntityNodeBase& node);

} // namespace tb::cli
