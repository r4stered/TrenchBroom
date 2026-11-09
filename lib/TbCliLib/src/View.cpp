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

#include "cli/View.h"

#include "cli/AgentCamera.h"
#include "cli/CappedList.h"
#include "cli/CliError.h"
#include "cli/FileUtils.h"
#include "cli/GeometryJson.h"
#include "cli/LiveSession.h"
#include "cli/NodeHandles.h"
#include "gl/OrthographicCamera.h"
#include "gl/PerspectiveCamera.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/Selection.h"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <system_error>

namespace tb::cli
{
namespace
{

/**
 * A float as JSON, with no more digits than it takes to read the float back, so that
 * 0.7071068f doesn't print as 0.70710676908493042. Parsed with from_chars, which unlike
 * stod ignores the locale that the editor sets.
 */
Json floatJson(const float f)
{
  const auto text = fmt::format("{}", f);
  auto value = double(f);
  std::from_chars(text.data(), text.data() + text.size(), value);
  return Json{value};
}

Json toJson(const vm::vec3f& v)
{
  return Json{Json::Array{floatJson(v.x()), floatJson(v.y()), floatJson(v.z())}};
}

Json toJson(const std::optional<ImageBox>& box)
{
  return box ? Json{Json::Array{box->left, box->top, box->right, box->bottom}}
             : Json{nullptr};
}

/**
 * The given points, clipped to the half space in front of a perspective camera's near
 * plane: the points in front of it, and where the segments between the other points and
 * them cross it. Their convex hull is the part of the points' convex hull in front of
 * the plane.
 */
std::vector<vm::vec3d> clipToNearPlane(
  const CameraPose& pose, const std::vector<vm::vec3d>& points)
{
  const auto position = vm::vec3d{pose.position};
  const auto direction = vm::vec3d{pose.direction};
  const auto nearPlane = double(pose.nearPlane);
  const auto depth = [&](const vm::vec3d& point) {
    return vm::dot(point - position, direction);
  };

  auto result = std::vector<vm::vec3d>{};
  for (const auto& point : points)
  {
    if (depth(point) >= nearPlane)
    {
      result.push_back(point);
    }
  }
  if (result.size() == points.size())
  {
    return result;
  }

  for (const auto& front : points)
  {
    const auto frontDepth = depth(front);
    if (frontDepth < nearPlane)
    {
      continue;
    }
    for (const auto& back : points)
    {
      if (const auto backDepth = depth(back); backDepth < nearPlane)
      {
        const auto t = (frontDepth - nearPlane) / (frontDepth - backDepth);
        result.push_back(front + (back - front) * t);
      }
    }
  }
  return result;
}

/** `{path, width, height}` of an image at the given path. */
Json imageJson(const std::filesystem::path& path, const ImageSize size)
{
  auto result = Json::object();
  result.set("path", path.string());
  result.set("width", size.width);
  result.set("height", size.height);
  return result;
}

/** Sets the members of the given object in another. */
void setMembers(Json& object, const Json& members)
{
  for (const auto& [key, value] : members.asObject())
  {
    object.set(key, value);
  }
}

/**
 * What a captured view shows: `{view, camera, pixel_ratio, selection, hovered}`, with
 * the selection and the node under the mouse as paths and handles of the current rev,
 * and the rectangles they cover in the image.
 */
Json viewContext(LiveSession& session, const ViewCapture& capture)
{
  auto& map = session.map();
  auto context = Json::object();
  context.set("view", viewType(capture.camera));
  context.set("camera", toJson(capture.camera));
  context.set("pixel_ratio", floatJson(capture.pixelRatio));

  auto& handles = session.handles();
  const auto boxOf = [&](const std::vector<vm::vec3d>& points) {
    return toJson(projectPoints(capture.camera, capture.image, points));
  };
  const auto boundsBox = [&](const mdl::Node& node) {
    return toJson(projectBounds(capture.camera, capture.image, node.logicalBounds()));
  };

  const auto& selection = map.selection();
  auto selectionJson = Json::object();
  selectionJson.set(
    "nodes", capped(selection.nodes, Paging{}.limit, [&](const mdl::Node* node) {
      auto json = Json::object();
      setNodePath(json, "path", *node, &handles);
      json.set("box", boundsBox(*node));
      return json;
    }));
  selectionJson.set(
    "faces",
    capped(selection.brushFaces, Paging{}.limit, [&](const mdl::BrushFaceHandle& face) {
      auto json = Json::object();
      setFacePath(json, "path", face.node(), face.faceIndex(), &handles);
      json.set("box", boxOf(face.face().vertexPositions()));
      return json;
    }));
  context.set("selection", std::move(selectionJson));

  auto hovered = Json{nullptr};
  if (capture.hovered && capture.hovered->node)
  {
    const auto& node = *capture.hovered->node;
    hovered = Json::object();
    setNodePath(hovered, "path", node, &handles);
    if (
      const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node);
      brushNode && capture.hovered->faceIndex)
    {
      setFacePath(hovered, "face", *brushNode, *capture.hovered->faceIndex, &handles);
    }
    hovered.set("point", cli::toJson(capture.hovered->point));
    hovered.set("box", boundsBox(node));
  }
  context.set("hovered", std::move(hovered));

