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

struct OpContext;

/**
 * The implementations of the ops that Ops.cpp defines. Each takes the arguments its
 * definition declares, already validated, and throws CliError if it fails.
 */

/**
 * `create_box {min, max, material, parent}`: an axis-aligned box brush in a layer,
 * group or brush entity.
 */
void createBox(OpContext& context);

/**
 * `delete {target}`: removes groups, entities, brushes and patches. An entity or group
 * left empty is removed with them and is reported as removed after the targets.
 */
void deleteNode(OpContext& context);

} // namespace tb::cli
