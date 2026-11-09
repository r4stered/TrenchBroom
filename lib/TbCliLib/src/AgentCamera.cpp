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

#include "cli/AgentCamera.h"

#include "cli/CliError.h"
#include "cli/NodeHandles.h"
#include "cli/ParseUtils.h"
#include "cli/Query.h"
#include "gl/Camera.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/Map.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

#include "vm/intersection.h"
#include "vm/plane.h"
#include "vm/ray.h"
#include "vm/scalar.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

namespace tb::cli
{
namespace
{

constexpr auto SubjectOptions = std::string_view{"--look-at, --frame and --selection"};
constexpr auto PlacementOptions =
  std::string_view{"--orbit, --from and --toward, --top, --front and --side"};

/**
 * The finite numbers, from `minCount` to `maxCount` of them, that an option's value
 * lists, separated by commas or spaces, as described.
 */
std::vector<double> parseNumbers(
  const std::string_view name,
  const std::string& value,
  const size_t minCount,
  const size_t maxCount,
  const std::string_view description)
{
  const auto numbers = parseNumberList(value);
  if (
    !numbers || numbers->size() < minCount || numbers->size() > maxCount
    || !std::ranges::all_of(*numbers, [](const double n) { return std::isfinite(n); }))
  {
    throw invalidArgument(
      fmt::format("{} requires {}, not '{}'", name, description, value));
  }
  return *numbers;
}

vm::vec3d parsePoint(const std::string_view name, const std::string& value)
{
  const auto numbers = parseNumbers(name, value, 3, 3, "a point x,y,z");
  return {numbers[0], numbers[1], numbers[2]};
}

vm::bbox3d parseFrame(const std::string& value)
{
  const auto numbers = parseNumbers("--frame", value, 6, 6, "a box x1,y1,z1,x2,y2,z2");
  const auto min = vm::vec3d{numbers[0], numbers[1], numbers[2]};
  const auto max = vm::vec3d{numbers[3], numbers[4], numbers[5]};
  if (!vm::bbox3d::is_valid(min, max))
  {
    throw invalidArgument(
      fmt::format(
        "--frame: the first corner of '{}' must be the minimum on every axis", value));
  }
  return {min, max};
}

Orbit parseOrbit(const std::string& value)
{
  const auto values =
    parseNumbers("--orbit", value, 2, 3, "yaw,pitch or yaw,pitch,distance");
  if (values[1] < -90.0 || values[1] > 90.0)
  {
    throw invalidArgument(
      fmt::format("--orbit: the pitch must be from -90 to 90, not {}", values[1]));
  }
  if (values.size() == 3 && values[2] <= 0.0)
  {
    throw invalidArgument(
      fmt::format("--orbit: the distance must be positive, not {}", values[2]));
  }
  return Orbit{
    .yaw = values[0],
    .pitch = values[1],
    .distance = values.size() == 3 ? std::optional{values[2]} : std::nullopt,
  };
}

ImageSize parseSize(const std::string& value)
{
  const auto x = value.find_first_of("xX");
  const auto width =
    x != std::string::npos ? parseUnsigned(value.substr(0, x)) : std::nullopt;
  const auto height =
    x != std::string::npos ? parseUnsigned(value.substr(x + 1)) : std::nullopt;
  if (!width || !height || *width == 0 || *height == 0)
  {
    throw invalidArgument(
      fmt::format(
        "--size requires a size WxH in pixels, such as 1024x768, not '{}'", value));
  }
  // fitted into MaxViewImageSize next, so larger sizes need only keep their aspect
  const auto clamp = [](const size_t n) { return int(std::min(n, size_t{1'000'000})); };
  return fitImageSize({clamp(*width), clamp(*height)});
}

/**
 * The up vector of a camera that looks in the given direction: as close to +Z as it can
 * be, or +Y for a camera that looks straight up or down, as in the editor's top view.
 */
vm::vec3d upVector(const vm::vec3d& direction)
{
  auto right = vm::cross(direction, vm::vec3d{0, 0, 1});
  if (vm::is_zero(right, vm::Cd::almost_zero()))
  {
    right = vm::cross(direction, vm::vec3d{0, 1, 0});
  }
  return vm::normalize(vm::cross(vm::normalize(right), direction));
}

CameraPose perspectivePose(
  const vm::vec3d& position, const vm::vec3d& direction, const ImageSize size)
{
  return CameraPose{
    .projection = CameraPose::Projection::Perspective,
    .position = vm::vec3f{position},
    .direction = vm::vec3f{direction},
    .up = vm::vec3f{upVector(direction)},
    .viewport = size,
  };
}

/**
 * How far around its subject a camera that fits it shows, so that the subject does not
 * touch the image's edges and a point still has some surroundings.
 */
double fitMargin(const vm::bbox3d& subject)
{
  return std::max(16.0, 0.05 * vm::length(subject.size()));
}

/**
 * How far back from the subject's center a perspective camera that looks in the given
 * direction must be for the subject to fit into its view: like the editor's "focus on
 * selection", the camera is put at the center and then moved back until every corner of
 * the subject, pushed out by the margin, is inside every side of its frustum.
 */
double fitDistance(
  const vm::bbox3d& subject, const vm::vec3d& direction, const ImageSize size)
{
  const auto camera = makeCamera(perspectivePose(subject.center(), direction, size));
  vm::plane3f frustumPlanes[4];
  camera->frustumPlanes(
    frustumPlanes[0], frustumPlanes[1], frustumPlanes[2], frustumPlanes[3]);

  const auto margin = float(fitMargin(subject));
  const auto backward = vm::ray3f{camera->position(), -camera->direction()};
  auto distance = 0.0f;
  subject.for_each_vertex([&](const vm::vec3d& corner) {
    for (const auto& plane : frustumPlanes)
    {
      const auto pushedOut =
        vm::plane3f{vm::vec3f{corner} + margin * plane.normal, plane.normal};
      if (const auto d = vm::intersect_ray_plane(backward, pushedOut); d && *d > 0.0f)
      {
        distance = std::max(distance, *d);
      }
    }
  });
  return double(distance);
}

CameraPose orbitPose(const Orbit& orbit, const vm::bbox3d& subject, const ImageSize size)
{
  const auto yaw = vm::to_radians(orbit.yaw);
  const auto pitch = vm::to_radians(orbit.pitch);
  // from the camera toward the center
  const auto direction = -vm::vec3d{
    std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
  const auto distance =
    orbit.distance ? *orbit.distance : fitDistance(subject, direction, size);

  auto pose = perspectivePose(subject.center() - direction * distance, direction, size);
  pose.farPlane = std::max(
    pose.farPlane, float(distance + vm::length(subject.size()) + fitMargin(subject)));
  return pose;
}

/**
 * An orthographic camera that looks along an axis as the editor's 2D view of the same
 * plane does, from the side of the world bounds it looks from, so that it shows the grid
 * at their far side, centered on the subject and zoomed to fit it.
 */
CameraPose axisPose(
  const AxisView axisView,
  const vm::bbox3d& subject,
  const vm::bbox3d& worldBounds,
  const ImageSize size)
{
  const auto center = subject.center();
  auto direction = vm::vec3d{};
  auto up = vm::vec3d{0, 0, 1};
  auto position = center;
  switch (axisView)
  {
  case AxisView::Top:
    direction = {0, 0, -1};
    up = {0, 1, 0};
    position[2] = worldBounds.max.z();
    break;
  case AxisView::Front:
    direction = {0, 1, 0};
    position[1] = worldBounds.min.y();
    break;
  case AxisView::Side:
    direction = {-1, 0, 0};
    position[0] = worldBounds.max.x();
    break;
  }

  const auto right = vm::cross(direction, up);
  const auto margin = 2.0 * fitMargin(subject);
  const auto width = std::abs(vm::dot(subject.size(), right)) + margin;
  const auto height = std::abs(vm::dot(subject.size(), up)) + margin;
  const auto zoom = std::min(double(size.width) / width, double(size.height) / height);
  const auto depth = std::abs(vm::dot(worldBounds.size(), direction));

  return CameraPose{
    .projection = CameraPose::Projection::Orthographic,
    .position = vm::vec3f{position},
    .direction = vm::vec3f{direction},
    .up = vm::vec3f{up},
    // the editor's 2D views can zoom this far
    .zoom = float(std::clamp(zoom, 0.02, 100.0)),
    .nearPlane = 1.0f,
    // the grid is at the far side of the world bounds, keep it off the far plane
    .farPlane = float(depth + 16.0),
    .viewport = size,
  };
}

} // namespace

std::optional<CameraRequest> parseCameraRequest(const CameraOptions& options)
{
  const auto subjects = int(options.lookAt.has_value()) + int(options.frame.has_value())
                        + int(options.selection);
  const auto fromToward = options.from || options.toward;
  const auto placements =
    int(options.orbit.has_value()) + int(fromToward) + int(options.axisViews.size());
  if (subjects == 0 && placements == 0)
  {
    if (options.size)
    {
      throw invalidArgument(
        fmt::format(
          "--size is for agent cameras: give one of {} or {}",
          SubjectOptions,
          PlacementOptions));
    }
    return std::nullopt;
  }
  if (subjects > 1)
  {
    throw invalidArgument(fmt::format("give only one of {}", SubjectOptions));
  }
  if (placements > 1)
  {
    throw invalidArgument(fmt::format("give only one of {}", PlacementOptions));
  }

  auto request = CameraRequest{};
  if (options.lookAt)
  {
    request.subject = LookAt{*options.lookAt};
  }
  else if (options.frame)
  {
    request.subject = Frame{parseFrame(*options.frame)};
  }
  else if (options.selection)
  {
    request.subject = SelectedNodes{};
  }

  if (fromToward)
  {
    if (!options.from || !options.toward)
    {
      throw invalidArgument("--from and --toward must be given together");
    }
    if (subjects > 0)
    {
      throw invalidArgument(
        fmt::format("--from and --toward fit no subject; leave out {}", SubjectOptions));
    }
    const auto from = parsePoint("--from", *options.from);
    const auto toward = parsePoint("--toward", *options.toward);
    if (from == toward)
    {
      throw invalidArgument("--toward must be another point than --from");
    }
    request.placement = FromToward{from, toward};
  }
  else if (options.orbit)
  {
    request.placement = parseOrbit(*options.orbit);
  }
  else if (!options.axisViews.empty())
  {
    request.placement = options.axisViews.front();
  }

  if (options.size)
  {
    request.size = parseSize(*options.size);
  }
  return request;
}

vm::bbox3d subjectBounds(
  mdl::Map& map, const CameraSubject& subject, NodeHandleTable* handles)
{
  return std::visit(
    kdl::overload(
      [&](const WholeMap&) {
        return matchBounds({&map.worldNode()}).value_or(vm::bbox3d{128.0});
      },
      [&](const LookAt& lookAt) {
        const auto target = resolveNodeReference(map.worldNode(), lookAt.target, handles);
        if (const auto bounds = matchBounds(target))
        {
          return *bounds;
        }
        throw invalidArgument(
          fmt::format("--look-at: '{}' has nothing in it to look at", lookAt.target));
      },
      [&](const Frame& frame) { return frame.bounds; },
      [&](const SelectedNodes&) {
        if (const auto& bounds = map.selectionBounds())
        {
          return *bounds;
        }
        const auto& faces = map.selection().brushFaces;
        if (faces.empty())
        {
          throw invalidArgument("--selection: the mapper has selected nothing");
        }
        auto bounds = faces.front().face().bounds();
        for (const auto& face : faces)
        {
          bounds = vm::merge(bounds, face.face().bounds());
        }
        return bounds;
      }),
    subject);
}

CameraPose resolveCameraPose(
  const CameraPlacement& placement,
  const vm::bbox3d& subject,
  const vm::bbox3d& worldBounds,
  const ImageSize size)
{
  return std::visit(
    kdl::overload(
      [&](const FromToward& fromToward) {
        return perspectivePose(
          fromToward.from, vm::normalize(fromToward.toward - fromToward.from), size);
      },
      [&](const Orbit& orbit) { return orbitPose(orbit, subject, size); },
      [&](const AxisView axisView) {
        return axisPose(axisView, subject, worldBounds, size);
      }),
    placement);
}

CameraPose resolveCameraPose(
  mdl::Map& map, const CameraRequest& request, NodeHandleTable* handles)
{
  const auto subject = std::holds_alternative<FromToward>(request.placement)
                         ? vm::bbox3d{}
                         : subjectBounds(map, request.subject, handles);
  return resolveCameraPose(request.placement, subject, map.worldBounds(), request.size);
}

} // namespace tb::cli
