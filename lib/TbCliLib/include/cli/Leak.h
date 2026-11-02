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

#include "vm/vec.h"

#include <optional>
#include <string_view>
#include <vector>

namespace tb::mdl
{
class BrushNode;
class Node;
} // namespace tb::mdl

namespace tb::cli
{

struct CompilerMessage;

/**
 * The points of a pointfile as qbsp writes it when the map leaks (`<map>.pts`): one
 * point `x y z` per line, along the trail from the occupant entity to the void. The
 * points are returned as they are, without resampling. Blank lines are skipped.
 *
 * Returns nothing if any other line is not a point.
 */
std::optional<std::vector<vm::vec3d>> parsePointFile(std::string_view text);

/**
 * The given trail reduced to its endpoints and the points where it turns: repeated
 * points and points on a straight line between their neighbours are left out.
 */
std::vector<vm::vec3d> simplifyTrail(const std::vector<vm::vec3d>& trail);

/**
 * At most `count` brushes under `root` that can seal the map (those of worldspawn,
 * func_group and func_detail), nearest first and in file order when equally near,
 * whose bounds are nearest to where the trail leaves the map: its last point inside
 * the bounds of those brushes before its first point outside, or its last point if it
 * never leaves those bounds. Returns nothing for an empty trail.
 */
std::vector<mdl::BrushNode*> brushesNearExit(
  mdl::Node& root, const std::vector<vm::vec3d>& trail, size_t count);

/**
 * Whether the message is qbsp's `Reached occupant "<classname>" at (<origin>)` line,
 * which names the entity from which the void can be reached when the map leaks.
 */
bool isLeakOccupant(const CompilerMessage& message);

} // namespace tb::cli
