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

#include "fs/DiskIO.h"
#include "fs/ImageFileSystem.h"

#include <filesystem>
#include <string>

namespace tb::fs
{

template <typename FS>
auto openFS(const std::filesystem::path& path)
{
  return Disk::openFile(path) | kdl::and_then([](auto file) {
           return createImageFileSystem<FS>(std::move(file));
         })
         | kdl::transform([&](auto fs) {
             fs->setMetadata(makeImageFileSystemMetadata(path));
             return fs;
           })
         | kdl::value();
}

/** Sets the permissions of a path and restores the original ones when destroyed. */
class SetPermissions
{
private:
  std::filesystem::path m_path;
  std::filesystem::perms m_permissions;

public:
  explicit SetPermissions(std::filesystem::path path, std::filesystem::perms permissions);
  ~SetPermissions();

  SetPermissions(const SetPermissions&) = delete;
  SetPermissions& operator=(const SetPermissions&) = delete;
};

std::string readTextFile(const std::filesystem::path& path);
Result<std::string> readTextFile(const FileSystem& fs, const std::filesystem::path& path);

} // namespace tb::fs
