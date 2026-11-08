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

#include <optional>
#include <string>
#include <vector>

namespace tb::mdl
{
class Entity;
}

namespace tb::cli
{

class Json;
struct IssueReport;
struct MapRef;

/**
 * The classes whose entities are key entities: the given ones, or else the player starts
 * and trigger_changelevel.
 */
const std::vector<std::string>& keyEntityClasses(
  const std::optional<std::vector<std::string>>& configured);

/** Whether the entity has a targetname or target. */
bool hasLink(const mdl::Entity& entity);

/** Whether the entity is of one of the given classes, or has a link. */
bool isKeyEntity(const mdl::Entity& entity, const std::vector<std::string>& classes);

/**
 * The `tbcli.describe/1` result for the given map: a summary whose size is bounded
 * regardless of the size of the map. Lists that could grow with the map are capped and
 * written as `{items, total, truncated}`.
 *
 * Entities of the given key entity classes are listed in full, in addition to
 * worldspawn and every entity with a targetname or target. Without a list, the player
 * starts and trigger_changelevel are.
 *
 * `built_rooms` lists the built rooms in file order, as builtRoomToJson writes them.
 *
 * `issues` counts the given issues of the in-process verify stages per type and
 * severity, as verify does.
 */
Json describe(
  const MapRef& mapRef,
  const std::optional<std::vector<std::string>>& keyEntityClasses,
  const IssueReport& issueReport);

} // namespace tb::cli
