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
struct MapRef;

/**
 * The `tbcli.fgd/1` result for the entity definition of the given class, as the map's
 * entity definitions define it, with base classes resolved:
 * `{schema, rev, classname, type: point | brush, description, size?, properties,
 * spawnflags}`.
 *
 * Point classes have a `size`. Each property is `{key, type, description,
 * long_description?, default?, choices?, read_only?}`, where `type` is the FGD type
 * name and `default` is a string, as property values are. Point classes include the
 * `origin` property that TrenchBroom defines for them. `spawnflags` lists
 * `{value, name, description?, default}` per flag and is not repeated among the
 * properties. A class's own properties and flags come before those of its bases.
 *
 * @throw CliError `undefined_class` if no entity definition has the given name, with a
 * message that says so if no entity definitions are loaded at all
 */
Json fgd(const MapRef& mapRef, std::string_view classname);

} // namespace tb::cli
