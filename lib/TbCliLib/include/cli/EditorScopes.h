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

#include <vector>

namespace tb::mdl
{
class Map;
class Node;
} // namespace tb::mdl

namespace tb::cli
{

/**
 * Selects the given nodes for its lifetime, and then clears the selection. Ops select
 * internally to call the map library's selection-based functions.
 */
class SelectionScope
{
private:
  mdl::Map& m_map;

public:
  SelectionScope(mdl::Map& map, const std::vector<mdl::Node*>& nodes);
  ~SelectionScope();

  SelectionScope(const SelectionScope&) = delete;
  SelectionScope& operator=(const SelectionScope&) = delete;
};

/**
 * Sets the editor's alignment lock, which keeps materials aligned to faces that move,
 * for its lifetime, and then restores it.
 */
class AlignmentLockScope
{
private:
  mdl::Map& m_map;
  bool m_oldAlignmentLock;

public:
  AlignmentLockScope(mdl::Map& map, bool alignmentLock);
  ~AlignmentLockScope();

  AlignmentLockScope(const AlignmentLockScope&) = delete;
  AlignmentLockScope& operator=(const AlignmentLockScope&) = delete;
};

} // namespace tb::cli
