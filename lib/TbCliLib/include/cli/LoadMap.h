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

#include "base/ParserStatus.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

/** Something the map reader skipped or changed while loading. */
struct LoadProblem
{
  /** The reader's message, without its location. */
  std::string message;
  /** The line the reader reported it at, if it reported one. */
  std::optional<size_t> line;
};

/**
 * Logs what the map reader reports about the given text and records its warnings and
 * errors, each of which means that the reader skipped or changed some of the text's
 * content.
 *
 * This relies on the reader warning only about content it drops or changes, which holds
 * for every warning in MapReader, StandardMapParser and WorldReader today. A purely
 * informational warning added there would make every map that triggers it lossy.
 */
class ProblemCollectingParserStatus : public ParserStatus
{
private:
  Logger& m_logger;
  std::string_view m_bytes;
  std::vector<LoadProblem>& m_problems;

public:
  ProblemCollectingParserStatus(
    Logger& logger, std::string_view bytes, std::vector<LoadProblem>& problems);

private:
  void doProgress(double progress) override;
  void doLog(LogLevel level, const std::string& str) override;
};

/**
 * A map loaded headlessly together with everything the map library needs to keep it
 * alive. The logger passed to loadMap must outlive this.
 */
struct LoadedMap
{
  /** The short SHA-256 prefix of the file's bytes as they were loaded. */
  std::string rev;
  /** The file's bytes as they were loaded, which `verify --compile` compiles. */
  std::string bytes;
  /**
   * What the reader skipped or changed while loading, as it logged it. A load with any
   * such problems is lossy: saving would not write back what the file holds.
   */
  std::vector<LoadProblem> loadProblems;
  /** How long reading the map took, in milliseconds. */
  double loadMs = 0.0;

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
 * Content the reader skips or changes (such as invalid faces) is logged and recorded in
 * `loadProblems`, and does not fail the load.
 *
 * @throw CliError if the map cannot be read, is not a Valve 220 map, does not parse, or
 * the configuration is incomplete
 */
LoadedMap loadMap(
  const std::filesystem::path& mapPath, const Config& config, Logger& logger);

/** The name of the loaded map's file without its extension, as compilers name outputs. */
std::string mapName(const LoadedMap& loadedMap);

} // namespace cli
} // namespace tb
