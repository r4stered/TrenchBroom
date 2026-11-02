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

namespace tb::cli
{

class Json;
struct IssueReport;
struct LoadedMap;

/**
 * The `tbcli.describe/1` result for the given map: a summary whose size is bounded
 * regardless of the size of the map. Lists that could grow with the map are capped and
 * written as `{items, total, truncated}`.
 *
 * Entities of the given key entity classes are listed in full, in addition to
 * worldspawn and every entity with a targetname or target. Without a list, the player
 * starts and trigger_changelevel are.
 *
 * `issues` counts the given issues of the in-process verify stages per type and
 * severity, as verify does.
 */
Json describe(
  const LoadedMap& loadedMap,
  const std::optional<std::vector<std::string>>& keyEntityClasses,
  const IssueReport& issueReport);

} // namespace tb::cli
