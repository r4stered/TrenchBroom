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

#include "vm/bbox.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace tb::mdl
{
class BrushNode;
class EntityNode;
class GroupNode;
class Node;
class PatchNode;
} // namespace tb::mdl

namespace tb::cli
{

/** The nodes below a node, in file order. */
struct MapContents
{
  std::vector<const mdl::GroupNode*> groups;
  std::vector<const mdl::EntityNode*> entities;
  std::vector<const mdl::BrushNode*> brushes;
  std::vector<const mdl::PatchNode*> patches;
};

/**
 * The given node's contents, including the node itself if it is a group, entity, brush or
 * patch.
 */
MapContents collectContents(const mdl::Node& node);

/** Counts and bounds of some contents. */
struct Summary
{
  size_t brushes = 0;
  size_t entities = 0;
  size_t faces = 0;
  /** Unset if the contents have nothing with bounds. */
  std::optional<vm::bbox3d> bounds;

  void addBounds(const vm::bbox3d& nodeBounds);
};

/**
 * Counts and bounds of the given contents. The bounds merge the bounds of brushes,
 * patches and point entities.
 */
Summary summarize(const MapContents& contents);

} // namespace tb::cli
