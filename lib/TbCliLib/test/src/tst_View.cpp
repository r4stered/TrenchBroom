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

#include "cli/CliError.h"
#include "cli/FileUtils.h"
#include "cli/View.h"
#include "gl/OrthographicCamera.h"
#include "gl/PerspectiveCamera.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{

TEST_CASE("View")
{
  SECTION("fitImageSize")
  {
    CHECK(fitImageSize({800, 600}) == ImageSize{800, 600});
    CHECK(fitImageSize({2048, 2048}) == ImageSize{2048, 2048});
    CHECK(fitImageSize({4096, 1024}) == ImageSize{2048, 512});
    CHECK(fitImageSize({1000, 3000}) == ImageSize{683, 2048});
    CHECK(fitImageSize({400, 100}, 200) == ImageSize{200, 50});
  }

  const auto perspective = gl::PerspectiveCamera{
    90.0f,
    1.0f,
    1000.0f,
    {0, 0, 800, 600},
    vm::vec3f{0, 0, 0},
    vm::vec3f{1, 0, 0},
    vm::vec3f{0, 0, 1}};

  // looks down at the XY plane from above; one unit is one pixel at zoom 1
  const auto makeTopDown = [](const float zoom) {
    auto camera = std::make_unique<gl::OrthographicCamera>(
      1.0f,
      1000.0f,
      gl::Camera::Viewport{0, 0, 800, 600},
      vm::vec3f{0, 0, 100},
      vm::vec3f{0, 0, -1},
      vm::vec3f{0, 1, 0});
    camera->setZoom(zoom);
    return camera;
  };
  const auto topDownCamera = makeTopDown(1.0f);
  const auto& topDown = *topDownCamera;

  SECTION("cameraPose and makeCamera")
  {
    const auto pose = cameraPose(perspective);
    CHECK(pose.projection == CameraPose::Projection::Perspective);
    CHECK(pose.position == vm::vec3f{0, 0, 0});
    CHECK(pose.direction == vm::vec3f{1, 0, 0});
    CHECK(pose.up == vm::vec3f{0, 0, 1});
    CHECK(pose.fov == 90.0f);
    CHECK(pose.zoom == 1.0f);
    CHECK(pose.viewport == ImageSize{800, 600});

    const auto point = vm::vec3f{100, 20, -30};
    CHECK(makeCamera(pose)->project(point) == perspective.project(point));

    const auto zoomed = makeTopDown(2.0f);
    const auto orthoPose = cameraPose(*zoomed);
    CHECK(orthoPose.projection == CameraPose::Projection::Orthographic);
    CHECK(orthoPose.zoom == 2.0f);
    CHECK(makeCamera(orthoPose)->project(point) == zoomed->project(point));
  }

  SECTION("viewType")
  {
    CHECK(viewType(cameraPose(perspective)) == "3d");
    CHECK(viewType(cameraPose(topDown)) == "xy");

    auto pose = cameraPose(topDown);
    pose.direction = {0, 1, 0};
    CHECK(viewType(pose) == "xz");
    pose.direction = {-1, 0, 0};
    CHECK(viewType(pose) == "yz");
  }

  SECTION("toJson")
  {
    const auto json = toJson(cameraPose(perspective));
    CHECK(json["projection"] == Json{"perspective"});
    CHECK(json["position"] == Json{Json::Array{0, 0, 0}});
    CHECK(json["direction"] == Json{Json::Array{1, 0, 0}});
    CHECK(json["up"] == Json{Json::Array{0, 0, 1}});
    CHECK(json["fov"] == Json{90});
    CHECK(json["zoom"] == Json{1});
    CHECK(json["viewport"] == Json{Json::Array{800, 600}});

    const auto orthoJson = toJson(cameraPose(topDown));
    CHECK(orthoJson["projection"] == Json{"orthographic"});
    CHECK(orthoJson.find("fov") == nullptr);
  }

  SECTION("projectBounds")
  {
    const auto pose = cameraPose(perspective);
    const auto image = ImageSize{800, 600};

    SECTION("a box ahead of the camera is centered in the image")
    {
      const auto box =
        projectBounds(pose, image, vm::bbox3d{{100, -10, -10}, {120, 10, 10}});
      REQUIRE(box);
      CHECK(box->left + box->right == 800);
      CHECK(box->top + box->bottom == 600);
      CHECK(box->left < 400);
      CHECK(box->top < 300);
    }

    SECTION("image y goes down, and the camera's left is +y")
    {
      const auto above =
        projectBounds(pose, image, vm::bbox3d{{100, -1, 20}, {100, 1, 30}});
      REQUIRE(above);
      CHECK(above->bottom < 300);

      const auto left =
        projectBounds(pose, image, vm::bbox3d{{100, 20, -1}, {100, 30, 1}});
      REQUIRE(left);
      CHECK(left->right < 400);
    }

    SECTION("a larger image scales the box")
    {
      const auto bounds = vm::bbox3d{{100, -10, -10}, {120, 10, 10}};
      const auto box = projectBounds(pose, image, bounds);
      const auto box2 = projectBounds(pose, {1600, 1200}, bounds);
      REQUIRE(box);
      REQUIRE(box2);
      // give or take the pixel that the box is rounded out to
      CHECK(std::abs(box2->left - box->left * 2) <= 1);
      CHECK(std::abs(box2->bottom - box->bottom * 2) <= 1);
    }

    SECTION("a box behind the camera is not in the image")
    {
      CHECK(
        projectBounds(pose, image, vm::bbox3d{{-120, -10, -10}, {-100, 10, 10}})
        == std::nullopt);
    }

    SECTION("a box off to the side is not in the image")
    {
      CHECK(
        projectBounds(pose, image, vm::bbox3d{{100, 500, -10}, {120, 520, 10}})
        == std::nullopt);
    }

    SECTION("a box around the camera covers the image")
    {
      CHECK(
        projectBounds(pose, image, vm::bbox3d{{-10, -10, -10}, {10, 10, 10}})
        == ImageBox{0, 0, 800, 600});
    }

    SECTION("a box partly behind the camera is clipped at the near plane")
    {
      // reaches from behind the camera to ahead of it, below and right of the view axis
      const auto box =
        projectBounds(pose, image, vm::bbox3d{{-100, -100, -50}, {100, -50, -20}});
      REQUIRE(box);
      CHECK(box->right == 800);
      CHECK(box->bottom == 600);
      CHECK(box->left > 400);
      CHECK(box->top > 300);
    }

    SECTION("orthographic")
    {
      const auto box =
        projectBounds(cameraPose(topDown), image, vm::bbox3d{{0, 0, 0}, {100, 50, 10}});
      CHECK(box == ImageBox{400, 250, 500, 300});

      CHECK(
        projectBounds(
          cameraPose(*makeTopDown(2.0f)), image, vm::bbox3d{{0, 0, 0}, {100, 50, 10}})
        == ImageBox{400, 200, 600, 300});
    }
  }

  SECTION("projectPoints")
  {
    const auto pose = cameraPose(perspective);
    const auto image = ImageSize{800, 600};

    // a triangle ahead of the camera, as a selected face's vertices
    const auto box =
      projectPoints(pose, image, {{100, 0, 0}, {100, -100, 0}, {100, 0, -75}});
    // one corner at the center; to the right is -y and down is -z
    CHECK(box == ImageBox{400, 300, 800, 600});

    CHECK(projectPoints(pose, image, {}) == std::nullopt);
    CHECK(projectPoints(pose, image, {{-100, 0, 0}}) == std::nullopt);
  }
}

