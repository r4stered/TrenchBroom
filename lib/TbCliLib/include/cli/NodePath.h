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

#include <string>
#include <string_view>

namespace tb::mdl
{
class Node;
}

namespace tb::cli
{

/** The path of the world node, which is also the worldspawn entity. */
inline constexpr auto WorldPath = std::string_view{"world"};

/**
 * The node path of the given node: `layer:<n>/group:<n>/entity:<n>/brush:<n>`, where
 * each index counts the preceding siblings of the same kind in file order. `layer:0`
 * is the default layer. The world node's path is `world`.
 *
 * Paths are positional and only valid for the rev they were computed from.
 */
std::string nodePath(const mdl::Node& node);

} // namespace tb::cli
