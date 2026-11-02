/*
 Copyright (C) 2025 Kristian Duske

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

#include "base/Result.h"

#include <string>
#include <vector>

namespace tb
{
class ParserStatus;
}

namespace tb::mdl
{
class Map;
class Node;

enum class PasteType;

std::string serializeSelectedNodes(Map& map);
std::string serializeSelectedBrushFaces(Map& map);

PasteType paste(Map& map, const std::string& str);

/**
 * Pastes the nodes that the given string holds into the given parent, as paste does
 * into the current group or layer, and selects those that can be selected. Unlike paste,
 * it does not try to paste brush faces. The parser reports what it skips to the given
 * status.
 *
 * Returns the pasted nodes that were added to the parent, or an error if the string holds
 * no nodes or they could not be added.
 */
Result<std::vector<Node*>> pasteNodes(
  Map& map, const std::string& str, Node& parent, ParserStatus& status);


} // namespace tb::mdl
