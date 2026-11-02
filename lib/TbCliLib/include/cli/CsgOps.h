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

namespace tb::cli
{

class Json;
struct OpContext;

/*
 * The CSG ops hollow, subtract, merge and intersect, and snap_vertices. Each acts on
 * brushes, with layers, groups and brush entities standing for the brushes inside them,
 * as the editor's CSG commands do on the selection. The brushes that a CSG op makes are
 * reported as created, in file order, and the brushes it replaces as removed, with the
 * entities and groups it leaves empty. An empty query target does nothing.
 */

/**
 * `hollow {target, thickness}`: replaces each brush with walls of the given thickness
 * around its inside, as the editor's hollow does with the grid size as the thickness.
 *
 * @throw CliError `op_failed` if a brush cannot be hollowed, typically because it is
 * too thin
 */
void hollow(OpContext& context);

/**
 * Checks the arguments of `hollow`.
 *
 * @throw CliError `invalid_argument` if the thickness is not greater than 0
 */
void checkHollowArgs(const Json& args);

/**
 * `subtract {cutters, from, keep_cutters}`: subtracts the cutters from each brush of
 * `from` that they overlap, replacing it with the fragments that remain, and removes the
 * cutters unless `keep_cutters` is set. Brushes of `from` that no cutter overlaps, and
 * the cutters themselves, are left as they are. If no cutter overlaps a brush of `from`,
 * nothing changes and the op warns `nothing_cut`.
 */
void subtract(OpContext& context);

/**
 * `merge {target}`: replaces the brushes with the convex hull of their vertices, in the
 * group, layer or entity of the first brush. The hull's faces take the materials and UV
 * of the best matching faces of the brushes.
 *
 * @throw CliError `op_failed` if the target is a single brush
 */
void merge(OpContext& context);

/**
 * `intersect {target}`: replaces the brushes with the space they all share, in the
 * group or layer that holds them.
 *
 * @throw CliError `op_failed` if the target is a single brush, or if the brushes share
 * no volume
 */
void intersect(OpContext& context);

/**
 * `snap_vertices {target, grid}`: moves every vertex of the brushes to the nearest
 * multiple of the grid, as the editor's snap vertices does with UV lock on. The brushes
 * whose vertices move are reported as modified.
 *
 * @throw CliError `op_failed` if a brush's vertices cannot be snapped, for example
 * because the brush would collapse
 */
void snapVertices(OpContext& context);

/**
 * Checks the arguments of `snap_vertices`.
 *
 * @throw CliError `invalid_argument` if the grid is not greater than 0
 */
void checkSnapVerticesArgs(const Json& args);

} // namespace tb::cli
