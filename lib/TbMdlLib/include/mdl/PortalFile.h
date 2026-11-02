/*
 Copyright (C) 2010 Kristian Duske

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

#include "kd/reflection_decl.h"

#include "vm/polygon.h"

#include <filesystem>
#include <iosfwd>
#include <vector>

namespace tb::mdl
{

/**
 * A portal between two cells of a portal graph.
 */
struct PortalGraphPortal
{
  vm::polygon3f polygon;

  /** The indices of the two cells (clusters) that this portal connects. */
  size_t a;
  size_t b;

  kdl_reflect_decl(PortalGraphPortal, polygon, a, b);
};

/**
 * The contents of a portal file written by a BSP compiler.
 *
 * The cells of the graph are vis clusters. For PRT1 files, every vis leaf is its own
 * cluster. For PRT2 and PRT1-AM files, a cluster groups one or more vis leafs. In Quake
 * BSP files, vis leaf i is BSP leaf i+1, because BSP leaf 0 is the shared solid leaf.
 */
struct PortalGraph
{
  size_t leafCount;
  size_t clusterCount;
  std::vector<PortalGraphPortal> portals;

  /** Maps each vis leaf index to the index of the cluster that contains it. */
  std::vector<size_t> clusterOfLeaf;

  kdl_reflect_decl(PortalGraph, leafCount, clusterCount, portals, clusterOfLeaf);
};

bool canLoadPortalFile(const std::filesystem::path& path);

/**
 * Parses a PRT1 (including the Quake 3 variant), PRT2 or PRT1-AM portal file.
 */
Result<PortalGraph> loadPortalGraph(std::istream& stream);

/**
 * Returns the portal polygons of the given portal file, for the portal viewer.
 *
 * This is the projection of loadPortalGraph onto the polygons, but it is more lenient: it
 * ignores the cell indices and everything after the portal lines.
 */
Result<std::vector<vm::polygon3f>> loadPortalFile(std::istream& stream);

} // namespace tb::mdl
