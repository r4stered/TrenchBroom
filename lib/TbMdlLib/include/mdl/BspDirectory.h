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

#include <cstddef>
#include <cstdint>

namespace tb::mdl
{

/**
 * The lump directory shared by Quake (BSP29, BSP2) and Half-Life (BSP30) BSP files: the
 * file offset of each lump's directory entry, which holds the lump's offset and length.
 */
namespace BspDirectory
{
constexpr size_t PlanesAddress = 0x0C;
constexpr size_t MaterialsAddress = 0x14;
constexpr size_t VerticesAddress = 0x1C;
constexpr size_t NodesAddress = 0x2C;
constexpr size_t TexInfosAddress = 0x34;
constexpr size_t FacesAddress = 0x3C;
constexpr size_t LeafsAddress = 0x54;
constexpr size_t EdgesAddress = 0x64;
constexpr size_t FaceEdgesAddress = 0x6C;
constexpr size_t ModelsAddress = 0x74;

/** The size of a model in the models lump. */
constexpr size_t ModelSize = 0x40;
} // namespace BspDirectory

/**
 * Whether the given version is Quake's BSP29 or Half-Life's BSP30, which share the same
 * layout apart from the textures.
 */
constexpr bool isBsp29Or30(const int32_t version)
{
  return version == 29 || version == 30;
}

} // namespace tb::mdl
