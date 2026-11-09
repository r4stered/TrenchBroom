/*
 Copyright (C) 2026 Kristian Duske

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

#include "base/PreferenceManager.h"
#include "gl/FontManager.h"
#include "gl/MockGl.h"
#include "gl/PerspectiveCamera.h"
#include "gl/ShaderManager.h"
#include "gl/TestGl.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "prefs/Preferences.h"
#include "render/RenderContext.h"
#include "ui/MapRendering.h"

#include <algorithm>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("MapRendering")
{
  auto fixture = mdl::MapFixture{};
  auto& map = fixture.create();

  SECTION("setupMapRenderContext")
  {
    auto testGl = gl::TestGl{};
    auto fontManager = gl::FontManager{[](const auto& path) { return path; }};
    auto shaderManager = gl::ShaderManager{[](const auto& path) { return path; }};

    // a camera that does not belong to any map view
    auto camera = gl::PerspectiveCamera{};
    auto renderContext = render::RenderContext{
      testGl, render::RenderMode::Render3D, camera, fontManager, shaderManager};

    SECTION("applies the pixel ratio and the map's grid")
    {
      map.grid().setSize(5);
      map.grid().toggleVisible();

      setupMapRenderContext(renderContext, map, 2.0f);

      CHECK(renderContext.dpiScale() == 2.0f);
      CHECK(renderContext.gridSize() == 32.0);
      CHECK(!renderContext.showGrid());
    }

    SECTION("applies the map view preferences")
    {
      setPref(Preferences::ShowEdges, false);
      setPref(Preferences::FaceRenderMode, Preferences::FaceRenderModeFlat);

      setupMapRenderContext(renderContext, map, 1.0f);

      CHECK(!renderContext.showEdges());
      CHECK(!renderContext.showMaterials());
      CHECK(renderContext.showFaces());

      setPref(Preferences::FaceRenderMode, Preferences::FaceRenderModeSkip);

      setupMapRenderContext(renderContext, map, 1.0f);

      CHECK(!renderContext.showFaces());

      PreferenceManager::instance().resetToDefault(Preferences::ShowEdges);
      PreferenceManager::instance().resetToDefault(Preferences::FaceRenderMode);
    }
  }

  SECTION("setupMapRenderGL")
  {
    auto mockGl = gl::MockGl{};

    auto viewports = std::vector<std::tuple<int, int, int, int>>{};
    auto enabled = std::vector<GLenum>{};
    auto disabled = std::vector<GLenum>{};
    mockGl.onViewport = [&](auto x, auto y, auto w, auto h) {
      viewports.emplace_back(x, y, w, h);
    };
    mockGl.onEnable = [&](auto cap) { enabled.push_back(cap); };
    mockGl.onDisable = [&](auto cap) { disabled.push_back(cap); };
    mockGl.onBlendFunc = [](auto, auto) {};
    mockGl.onShadeModel = [](auto) {};

    auto camera = gl::PerspectiveCamera{};
    camera.setViewport(gl::Camera::Viewport{10, 20, 100, 50});

    SECTION("scales the camera's viewport by the pixel ratio")
    {
      setupMapRenderGL(mockGl, camera, 2.0f);

      CHECK(viewports == std::vector<std::tuple<int, int, int, int>>{{20, 40, 200, 100}});
      CHECK(std::ranges::find(enabled, static_cast<GLenum>(GL_BLEND)) != enabled.end());
    }

    SECTION("enables multisampling if the preference is set")
    {
      setPref(Preferences::EnableMSAA, true);
      setupMapRenderGL(mockGl, camera, 1.0f);

      CHECK(
        std::ranges::find(enabled, static_cast<GLenum>(GL_MULTISAMPLE)) != enabled.end());

      PreferenceManager::instance().resetToDefault(Preferences::EnableMSAA);
    }

    SECTION("disables multisampling if the preference is unset")
    {
      setPref(Preferences::EnableMSAA, false);
      setupMapRenderGL(mockGl, camera, 1.0f);

      CHECK(
        std::ranges::find(disabled, static_cast<GLenum>(GL_MULTISAMPLE))
        != disabled.end());

      PreferenceManager::instance().resetToDefault(Preferences::EnableMSAA);
    }
  }
}

} // namespace tb::ui