  return context;
}

/** The whole seconds from the given shown view's showing to the given time. */
int64_t ageInSeconds(
  const ShownView& shown, const std::chrono::steady_clock::time_point now)
{
  return std::max(
    int64_t{0},
    int64_t(std::chrono::floor<std::chrono::seconds>(now - shown.shownAt).count()));
}

/**
 * The given shown view as `tbcli view` reports it: its rev, age and image, which this
 * adds to the session's view images, and what it showed.
 */
Json shownViewJson(LiveSession& session, const ShownView& shown)
{
  const auto imagePath = session.viewImages().add(shown.png);

  auto result = Json::object();
  result.set("rev", shown.rev);
  result.set("age", ageInSeconds(shown, std::chrono::steady_clock::now()));

  result.set("image", imageJson(imagePath, shown.image));
  setMembers(result, shown.context);
  return result;
}

} // namespace

ImageSize fitImageSize(const ImageSize size, const int maxSize)
{
  const auto largest = std::max(size.width, size.height);
  if (largest <= maxSize)
  {
    return size;
  }
  const auto scale = double(maxSize) / double(largest);
  return {
    std::max(1, int(std::lround(size.width * scale))),
    std::max(1, int(std::lround(size.height * scale))),
  };
}

CameraPose cameraPose(const gl::Camera& camera)
{
  const auto* perspective = dynamic_cast<const gl::PerspectiveCamera*>(&camera);
  return CameraPose{
    .projection = perspective ? CameraPose::Projection::Perspective
                              : CameraPose::Projection::Orthographic,
    .position = camera.position(),
    .direction = camera.direction(),
    .up = camera.up(),
    .fov = perspective ? perspective->fov() : 90.0f,
    .zoom = camera.zoom(),
    .nearPlane = camera.nearPlane(),
    .farPlane = camera.farPlane(),
    .viewport = {camera.viewport().width, camera.viewport().height},
  };
}

std::unique_ptr<gl::Camera> makeCamera(const CameraPose& pose)
{
  const auto viewport =
    gl::Camera::Viewport{0, 0, pose.viewport.width, pose.viewport.height};
  auto camera = std::unique_ptr<gl::Camera>{};
  if (pose.projection == CameraPose::Projection::Perspective)
  {
    camera = std::make_unique<gl::PerspectiveCamera>(
      pose.fov,
      pose.nearPlane,
      pose.farPlane,
      viewport,
      pose.position,
      pose.direction,
      pose.up);
  }
  else
  {
    camera = std::make_unique<gl::OrthographicCamera>(
      pose.nearPlane, pose.farPlane, viewport, pose.position, pose.direction, pose.up);
  }
  camera->setZoom(pose.zoom);
  return camera;
}

std::string viewType(const CameraPose& pose)
{
  if (pose.projection == CameraPose::Projection::Perspective)
  {
    return "3d";
  }
  switch (vm::find_abs_max_component(pose.direction))
  {
  case vm::axis::x:
    return "yz";
  case vm::axis::y:
    return "xz";
  default:
    return "xy";
  }
}

Json toJson(const CameraPose& pose)
{
  const auto perspective = pose.projection == CameraPose::Projection::Perspective;
  auto result = Json::object();
  result.set("projection", perspective ? "perspective" : "orthographic");
  result.set("position", toJson(pose.position));
  result.set("direction", toJson(pose.direction));
  result.set("up", toJson(pose.up));
  if (perspective)
  {
    result.set("fov", floatJson(pose.fov));
  }
  result.set("zoom", floatJson(pose.zoom));
  result.set("viewport", Json{Json::Array{pose.viewport.width, pose.viewport.height}});
  return result;
}

