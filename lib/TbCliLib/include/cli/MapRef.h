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

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mdl
{
class Map;
} // namespace tb::mdl

namespace tb::cli
{

class NodeHandleTable;

/** Something the map reader skipped or changed while loading. */
struct LoadProblem
{
  /** The reader's message, without its location. */
  std::string message;
  /** The line the reader reported it at, if it reported one. */
  std::optional<size_t> line;
};

/**
 * The map a command reads or edits, owned by someone else: by the loaded file in file
 * mode, or by whoever lends it.
 */
struct MapRef
{
  mdl::Map& map;
  /** The rev that the command reports the map at. */
  std::string rev;
  /**
   * What the reader skipped or changed while loading the map. A load with any such
   * problems is lossy: saving would not write back what the file holds.
   */
  const std::vector<LoadProblem>& loadProblems;
  /**
   * The node handles of the live session whose map this is, which commands accept as
   * well as node paths and report next to them; null in file mode.
   */
  NodeHandleTable* handles = nullptr;
};

/**
 * The bytes a map was read from, for the commands that compile them, and how long
 * reading them took. A map that was not read from a file, such as one the editor
 * lends, has none.
 */
struct MapSource
{
  std::string_view bytes;
  /** How long reading the map took, in milliseconds. */
  double loadMs = 0.0;
};

/** The name of the map's file without its extension, as compilers name outputs. */
std::string mapName(const MapRef& mapRef);

} // namespace tb::cli
