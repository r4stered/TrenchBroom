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

#include "cli/EditorScopes.h"

#include "mdl/EditorContext.h"
#include "mdl/Map.h"
#include "mdl/Map_Selection.h"

namespace tb::cli
{

SelectionScope::SelectionScope(mdl::Map& map, const std::vector<mdl::Node*>& nodes)
  : m_map{map}
{
  mdl::deselectAll(m_map);
  mdl::selectNodes(m_map, nodes);
}

SelectionScope::~SelectionScope()
{
  mdl::deselectAll(m_map);
}

AlignmentLockScope::AlignmentLockScope(mdl::Map& map, const bool alignmentLock)
  : m_map{map}
  , m_oldAlignmentLock{map.editorContext().alignmentLock()}
{
  m_map.editorContext().setAlignmentLock(alignmentLock);
}

AlignmentLockScope::~AlignmentLockScope()
{
  m_map.editorContext().setAlignmentLock(m_oldAlignmentLock);
}

} // namespace tb::cli
