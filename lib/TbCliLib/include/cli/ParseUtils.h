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

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace tb::cli
{

/** The value of `true` or `false`, or nothing for any other string. */
std::optional<bool> parseBool(std::string_view str);

/**
 * The value of a string that consists of decimal digits only, or nothing if the string
 * is empty, has any other character (including a sign or whitespace) or overflows.
 */
std::optional<size_t> parseUnsigned(std::string_view str);

/**
 * The value of a decimal number such as `-1`, `0.5` or `1e3`, or nothing unless the
 * whole string is such a number.
 */
std::optional<double> parseDecimal(std::string_view str);

/**
 * The numbers of a list such as `1,2.5,-3` or `1 2.5 -3`, separated by commas, spaces
 * or both, or nothing if any element is not a decimal number (see parseDecimal).
 */
std::optional<std::vector<double>> parseNumberList(std::string_view str);

/** The names of the axes that ops take: `x`, `y` and `z`. */
std::span<const std::string_view> axisNames();

/** The axis with the given name, if it is one of axisNames(). */
std::optional<vm::axis::type> parseAxis(std::string_view name);

} // namespace tb::cli