TEST_CASE("ViewImages")
{
  auto temp = TemporaryDirectory{"tbcli-view-test-"};
  const auto dir = temp.path() / "views";

  SECTION("creates its directory with the first image and keeps the latest images")
  {
    {
      auto images = ViewImages{dir, 3};
      CHECK(images.dir() == dir);
      CHECK(!std::filesystem::exists(dir));

      auto paths = std::vector<std::filesystem::path>{};
      for (int i = 0; i < 5; ++i)
      {
        paths.push_back(images.add("png " + std::to_string(i)));
      }

      CHECK(paths[0] != paths[1]);
      CHECK(paths[4].parent_path() == dir);
      CHECK(paths[4].extension() == ".png");
      CHECK(readFile(paths[4]) == "png 4");
      CHECK(!std::filesystem::exists(paths[0]));
      CHECK(!std::filesystem::exists(paths[1]));
      CHECK(std::filesystem::exists(paths[2]));

      auto count = 0;
      for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator{dir})
      {
        ++count;
      }
      CHECK(count == 3);
    }

    // deleted with the images
    CHECK(!std::filesystem::exists(dir));
  }

  SECTION("a failed write gives write_failed")
  {
    writeFile(temp.path() / "file", "not a directory");
    auto images = ViewImages{temp.path() / "file" / "views"};
    CHECK_THROWS_AS(images.add("png"), CliError);
  }
}

} // namespace tb::cli
