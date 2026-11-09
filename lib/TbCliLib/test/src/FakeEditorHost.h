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

#include "cli/FileUtils.h"
#include "cli/LiveSession.h"
#include "cli/NodeHandles.h"
#include "cli/Run.h"
#include "cli/View.h"
#include "mdl/MapFixture.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::cli
{

/**
 * An editor that has a map open, in-process: the live session that the editor host
 * provides, on top of a MapFixture. Like the editor, it keeps command collation on.
 */
class FakeEditorHost : public LiveSession
{
private:
  mdl::MapFixture m_fixture;
  mdl::Map& m_map;
  LiveRev m_rev;
  NodeHandleTable m_handles;
  AgentHistory m_agentHistory;
  std::optional<ViewCapture> m_mapperView;
  TemporaryDirectory m_viewDir;
  ViewImages m_viewImages;

public:
  /**
   * Opens the map file at the given absolute path, as the editor would, with a change
   * log that keeps the given number of changes.
   */
  explicit FakeEditorHost(
    const std::filesystem::path& mapPath,
    size_t changeLogCapacity = ChangeLog::DefaultCapacity);
  ~FakeEditorHost() override;

  mdl::Map& map() override;
  std::string epoch() const override;
  std::string rev() const override;
  NodeHandleTable& handles() override;
  AgentHistory& agentHistory() override;
  ChangeLog& changeLog() override;
  ViewCapture captureMapperView() override;
  ViewImages& viewImages() override;

  /**
   * Sets what captureMapperView returns, or makes it fail with `view_failed`. By
   * default, it returns a 4x3 PNG of a perspective view.
   */
  void setMapperView(std::optional<ViewCapture> view);

  /** Runs tbcli against this editor's map. Paths are passed as UTF-8 strings. */
  RunResult run(const std::vector<std::string>& args, std::string_view in = {});
};

} // namespace tb::cli
