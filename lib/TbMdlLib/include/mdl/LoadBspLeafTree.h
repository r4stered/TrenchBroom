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

#include "base/Result.h"
#include "fs/Reader.h"

#include "kd/reflection_decl.h"

#include "vm/bbox.h"
#include "vm/plane.h"
#include "vm/vec.h"

#include <array>
#include <cstdint>
#include <iosfwd>
#include <vector>

namespace tb::mdl
{

/**
 * The contents of a BSP leaf. Values other than the enumerators can occur.
 */
enum class BspLeafContents : int32_t
{
  Empty = -1,
  Solid = -2,
  Water = -3,
  Slime = -4,
  Lava = -5,
  Sky = -6,
};

std::ostream& operator<<(std::ostream& lhs, BspLeafContents rhs);

struct BspLeaf
{
  BspLeafContents contents;
  vm::bbox3d bounds;

  kdl_reflect_decl(BspLeaf, contents, bounds);
};

/**
 * The BSP tree of hull 0 of the world model (model 0) of a Quake or Half-Life BSP file.
 *
 * Leaf 0 is the solid leaf shared by all solid space. The other leafs are the vis leafs:
 * BSP leaf i+1 is vis leaf i of the portal file.
 */
class BspLeafTree
{
public:
  /** A reference to a node or a leaf of the tree. */
  struct Child
  {
    enum class Type
    {
      Node,
      Leaf,
    };

    Type type;
    size_t index;
  };

  struct Node
  {
    vm::plane3d plane;

    /** The children on the front and back side of the plane. */
    std::array<Child, 2> children;
  };

private:
  std::vector<Node> m_nodes;
  std::vector<BspLeaf> m_leafs;
  Child m_root;

public:
  /**
   * Creates a tree from the given nodes and leafs. Every child reachable from the given
   * root must refer to a valid node or leaf, and every node must be reachable only once.
   */
  BspLeafTree(std::vector<Node> nodes, std::vector<BspLeaf> leafs, Child root);

  /**
   * The leafs of the world model, indexed by BSP leaf index. Includes the shared solid
   * leaf 0.
   */
  const std::vector<BspLeaf>& leafs() const;

  /**
   * Returns the index of the leaf that contains the given point. A point on a node plane
   * belongs to the back side, as in the Quake engine.
   */
  size_t findLeaf(const vm::vec3d& point) const;
};

/**
 * Reads the planes, nodes and leafs lumps of a BSP file (Quake BSP29 and BSP2, and
 * Half-Life BSP30) and returns the leaf tree of hull 0 of the world model.
 */
Result<BspLeafTree> loadBspLeafTree(fs::Reader reader);

} // namespace tb::mdl
