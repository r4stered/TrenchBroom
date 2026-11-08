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
#include "fs/File.h"
#include "fs/PathMatcher.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>

namespace tb::fs
{
enum class PathInfo;
struct TraversalMode;

namespace Disk
{
bool isCaseSensitive();

std::filesystem::path fixPath(const std::filesystem::path& path);

PathInfo pathInfo(const std::filesystem::path& path);

Result<std::vector<std::filesystem::path>> find(
  const std::filesystem::path& path,
  const TraversalMode& traversalMode,
  const PathMatcher& pathMatcher = matchAnyPath);

Result<std::shared_ptr<CFile>> openFile(const std::filesystem::path& path);

Result<bool> createDirectory(const std::filesystem::path& path);

Result<bool> deleteFile(const std::filesystem::path& path);

Result<void> copyFile(
  const std::filesystem::path& sourcePath, const std::filesystem::path& destPath);

Result<void> moveFile(
  const std::filesystem::path& sourcePath, const std::filesystem::path& destPath);

Result<void> renameDirectory(
  const std::filesystem::path& sourcePath, const std::filesystem::path& destPath);

/**
 * Writes a file by passing a text mode stream to the given function, all or nothing.
 *
 * The contents go to a temporary file in the same directory, which then replaces the file
 * at the given path. If anything fails, including the function throwing or leaving the
 * stream in a failed state, an error is returned and an existing file at the path is left
 * as it was. A read-only file is not replaced. A symlink at the path is kept and the file
 * it points to is replaced; a dangling symlink is an error.
 *
 * The file is replaced, not rewritten, so only its permissions carry over: hard links to
 * it, its owner, ACLs and other attributes do not. The data is not synced to the device
 * before the rename.
 */
Result<void> writeFileAtomically(
  const std::filesystem::path& path,
  const std::function<void(std::ostream&)>& writeContents);

std::filesystem::path resolvePath(
  const std::vector<std::filesystem::path>& searchPaths,
  const std::filesystem::path& path);

Result<std::filesystem::path> makeUniqueFilename(
  const std::filesystem::path& directoryPath);

} // namespace Disk
} // namespace tb::fs
