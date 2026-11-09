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

#include <QByteArray>
#include <QColor>
#include <QImage>

#include "cli/CliError.h"
#include "cli/View.h"
#include "gl/OrthographicCamera.h"
#include "gl/PerspectiveCamera.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/Hit.h"
#include "mdl/MapFixture.h"
#include "mdl/PickResult.h"
#include "mdl/TestFactory.h"
#include "ui/MapViewCapture.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

TEST_CASE("MapViewCapture")
{
  // captureMapView needs a visible map view with a GL context, so it is checked by hand
  SECTION("makeViewCapture")
  {
    const auto camera = gl::PerspectiveCamera{
      75.0f,
      1.0f,
      8000.0f,
      {0, 0, 1500, 750},
      vm::vec3f{10, 20, 30},
      vm::vec3f{0, 1, 0},
      vm::vec3f{0, 0, 1}};

    // a view of 1500x750 logical pixels on a screen with two physical pixels per pixel
    auto framebuffer = QImage{3000, 1500, QImage::Format_RGB32};
    framebuffer.fill(QColor{255, 0, 0});
    const auto noPick = mdl::PickResult{};

    SECTION("encodes the framebuffer as PNG, scaled to fit 2048 pixels")
    {
      const auto capture = makeViewCapture(framebuffer, camera, 2.0f, noPick);

      CHECK(capture.image == cli::ImageSize{2048, 1024});
      const auto decoded = QImage::fromData(
        QByteArray{capture.png.data(), qsizetype(capture.png.size())}, "PNG");
      REQUIRE(!decoded.isNull());
      CHECK(decoded.width() == 2048);
      CHECK(decoded.height() == 1024);
      CHECK(decoded.pixelColor(1000, 500) == QColor{255, 0, 0});
    }

    SECTION("keeps a framebuffer that fits as it is")
    {
      const auto small = framebuffer.copy(0, 0, 800, 600);
      const auto capture = makeViewCapture(small, camera, 1.0f, noPick);
      CHECK(capture.image == cli::ImageSize{800, 600});
    }

    SECTION("reports the view's camera and pixel ratio")
    {
      const auto capture = makeViewCapture(framebuffer, camera, 2.0f, noPick);
      CHECK(capture.camera.projection == cli::CameraPose::Projection::Perspective);
      CHECK(capture.camera.position == vm::vec3f{10, 20, 30});
      CHECK(capture.camera.direction == vm::vec3f{0, 1, 0});
      CHECK(capture.camera.fov == 75.0f);
      CHECK(capture.camera.viewport == cli::ImageSize{1500, 750});
      CHECK(capture.pixelRatio == 2.0f);
      CHECK(capture.hovered == std::nullopt);

      const auto topDown = gl::OrthographicCamera{
        1.0f,
        8000.0f,
        {0, 0, 1500, 750},
        vm::vec3f{0, 0, 100},
        vm::vec3f{0, 0, -1},
        vm::vec3f{0, 1, 0}};
      const auto orthoCapture = makeViewCapture(framebuffer, topDown, 2.0f, noPick);
      CHECK(orthoCapture.camera.projection == cli::CameraPose::Projection::Orthographic);
      CHECK(cli::viewType(orthoCapture.camera) == "xy");
    }

    SECTION("reports the node and face under the mouse")
    {
      auto fixture = mdl::MapFixture{};
      auto& map = fixture.create();
      auto brushNode = mdl::createBrushNode(map);
      auto entityNode = mdl::EntityNode{mdl::Entity{}};

      auto pickResult = mdl::PickResult{};
      pickResult.addHit(
        mdl::Hit{
          mdl::BrushNode::BrushHitType,
          10.0,
          vm::vec3d{1, 2, 3},
          mdl::BrushFaceHandle{*brushNode, 2}});
      pickResult.addHit(
        mdl::Hit{mdl::EntityNode::EntityHitType, 20.0, vm::vec3d{4, 5, 6}, &entityNode});

      const auto capture = makeViewCapture(framebuffer, camera, 1.0f, pickResult);
      REQUIRE(capture.hovered);
      CHECK(capture.hovered->node == brushNode.get());
      CHECK(capture.hovered->faceIndex == 2);
      CHECK(capture.hovered->point == vm::vec3d{1, 2, 3});

      auto entityPick = mdl::PickResult{};
      entityPick.addHit(
        mdl::Hit{mdl::EntityNode::EntityHitType, 20.0, vm::vec3d{4, 5, 6}, &entityNode});
      const auto entityCapture = makeViewCapture(framebuffer, camera, 1.0f, entityPick);
      REQUIRE(entityCapture.hovered);
      CHECK(entityCapture.hovered->node == &entityNode);
      CHECK(entityCapture.hovered->faceIndex == std::nullopt);
    }

    SECTION("an empty framebuffer gives view_failed")
    {
      try
      {
        makeViewCapture(QImage{}, camera, 1.0f, noPick);
        FAIL("expected a CliError");
      }
      catch (const cli::CliError& e)
      {
        CHECK(e.code() == cli::ErrorCode::ViewFailed);
      }
    }
  }
}

} // namespace tb::ui
