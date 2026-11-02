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
#include <memory>
#include <string>
#include <string_view>

namespace kdl
{
class task_manager;
}

namespace tb
{
class Logger;

namespace gl
{
class ResourceManager;
}

namespace mdl
{
struct GameInfo;
class Map;
} // namespace mdl

namespace cli
{
struct Config;

/**
 * A map loaded headlessly together with everything the map library needs to keep it
 * alive. The logger passed to loadMap must outlive this.
 */
struct LoadedMap
{
  /** The short SHA-256 prefix of the file's bytes as they were loaded. */
  std::string rev;

  std::unique_ptr<kdl::task_manager> taskManager;
  std::unique_ptr<gl::ResourceManager> resourceManager;
  /** Referenced by the map, which does not own it. */
  std::unique_ptr<mdl::GameInfo> gameInfo;
  // declared last so that it is destroyed first
  std::unique_ptr<mdl::Map> map;

  LoadedMap();
  LoadedMap(LoadedMap&&) noexcept;
  LoadedMap& operator=(LoadedMap&&) noexcept;
  ~LoadedMap();
};

/** The rev of a file with the given bytes. */
std::string computeRev(std::string_view bytes);

/**
 * Loads the given Valve 220 map using the game config and FGD from the given config.
 * No GL context and no Quake directory are required.
 *
 * Content the reader skips (such as invalid faces) is logged and does not fail the load.
 *
 * @throw CliError if the map cannot be read, is not a Valve 220 map, does not parse, or
 * the configuration is incomplete
 */
LoadedMap loadMap(
  const std::filesystem::path& mapPath, const Config& config, Logger& logger);

} // namespace cli
} // namespace tb
