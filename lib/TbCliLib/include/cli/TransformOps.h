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

#include "vm/util.h"

#include <optional>
#include <span>
#include <string_view>

namespace tb::cli
{

class Json;
struct OpContext;

/** The names of the axes that rotate and flip take: `x`, `y` and `z`. */
std::span<const std::string_view> axisNames();

/** The axis with the given name, if it is one of axisNames(). */
std::optional<vm::axis::type> parseAxis(std::string_view name);

/*
 * The transform ops: translate, rotate, scale and flip. Each moves its targets as the
 * editor's transform tools do, with UV lock on unless `uv_lock` is false. A layer stands
 * for the nodes in it, and a target inside another target moves with it once. The
 * targets are reported as modified, and so are the entities that change with them: a
 * brush entity whose brushes all move.
 *
 * Each throws CliError `op_failed` if a brush cannot be transformed, for example because
 * it would leave the world bounds.
 */

/** `translate {target, delta, uv_lock}`: moves the targets by delta. */
void translate(OpContext& context);

/**
 * `rotate {target, axis, degrees, center?, uv_lock}`: rotates the targets about an axis
 * through the center, counterclockwise when looking against the axis, as in a
 * right-handed coordinate system. The center defaults to the center of the targets'
 * bounds. Point entities have their angle, angles or mangle property updated.
 */
void rotate(OpContext& context);

/**
 * `scale {target, to | factors, center?, uv_lock}`: scales the targets so that their
 * bounds become `to`, or by `factors` about the center, which defaults to the center of
 * the targets' bounds.
 */
void scale(OpContext& context);

/**
 * Checks the arguments of `scale`.
 *
 * @throw CliError `invalid_argument` unless exactly one of `to` and `factors` is given,
 * if `center` is given with `to`, if `to` is empty on an axis, or if a factor is 0
 */
void checkScaleArgs(const Json& args);

/**
 * `flip {target, axis, center?, uv_lock}`: mirrors the targets along an axis through
 * the center, which defaults to the center of the targets' bounds.
 */
void flip(OpContext& context);

} // namespace tb::cli
