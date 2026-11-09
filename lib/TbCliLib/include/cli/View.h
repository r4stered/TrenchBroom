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

#include "cli/Json.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::gl
{
class Camera;
} // namespace tb::gl

namespace tb::mdl
{
class Node;
} // namespace tb::mdl

namespace tb::cli
{

class LiveSession;

/** The largest width or height of an image that `tbcli view` returns, in pixels. */
inline constexpr int MaxViewImageSize = 2048;

/** How many of a session's view images are kept; older ones are deleted. */
inline constexpr size_t KeptViewImages = 20;

/** The size of an image in pixels. */
struct ImageSize
{
  int width = 0;
  int height = 0;

  bool operator==(const ImageSize&) const = default;
};

/**
 * The given size, scaled down to fit `maxSize` in both directions with its aspect ratio
 * kept, or as it is if it fits already.
 */
ImageSize fitImageSize(ImageSize size, int maxSize = MaxViewImageSize);

/**
 * Where a camera is and how it projects: everything needed to make the same camera
 * again, as a view result reports it.
 */
struct CameraPose
{
  enum class Projection
  {
    Perspective,
    Orthographic,
  };

  Projection projection = Projection::Perspective;
  vm::vec3f position = vm::vec3f{0, 0, 0};
  vm::vec3f direction = vm::vec3f{1, 0, 0};
  vm::vec3f up = vm::vec3f{0, 0, 1};
  /** The unzoomed field of view in degrees; perspective cameras only. */
  float fov = 90.0f;
  float zoom = 1.0f;
  float nearPlane = 1.0f;
  float farPlane = 65536.0f;
  /** The size of the viewport in logical (device independent) pixels. */
  ImageSize viewport = {1024, 768};
};

/** The pose of the given camera. */
CameraPose cameraPose(const gl::Camera& camera);

/** A camera with the given pose. */
std::unique_ptr<gl::Camera> makeCamera(const CameraPose& pose);

/**
 * The kind of view that a camera with the given pose shows: `3d` for a perspective
 * camera, otherwise the plane of a 2D view that looks along an axis, `xy`, `xz` or `yz`.
 */
std::string viewType(const CameraPose& pose);

/**
 * `{projection, position, direction, up, fov, zoom, viewport}`; `fov` only for a
 * perspective camera.
 */
Json toJson(const CameraPose& pose);

/** A rectangle in an image, in pixels from its top left corner. */
struct ImageBox
{
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  bool operator==(const ImageBox&) const = default;
};

/**
 * The rectangle that the given points cover in an image of the given size that shows the
 * view of a camera with the given pose, clipped to the image, or nothing if they are not
 * in the image at all. The parts in front of a perspective camera's near plane count, so
 * that a box the camera is inside of still covers the image.
 */
std::optional<ImageBox> projectPoints(
  const CameraPose& pose, ImageSize image, const std::vector<vm::vec3d>& points);

/** The rectangle that the given bounds cover in the image, as projectPoints. */
std::optional<ImageBox> projectBounds(
  const CameraPose& pose, ImageSize image, const vm::bbox3d& bounds);

/** The thing under the mapper's mouse when a view last picked. */
struct HoveredPick
{
  mdl::Node* node = nullptr;
  /** The brush face under the mouse, if the node is a brush. */
  std::optional<size_t> faceIndex;
  /** Where the pick ray hit the node. */
  vm::vec3d point = vm::vec3d{0, 0, 0};
};

/** One map view as the editor shows it, captured for `tbcli view`. */
struct ViewCapture
{
  /** The image, encoded as PNG. */
  std::string png;
  /** The size of the image in pixels. */
  ImageSize image;
  CameraPose camera;
  /** The physical pixels per logical pixel of the view. */
  float pixelRatio = 1.0f;
  std::optional<HoveredPick> hovered;
};

/**
 * The images of a live session's views: a temporary directory that holds the most
 * recent of them and is deleted with this.
 */
class ViewImages
{
private:
  std::filesystem::path m_dir;
  size_t m_kept;
  uint64_t m_count = 0;
  std::deque<std::filesystem::path> m_images;

public:
  /**
   * Images in the given directory, which is created with the first image, of which the
   * given number are kept.
   */
  explicit ViewImages(std::filesystem::path dir, size_t kept = KeptViewImages);
  ~ViewImages();

  ViewImages(const ViewImages&) = delete;
  ViewImages& operator=(const ViewImages&) = delete;

  const std::filesystem::path& dir() const;

  /**
   * Writes the given PNG as a new image and deletes the oldest image beyond the number
   * kept.
   *
   * @return the path of the new image
   * @throw CliError `write_failed` if the image cannot be written
   */
  std::filesystem::path add(std::string_view png);
};

/**
 * `tbcli view`: captures the mapper's focused view as a PNG in the session's view images
 * and reports what it shows: the camera, the view type, the selection and the node under
 * the mouse, with the rectangles they cover in the image, the pixel ratio and the rev.
 * With `out`, also copies the image there.
 *
 * @throw CliError `view_failed` if the editor cannot capture its view, `write_failed`
 * if the image cannot be written
 */
Json viewLive(LiveSession& session, const std::optional<std::filesystem::path>& out);

} // namespace tb::cli
