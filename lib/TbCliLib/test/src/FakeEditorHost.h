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

#include <chrono>
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
  bool m_agentViewsFail = false;
  TemporaryDirectory m_viewDir;
  ViewImages m_viewImages;
  std::optional<ShownView> m_shownView;

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
  /**
   * Returns a fake PNG of the pose's viewport size, or fails with `view_failed` if the
   * last setAgentViewsFail asked for it.
   */
  ViewCapture renderAgentView(const CameraPose& pose) override;
  ViewImages& viewImages() override;
  const ShownView* shownView() const override;
  std::optional<ShownView> takeShownView() override;

  /**
   * Sets what captureMapperView returns, or makes it fail with `view_failed`. By
   * default, it returns a 4x3 PNG of a perspective view.
   */
  void setMapperView(std::optional<ViewCapture> view);

  /** Makes renderAgentView fail with `view_failed`, or succeed again. */
  void setAgentViewsFail(bool fail);

  /**
   * Shows the agent the given capture of a view at the given time, as the mapper's
   * "Show to agent" does; it replaces a view shown before.
   */
  void show(
    const ViewCapture& view,
    std::chrono::steady_clock::time_point shownAt = std::chrono::steady_clock::now());

  /** Runs tbcli against this editor's map. Paths are passed as UTF-8 strings. */
  RunResult run(const std::vector<std::string>& args, std::string_view in = {});
};

} // namespace tb::cli
