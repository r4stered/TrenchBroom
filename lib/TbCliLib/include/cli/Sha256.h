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

#include <string>
#include <string_view>

namespace tb::cli
{

/**
 * The SHA-256 digest of the given bytes as 64 lowercase hex digits. Used for revs;
 * implemented here because TrenchBroom has no hashing library outside of Qt.
 */
std::string sha256Hex(std::string_view bytes);

} // namespace tb::cli
