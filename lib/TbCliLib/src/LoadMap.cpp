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

#include "cli/LoadMap.h"

#include "base/Logger.h"
#include "base/SimpleParserStatus.h"
#include "cli/CliError.h"
#include "cli/Config.h"
#include "cli/FileUtils.h"
#include "cli/Sha256.h"
#include "gl/Resource.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "mdl/EntityProperties.h"
#include "mdl/EnvironmentConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/ParseGameConfig.h"
#include "mdl/WorldNode.h"
#include "mdl/WorldReader.h"

#include "kd/task_manager.h"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fmt/std.h>

#include <regex>

namespace tb::cli
{
namespace
{

constexpr auto RevLength = size_t{12};

// The editor's default world bounds.
const auto WorldBounds = vm::bbox3d{-32768.0, 32768.0};

std::string errorMessage(const auto& result)
{
  return std::visit([](const auto& e) { return e.msg; }, result.error());
}

/**
 * Extracts the line number from a parser error message. The map and game config
 * parsers report their location only as text ("At line N, column M: ..."), so this is
 * the only way to recover it.
 */
std::optional<size_t> lineOf(const std::string& message)
{
  static const auto pattern = std::regex{R"(line (\d+))"};
  if (auto match = std::smatch{}; std::regex_search(message, match, pattern))
  {
    return std::stoul(match[1].str());
  }
  return std::nullopt;
}

mdl::GameConfig loadGameConfig(const Config& config)
{
  if (!config.gameConfig)
  {
    throw configError(
      fmt::format(
        "no game config: set \"game_config\" in {} or pass --game-config",
        ConfigFileName));
  }

  const auto& path = *config.gameConfig;
  const auto contents = readFile(path);
  if (!contents)
  {
    throw configError(fmt::format("cannot read game config {}", path));
  }

  auto gameConfig = mdl::parseGameConfig(*contents, path);
  if (!gameConfig)
  {
    const auto message = errorMessage(gameConfig);
    throw configError(
      fmt::format("cannot parse game config {}: {}", path, message), lineOf(message));
  }

  return std::move(gameConfig).value();
}

mdl::GameInfo makeGameInfo(const Config& config, Logger& logger)
{
  auto gameConfig = loadGameConfig(config);

  if (!config.fgd.empty())
  {
    const auto& fgd = config.fgd.front();
    if (auto ec = std::error_code{}; !std::filesystem::is_regular_file(fgd, ec))
    {
      throw configError(fmt::format("FGD {} not found", fgd));
    }
    if (config.fgd.size() > 1)
    {
      logger.warn() << fmt::format(
        "Only the first FGD is loaded, ignoring {}",
        fmt::join(config.fgd.begin() + 1, config.fgd.end(), ", "));
    }
    // The map's definition file, unless worldspawn sets _tb_def.
    gameConfig.entityConfig.defFilePaths = {fgd};
  }

  return mdl::makeGameInfo(std::move(gameConfig));
}

/**
 * The Quake directory is only needed for textures, so a configured one that does not
 * exist (for example a path from another machine) is reported and ignored.
 */
std::filesystem::path gamePath(const Config& config, Logger& logger)
{
  if (!config.quakeDir)
  {
    return {};
  }
  if (auto ec = std::error_code{}; !std::filesystem::is_directory(*config.quakeDir, ec))
  {
    logger.warn() << fmt::format(
      "Quake directory {} not found, loading without it", *config.quakeDir);
    return {};
  }
  return *config.quakeDir;
}

Result<std::unique_ptr<mdl::WorldNode>> readWorld(
  const std::string_view bytes,
  const mdl::MapFormat format,
  const mdl::GameConfig& gameConfig,
  ParserStatus& status,
  kdl::task_manager& taskManager)
{
  const auto entityPropertyConfig = mdl::EntityPropertyConfig{
    gameConfig.entityConfig.scaleExpression,
    gameConfig.entityConfig.setDefaultProperties};

  auto reader = mdl::WorldReader{bytes, format, entityPropertyConfig};
  return reader.read(WorldBounds, status, taskManager);
}

/** The format named by the "// Format: <name>" header that editors write, if any. */
std::optional<mdl::MapFormat> headerFormat(const std::string_view bytes)
{
  static const auto pattern = std::regex{R"(// Format: ([^\r\n]+))"};
  const auto head = std::string{bytes.substr(0, 256)};
  if (auto match = std::smatch{}; std::regex_search(head, match, pattern))
  {
    if (
      const auto format = mdl::formatFromName(match[1].str());
      format != mdl::MapFormat::Unknown)
    {
      return format;
    }
  }
  return std::nullopt;
}

/** Returns a format other than Valve 220 that the given map can be read in, if any. */
std::optional<mdl::MapFormat> readableOtherFormat(
  const std::string_view bytes,
  const mdl::GameConfig& gameConfig,
  kdl::task_manager& taskManager)
{
  const auto otherFormats = std::vector<mdl::MapFormat>{
    mdl::MapFormat::Standard,
    mdl::MapFormat::Quake2,
    mdl::MapFormat::Quake2_Valve,
    mdl::MapFormat::Hexen2,
    mdl::MapFormat::Daikatana,
    mdl::MapFormat::Quake3_Legacy,
    mdl::MapFormat::Quake3_Valve,
    mdl::MapFormat::Quake3,
  };

  auto nullLogger = NullLogger{};
  for (const auto format : otherFormats)
  {
    auto status = SimpleParserStatus{nullLogger};
    if (readWorld(bytes, format, gameConfig, status, taskManager))
    {
      return format;
    }
  }
  return std::nullopt;
}

CliError notValve220(const mdl::MapFormat format)
{
  return CliError{
    ErrorCode::NotValve220,
    fmt::format(
      "the map is in {} format; tbcli supports only Valve 220 maps",
      mdl::formatName(format))};
}

} // namespace

LoadedMap::LoadedMap() = default;
LoadedMap::LoadedMap(LoadedMap&&) noexcept = default;
LoadedMap& LoadedMap::operator=(LoadedMap&&) noexcept = default;
LoadedMap::~LoadedMap() = default;

std::string computeRev(const std::string_view bytes)
{
  return sha256Hex(bytes).substr(0, RevLength);
}

LoadedMap loadMap(
  const std::filesystem::path& mapPath, const Config& config, Logger& logger)
{
  const auto absPath = std::filesystem::absolute(mapPath).lexically_normal();
  const auto bytes = readFile(absPath);
  if (!bytes)
  {
    throw CliError{ErrorCode::FileNotFound, fmt::format("cannot read map {}", absPath)};
  }

  if (
    const auto format = headerFormat(*bytes); format && *format != mdl::MapFormat::Valve)
  {
    throw notValve220(*format);
  }

  const auto gameInfo = makeGameInfo(config, logger);

  auto result = LoadedMap{};
  result.rev = computeRev(*bytes);
  result.taskManager = std::make_unique<kdl::task_manager>();
  result.resourceManager = std::make_unique<gl::ResourceManager>();

  logger.info() << "Loading document from " << absPath;

  auto status = SimpleParserStatus{logger};
  auto worldNode = readWorld(
    *bytes, mdl::MapFormat::Valve, gameInfo.gameConfig, status, *result.taskManager);

  if (!worldNode)
  {
    // Faces in other formats never parse as Valve 220, so a map without a format
    // header that fails here may still be valid in another format: refuse it clearly
    // instead of reporting a parse error. Only failed loads pay for this.
    if (
      const auto otherFormat =
        readableOtherFormat(*bytes, gameInfo.gameConfig, *result.taskManager))
    {
      throw notValve220(*otherFormat);
    }

    const auto message = errorMessage(worldNode);
    throw CliError{
      ErrorCode::ParseError,
      fmt::format("cannot parse the map: {}", message),
      std::nullopt,
      lineOf(message)};
  }

  result.map = std::make_unique<mdl::Map>(
    mdl::EnvironmentConfig{},
    gameInfo,
    gamePath(config, logger),
    std::move(worldNode).value(),
    WorldBounds,
    absPath,
    *result.taskManager,
    *result.resourceManager,
    logger);
  result.map->setIsCommandCollationEnabled(false);

  // Process loaded resources without a GL context so that textures get their sizes.
  auto gl = gl::TestGl{};
  gl::processResourcesSync(
    *result.resourceManager, gl::ProcessContext{gl, [](auto, auto) {}});

  return result;
}

} // namespace tb::cli
