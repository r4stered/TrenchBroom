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

#include "cli/FileUtils.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <fstream>
#include <iterator>
#include <random>

namespace tb::cli
{

std::optional<std::string> readFile(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  if (!stream)
  {
    return std::nullopt;
  }

  auto result =
    std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
  if (stream.bad())
  {
    return std::nullopt;
  }
  return result;
}

namespace
{

std::filesystem::path temporaryPath(const std::filesystem::path& path)
{
  auto random = std::random_device{};
  auto name = path.filename();
  name += fmt::format(".tbcli-{:08x}.tmp", random());
  return path.parent_path() / name;
}

std::optional<std::string> writeAndConfirm(
  const std::filesystem::path& path,
  const std::string_view bytes,
  const FileReader readBack)
{
  {
    auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
    if (!stream)
    {
      return fmt::format("cannot create {}", path);
    }
    stream.write(bytes.data(), std::streamsize(bytes.size()));
    stream.close();
    if (!stream)
    {
      return fmt::format("cannot write {}", path);
    }
  }

  if (readBack(path) != bytes)
  {
    return fmt::format("{} does not read back as written", path);
  }
  return std::nullopt;
}

} // namespace

std::optional<std::string> writeFileAtomically(
  const std::filesystem::path& path,
  const std::string_view bytes,
  const FileReader readBack)
{
  const auto tempPath = temporaryPath(path);
  auto error = writeAndConfirm(tempPath, bytes, readBack);
  if (!error)
  {
    auto ec = std::error_code{};
    std::filesystem::rename(tempPath, path, ec);
    if (ec)
    {
      error = fmt::format("cannot rename {} to {}: {}", tempPath, path, ec.message());
    }
  }

  if (error)
  {
    auto ec = std::error_code{};
    std::filesystem::remove(tempPath, ec);
  }
  return error;
}

} // namespace tb::cli
