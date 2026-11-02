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

#include "cli/Json.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <string>

namespace tb::cli
{

/** `[x, y, z]` */
Json toJson(const vm::vec3d& v);

/** `{"min": [x, y, z], "max": [x, y, z]}` */
Json toJson(const vm::bbox3d& bounds);

/** `x y z`, with the numbers written as formatNumber writes them. */
std::string formatVec3(const vm::vec3d& v);

/** The point of a well-formed `[x, y, z]`. */
vm::vec3d vec3FromJson(const Json& json);

/** The box of a well-formed `{"min": [x, y, z], "max": [x, y, z]}`. */
vm::bbox3d bboxFromJson(const Json& json);

} // namespace tb::cli