std::optional<ImageBox> projectPoints(
  const CameraPose& pose, const ImageSize image, const std::vector<vm::vec3d>& points)
{
  const auto visible = pose.projection == CameraPose::Projection::Perspective
                         ? clipToNearPlane(pose, points)
                         : points;
  if (visible.empty() || pose.viewport.width <= 0 || pose.viewport.height <= 0)
  {
    return std::nullopt;
  }

  const auto camera = makeCamera(pose);
  const auto scaleX = double(image.width) / double(pose.viewport.width);
  const auto scaleY = double(image.height) / double(pose.viewport.height);

  auto left = std::numeric_limits<double>::max();
  auto top = std::numeric_limits<double>::max();
  auto right = std::numeric_limits<double>::lowest();
  auto bottom = std::numeric_limits<double>::lowest();
  for (const auto& point : visible)
  {
    // the camera's window coordinates have their origin at the bottom left
    const auto window = camera->project(vm::vec3f{point});
    const auto x = double(window.x()) * scaleX;
    const auto y = (double(pose.viewport.height) - double(window.y())) * scaleY;
    left = std::min(left, x);
    right = std::max(right, x);
    top = std::min(top, y);
    bottom = std::max(bottom, y);
  }

  if (
    right <= 0.0 || bottom <= 0.0 || left >= double(image.width)
    || top >= double(image.height))
  {
    return std::nullopt;
  }

  // rounded out to whole pixels, forgiving the float error of the projection
  constexpr auto Epsilon = 0.001;
  const auto clamp = [](const double value, const int max) {
    return int(std::clamp(value, 0.0, double(max)));
  };
  return ImageBox{
    clamp(std::floor(left + Epsilon), image.width),
    clamp(std::floor(top + Epsilon), image.height),
    clamp(std::ceil(right - Epsilon), image.width),
    clamp(std::ceil(bottom - Epsilon), image.height),
  };
}

std::optional<ImageBox> projectBounds(
  const CameraPose& pose, const ImageSize image, const vm::bbox3d& bounds)
{
  const auto corners = bounds.vertices();
  return projectPoints(pose, image, {corners.begin(), corners.end()});
}

ViewImages::ViewImages(std::filesystem::path dir, const size_t kept)
  : m_dir{std::move(dir)}
  , m_kept{kept}
{
}

ViewImages::~ViewImages()
{
  if (m_count > 0)
  {
    auto error = std::error_code{};
    std::filesystem::remove_all(m_dir, error);
  }
}

const std::filesystem::path& ViewImages::dir() const
{
  return m_dir;
}

std::filesystem::path ViewImages::add(const std::string_view png)
{
  auto error = std::error_code{};
  std::filesystem::create_directories(m_dir, error);
  if (error)
  {
    throw CliError{
      ErrorCode::WriteFailed,
      fmt::format("cannot create {}: {}", m_dir.string(), error.message())};
  }

  auto path = m_dir / fmt::format("view-{:04}.png", ++m_count);
  if (const auto writeError = writeFile(path, png))
  {
    throw CliError{
      ErrorCode::WriteFailed,
      fmt::format("cannot write {}: {}", path.string(), *writeError)};
  }

  m_images.push_back(path);
  while (m_images.size() > m_kept)
  {
    std::filesystem::remove(m_images.front(), error);
    m_images.pop_front();
  }
  return path;
}

ShownView makeShownView(
  LiveSession& session,
  const ViewCapture& capture,
  const std::chrono::steady_clock::time_point shownAt)
{
  auto context = viewContext(session, capture);
  context.set(
    "cursor",
    capture.cursor ? Json{Json::Array{capture.cursor->x, capture.cursor->y}}
                   : Json{nullptr});
  return ShownView{
    .png = capture.png,
    .image = capture.image,
    .rev = session.rev(),
    .context = std::move(context),
    .shownAt = shownAt,
  };
}

Json shownHint(const ShownView& shown, const std::chrono::steady_clock::time_point now)
{
  auto result = Json::object();
  result.set("rev", shown.rev);
  result.set("age", ageInSeconds(shown, now));
  result.set("run", "tbcli view");
  return result;
}

Json viewLive(
  LiveSession& session,
  const std::optional<CameraRequest>& camera,
  const std::optional<std::filesystem::path>& out)
{
  auto& map = session.map();
  if (out && isSameFile(*out, map.path()))
  {
    throw invalidArgument("--out names the map's own file");
  }

  const auto capture =
    camera ? session.renderAgentView(resolveCameraPose(map, *camera, &session.handles()))
           : session.captureMapperView();
  const auto imagePath = session.viewImages().add(capture.png);

  auto result = Json::object();
  result.set("schema", "tbcli.view/1");
  result.set("session", "live");
  result.set("rev", session.rev());

  result.set("image", imageJson(imagePath, capture.image));

  if (out)
  {
    const auto outPath = std::filesystem::absolute(*out);
    auto error = std::error_code{};
    std::filesystem::copy_file(
      imagePath, outPath, std::filesystem::copy_options::overwrite_existing, error);
    if (error)
    {
      throw CliError{
        ErrorCode::WriteFailed,
        fmt::format(
          "cannot copy the image to {}: {}", outPath.string(), error.message())};
    }
    result.set("out", outPath.string());
  }

  setMembers(result, viewContext(session, capture));

  if (const auto* shown = session.shownView())
  {
    result.set("shown", shownViewJson(session, *shown));
    // seen once
    session.takeShownView();
  }

  return result;
}

} // namespace tb::cli
