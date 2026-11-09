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

#include "cli/View.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace tb::mdl
{
class Map;
} // namespace tb::mdl

namespace tb::cli
{

class NodeHandleTable;

/** The size of an agent camera's image unless `--size` gives another. */
inline constexpr ImageSize DefaultAgentViewSize = {1024, 768};

/**
 * An orthographic camera that looks along an axis like the editor's 2D views, zoomed to
 * fit its subject: from above (xy), from the front (xz) or from the side (yz).
 */
enum class AxisView
{
  Top,
  Front,
  Side,
};

/** The `tbcli view` options that pick an agent camera, as given on the command line. */
struct CameraOptions
{
  /** --look-at <path|handle> */
  std::optional<std::string> lookAt;
  /** --frame x1,y1,z1,x2,y2,z2 */
  std::optional<std::string> frame;
  /** --selection */
  bool selection = false;
  /** --from x,y,z */
  std::optional<std::string> from;
  /** --toward x,y,z */
  std::optional<std::string> toward;
  /** --orbit yaw,pitch[,distance] */
  std::optional<std::string> orbit;
  /** --top, --front and --side, in the order given; at most one is valid */
  std::vector<AxisView> axisViews;
  /** --size WxH */
  std::optional<std::string> size;
};

/** An agent camera looks at everything in the map unless it is given a subject. */
struct WholeMap
{
  bool operator==(const WholeMap&) const = default;
};

/** --look-at: a node or face, by path or handle. */
struct LookAt
{
  std::string target;

  bool operator==(const LookAt&) const = default;
};

/** --frame: a box. */
struct Frame
{
  vm::bbox3d bounds;

  bool operator==(const Frame&) const = default;
};

/** --selection: the mapper's selected nodes. */
struct SelectedNodes
{
  bool operator==(const SelectedNodes&) const = default;
};

/** What an agent camera fits into its view. */
using CameraSubject = std::variant<WholeMap, LookAt, Frame, SelectedNodes>;

/**
 * A perspective camera around the center of its subject, at the given angles in degrees:
 * the yaw counterclockwise about +Z from +X to where the camera is, the pitch up from the
 * horizontal plane. Without a distance, the camera backs off until the subject fits.
 */
struct Orbit
{
  /** Southwest of the subject, the way Quake maps are often looked at. */
  static constexpr double DefaultYaw = 225.0;
  static constexpr double DefaultPitch = 30.0;

  double yaw = DefaultYaw;
  double pitch = DefaultPitch;
  std::optional<double> distance;

  bool operator==(const Orbit&) const = default;
};

/** A perspective camera at one point that looks toward another. */
struct FromToward
{
  vm::vec3d from;
  vm::vec3d toward;

  bool operator==(const FromToward&) const = default;
};

/** Where an agent camera is and where it looks. */
using CameraPlacement = std::variant<Orbit, FromToward, AxisView>;

/** An agent camera, as `tbcli view` asks for it. */
struct CameraRequest
{
  CameraSubject subject = WholeMap{};
  CameraPlacement placement = Orbit{};
  ImageSize size = DefaultAgentViewSize;

  bool operator==(const CameraRequest&) const = default;
};

/**
 * The agent camera that the given options ask for, or nothing if they ask for none, in
 * which case `tbcli view` captures the mapper's view.
 *
 * A subject without a placement orbits it at the default angles, and a placement without
 * a subject fits the whole map. The size is fitted into MaxViewImageSize.
 *
 * @throw CliError `invalid_argument` if an option's value is malformed or the options
 * contradict each other
 */
std::optional<CameraRequest> parseCameraRequest(const CameraOptions& options);

/**
 * The bounds of the given subject in the given map: the bounds of the node or face that
 * --look-at names, the frame, the bounds of the selected nodes, or of everything in the
 * map. An empty map has a small box around the origin.
 *
 * @throw CliError as resolveNodeReference does for a node that --look-at names, and
 * `invalid_argument` if the subject has no bounds, such as an empty selection
 */
vm::bbox3d subjectBounds(
  mdl::Map& map, const CameraSubject& subject, NodeHandleTable* handles);

/**
 * The pose of an agent camera that is placed as given and fits the given subject into an
 * image of the given size. Orthographic cameras look across the given world bounds, so
 * that the grid at their far side shows.
 */
CameraPose resolveCameraPose(
  const CameraPlacement& placement,
  const vm::bbox3d& subject,
  const vm::bbox3d& worldBounds,
  ImageSize size);

/**
 * The pose of the given agent camera in the given map: fitting the bounds of its subject
 * (see subjectBounds), unless it looks from one point toward another, which fits nothing.
 *
 * @throw CliError as subjectBounds does
 */
CameraPose resolveCameraPose(
  mdl::Map& map, const CameraRequest& request, NodeHandleTable* handles);

} // namespace tb::cli
