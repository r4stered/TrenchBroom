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

#include "FakeEditorHost.h"

#include "CliTestUtils.h"
#include "cli/CliError.h"
#include "cli/FileUtils.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/ParseGameConfig.h"

#include "kd/contracts.h"
#include "kd/result.h"

namespace tb::cli
{
namespace
{

/**
 * The Quake game config and FGD shipped with TrenchBroom, as tbcli loads them in file
 * mode with quakeConfig(), and no game path, so that materials aren't checked.
 */
mdl::MapFixtureConfig quakeEditorConfig()
{
  const auto contents = readFile(quakeGameConfigPath());
  contract_assert(contents.has_value());

  auto gameConfig = mdl::parseGameConfig(*contents, quakeGameConfigPath()) | kdl::value();
  gameConfig.entityConfig.defFilePaths = {quakeFgdPath()};
  return mdl::MapFixtureConfig{
    .mapFormat = mdl::MapFormat::Valve,
    .gameInfo = mdl::makeGameInfo(std::move(gameConfig)),
  };
}

/** Stands in for the PNG of a captured 4x3 view, which tbcli never decodes. */
constexpr auto TinyPng = std::string_view{"PNG of a fake view"};

ViewCapture defaultMapperView()
{
  return ViewCapture{
    .png = std::string{TinyPng},
    .image = {4, 3},
    .camera = CameraPose{.viewport = {4, 3}},
  };
}

} // namespace

FakeEditorHost::FakeEditorHost(
  const std::filesystem::path& mapPath, const size_t changeLogCapacity)
  : m_map{m_fixture.load(mapPath, quakeEditorConfig())}
  , m_rev{m_map, changeLogCapacity}
  , m_handles{m_map, m_rev.epoch()}
  , m_mapperView{defaultMapperView()}
  , m_viewDir{"tbcli-fake-editor-"}
  , m_viewImages{m_viewDir.path() / "views"}
{
  // MapFixture turns collation off, the editor has it on
  m_map.setIsCommandCollationEnabled(true);
}

FakeEditorHost::~FakeEditorHost() = default;

mdl::Map& FakeEditorHost::map()
{
  return m_map;
}

std::string FakeEditorHost::epoch() const
{
  return m_rev.epoch();
}

std::string FakeEditorHost::rev() const
{
  return m_rev.str();
}

NodeHandleTable& FakeEditorHost::handles()
{
  return m_handles;
}

AgentHistory& FakeEditorHost::agentHistory()
{
  return m_agentHistory;
}

ChangeLog& FakeEditorHost::changeLog()
{
  return m_rev.changeLog();
}

ViewCapture FakeEditorHost::captureMapperView()
{
  if (!m_mapperView)
  {
    throw CliError{ErrorCode::ViewFailed, "the fake editor has no map view"};
  }
  return *m_mapperView;
}

ViewImages& FakeEditorHost::viewImages()
{
  return m_viewImages;
}

void FakeEditorHost::setMapperView(std::optional<ViewCapture> view)
{
  m_mapperView = std::move(view);
}

RunResult FakeEditorHost::run(
  const std::vector<std::string>& args, const std::string_view in)
{
  return cli::run(args, in, *this);
}

} // namespace tb::cli
