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

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace tb::cli
{

/** Reads the raw bytes of the given file, or nullopt if it cannot be read. */
std::optional<std::string> readFile(const std::filesystem::path& path);

/**
 * Writes the given bytes to the given file, replacing it.
 *
 * @return a description of the failure, or nullopt if the file was written
 */
std::optional<std::string> writeFile(
  const std::filesystem::path& path, std::string_view bytes);

/** A function that reads a file as readFile does. */
using FileReader = std::optional<std::string> (*)(const std::filesystem::path&);

/**
 * Replaces the given file with the given bytes atomically: the bytes are written to a
 * temporary file next to it, read back and compared, and only then is the temporary
 * file renamed over the target. If any step fails, the temporary file is removed and
 * the target is left as it was.
 *
 * `readBack` reads the temporary file back; tests replace it to simulate a write that
 * does not read back as written.
 *
 * @return a description of the failed step, or nullopt if the file was written
 */
std::optional<std::string> writeFileAtomically(
  const std::filesystem::path& path,
  std::string_view bytes,
  FileReader readBack = readFile);

} // namespace tb::cli
