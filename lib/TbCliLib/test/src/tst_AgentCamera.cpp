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

#include "CliTestUtils.h"
#include "FakeEditorHost.h"
#include "cli/AgentCamera.h"
#include "cli/CliError.h"
#include "cli/View.h"
#include "gl/Camera.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Selection.h"
#include "mdl/WorldNode.h"

#include "vm/vec.h"

#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{

TEST_CASE("AgentCamera")
{
  const auto worldBounds = vm::bbox3d{4096.0};
  const auto size = ImageSize{1024, 768};

  SECTION("parseCameraRequest")
  {
    // no camera options: the mapper's view
    CHECK(parseCameraRequest({}) == std::nullopt);

    // a subject alone orbits it at the default angles, at the default size
    CHECK(
      parseCameraRequest({.lookAt = "n:1.2"})
      == CameraRequest{.subject = LookAt{"n:1.2"}, .placement = Orbit{}});
    CHECK(
      parseCameraRequest({.selection = true})
      == CameraRequest{.subject = SelectedNodes{}});

    // a placement alone fits the whole map
    CHECK(
      parseCameraRequest({.axisViews = {AxisView::Side}})
      == CameraRequest{.subject = WholeMap{}, .placement = AxisView::Side});

    CHECK(
      parseCameraRequest(
        {.frame = "0 0 0 64 64 64", .orbit = "90,-10,300", .size = "640x480"})
      == CameraRequest{
        .subject = Frame{vm::bbox3d{{0, 0, 0}, {64, 64, 64}}},
        .placement = Orbit{.yaw = 90, .pitch = -10, .distance = 300},
        .size = {640, 480}});
    CHECK(
      parseCameraRequest({.from = "1,2,3", .toward = "4,5,6"})
      == CameraRequest{.placement = FromToward{{1, 2, 3}, {4, 5, 6}}});

    // fitted into 2048 pixels
    CHECK(
      parseCameraRequest({.axisViews = {AxisView::Top}, .size = "8192x2048"})->size
      == ImageSize{2048, 512});

    // contradictions
    CHECK_THROWS_AS(
      parseCameraRequest({.axisViews = {AxisView::Top, AxisView::Front}}), CliError);
    CHECK_THROWS_AS(parseCameraRequest({.size = "640x480"}), CliError);
    CHECK_THROWS_AS(parseCameraRequest({.from = "1,2,3"}), CliError);
  }

  SECTION("subjectBounds")
  {
    auto fixture = EditFixture{};
    auto editor = FakeEditorHost{fixture.env.path(fixture.fileName)};
    auto& map = editor.map();
    auto* brush1 =
      dynamic_cast<mdl::BrushNode*>(map.worldNode().defaultLayer()->children()[1]);
    REQUIRE(brush1 != nullptr);

    // brush 0 spans -64..64 on x and y and -16..0 on z
    const auto brush0Bounds = vm::bbox3d{{-64, -64, -16}, {64, 64, 0}};
    CHECK(subjectBounds(map, LookAt{"layer:0/brush:0"}, nullptr) == brush0Bounds);
    CHECK(
      subjectBounds(map, LookAt{editor.handles().handle(*brush1)}, &editor.handles())
      == vm::bbox3d{{-64, -64, 0}, {-48, 64, 128}});
    // brush 1's bottom face
    CHECK(
      subjectBounds(map, LookAt{"layer:0/brush:1/face:2"}, nullptr)
      == vm::bbox3d{{-64, -64, 0}, {-48, 64, 0}});

    const auto frame = vm::bbox3d{{1, 2, 3}, {4, 5, 6}};
    CHECK(subjectBounds(map, Frame{frame}, nullptr) == frame);

    // the whole map holds both brushes
    const auto whole = subjectBounds(map, WholeMap{}, nullptr);
    CHECK(whole.contains(brush0Bounds));

    CHECK_THROWS_AS(subjectBounds(map, SelectedNodes{}, nullptr), CliError);
    mdl::selectBrushFaces(map, {mdl::BrushFaceHandle{*brush1, 2}});
    CHECK(
      subjectBounds(map, SelectedNodes{}, nullptr)
      == vm::bbox3d{{-64, -64, 0}, {-48, 64, 0}});
    mdl::deselectAll(map);
    mdl::selectNodes(map, {brush1});
    CHECK(
      subjectBounds(map, SelectedNodes{}, nullptr)
      == vm::bbox3d{{-64, -64, 0}, {-48, 64, 128}});
  }

  SECTION("resolveCameraPose")
  {
    SECTION("from a point toward another")
    {
      const auto pose =
        resolveCameraPose(FromToward{{0, 0, 64}, {100, 0, 64}}, {}, worldBounds, size);
      CHECK(pose.projection == CameraPose::Projection::Perspective);
      CHECK(pose.position == vm::vec3f{0, 0, 64});
      CHECK(pose.direction == vm::vec3f{1, 0, 0});
      CHECK(pose.up == vm::vec3f{0, 0, 1});
      CHECK(pose.zoom == 1.0f);
      CHECK(pose.viewport == size);

      // looking straight down, the top of the image is +Y, as in the top view
      const auto down =
        resolveCameraPose(FromToward{{0, 0, 512}, {0, 0, 0}}, {}, worldBounds, size);
      CHECK(down.direction == vm::vec3f{0, 0, -1});
      CHECK(down.up == vm::vec3f{0, 1, 0});
    }

    // a 128 unit wide, 32 unit tall box around the origin
    const auto subject = vm::bbox3d{{-64, -64, -16}, {64, 64, 16}};

    // whether the subject is in the image and fills at least the given part of the
    // image's width or height
    const auto fits = [&](const CameraPose& pose, const vm::bbox3d& bounds, double fill) {
      const auto box = projectBounds(pose, size, bounds);
      if (!box)
      {
        return false;
      }
      const auto width = double(box->right - box->left) / double(size.width);
      const auto height = double(box->bottom - box->top) / double(size.height);
      return box->left > 0 && box->top > 0 && box->right < size.width
             && box->bottom < size.height && (width >= fill || height >= fill);
    };

    SECTION("top, front and side views")
    {
      const auto top = resolveCameraPose(AxisView::Top, subject, worldBounds, size);
      CHECK(top.projection == CameraPose::Projection::Orthographic);
      CHECK(top.direction == vm::vec3f{0, 0, -1});
      CHECK(top.up == vm::vec3f{0, 1, 0});
      // above the world, so that the grid at the world's bottom shows
      CHECK(top.position == vm::vec3f{0, 0, 4096});
      CHECK(top.farPlane > 8192.0f);
      CHECK(top.viewport == size);
      CHECK(viewType(top) == "xy");
      CHECK(fits(top, subject, 0.75));

      const auto front = resolveCameraPose(AxisView::Front, subject, worldBounds, size);
      CHECK(front.direction == vm::vec3f{0, 1, 0});
      CHECK(front.up == vm::vec3f{0, 0, 1});
      CHECK(front.position == vm::vec3f{0, -4096, 0});
      CHECK(viewType(front) == "xz");
      CHECK(fits(front, subject, 0.75));

      const auto side = resolveCameraPose(AxisView::Side, subject, worldBounds, size);
      CHECK(side.direction == vm::vec3f{-1, 0, 0});
      CHECK(side.up == vm::vec3f{0, 0, 1});
      CHECK(side.position == vm::vec3f{4096, 0, 0});
      CHECK(viewType(side) == "yz");
      CHECK(fits(side, subject, 0.75));

      // centered on the subject
      const auto offCenter = vm::bbox3d{{100, 200, 0}, {300, 400, 64}};
      const auto topOffCenter =
        resolveCameraPose(AxisView::Top, offCenter, worldBounds, size);
      CHECK(topOffCenter.position == vm::vec3f{200, 300, 4096});
      CHECK(fits(topOffCenter, offCenter, 0.75));
    }

    SECTION("the zoom of top, front and side views stays in the editor's range")
    {
      const auto tiny = vm::bbox3d{{0, 0, 0}, {0.5, 0.5, 0.5}};
      CHECK(resolveCameraPose(AxisView::Top, tiny, worldBounds, size).zoom <= 100.0f);
      CHECK(
        resolveCameraPose(AxisView::Top, worldBounds, worldBounds, size).zoom >= 0.02f);
    }

    SECTION("orbit")
    {
      // southwest of the subject and above it, looking at its center
      const auto pose = resolveCameraPose(Orbit{}, subject, worldBounds, size);
      CHECK(pose.projection == CameraPose::Projection::Perspective);
      CHECK(pose.position.x() < 0.0f);
      CHECK(pose.position.y() < 0.0f);
      CHECK(pose.position.z() > 0.0f);
      CHECK(
        vm::is_equal(
          vm::normalize(-pose.position),
          pose.direction,
          vm::constants<float>::almost_zero()));
      CHECK(pose.up.z() > 0.0f);
      CHECK(fits(pose, subject, 0.5));

      // at the given angles and distance from the center
      const auto east = resolveCameraPose(
        Orbit{.yaw = 0, .pitch = 0, .distance = 500}, subject, worldBounds, size);
      CHECK(vm::is_equal(east.position, vm::vec3f{500, 0, 0}, 0.001f));
      CHECK(vm::is_equal(east.direction, vm::vec3f{-1, 0, 0}, 0.001f));
      CHECK(vm::is_equal(east.up, vm::vec3f{0, 0, 1}, 0.001f));

      const auto north = resolveCameraPose(
        Orbit{.yaw = 90, .pitch = 45, .distance = 100}, subject, worldBounds, size);
      const auto n = float(std::sqrt(0.5) * 100.0);
      CHECK(vm::is_equal(north.position, vm::vec3f{0, n, n}, 0.001f));

      // straight down from above, with +Y up as in the top view
      const auto above = resolveCameraPose(
        Orbit{.yaw = 0, .pitch = 90, .distance = 100}, subject, worldBounds, size);
      CHECK(vm::is_equal(above.position, vm::vec3f{0, 0, 100}, 0.001f));
      CHECK(vm::is_equal(above.direction, vm::vec3f{0, 0, -1}, 0.001f));
      CHECK(vm::is_equal(above.up, vm::vec3f{0, 1, 0}, 0.001f));

      // a large subject is fitted too, and the far plane reaches past it
      const auto large = vm::bbox3d{{-3000, -3000, -500}, {3000, 3000, 500}};
      const auto largePose = resolveCameraPose(Orbit{}, large, worldBounds, size);
      CHECK(fits(largePose, large, 0.5));
      CHECK(
        largePose.farPlane
        > float(vm::length(vm::vec3d{largePose.position}) + vm::length(large.size())));

      // a point is fitted with a margin around it
      const auto point = vm::bbox3d{{10, 20, 30}, {10, 20, 30}};
      const auto pointPose = resolveCameraPose(Orbit{}, point, worldBounds, size);
      CHECK(vm::length(pointPose.position - vm::vec3f{10, 20, 30}) > 1.0f);
      CHECK(fits(pointPose, point.expand(1.0), 0.0));
    }

    SECTION("the result makes a camera with the same pose")
    {
      for (const auto& placement : std::vector<CameraPlacement>{
             Orbit{}, AxisView::Top, AxisView::Side, FromToward{{1, 2, 3}, {4, 5, 6}}})
      {
        const auto pose = resolveCameraPose(placement, subject, worldBounds, size);
        CHECK(cameraPose(*makeCamera(pose)).zoom == pose.zoom);
        CHECK(cameraPose(*makeCamera(pose)).position == pose.position);
      }
    }
  }
}

} // namespace tb::cli
