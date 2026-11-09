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

class QImage;

namespace tb::gl
{
class Camera;
} // namespace tb::gl

namespace tb::mdl
{
class PickResult;
} // namespace tb::mdl

namespace tb::ui
{
class MapViewBase;

/**
 * A capture of a map view for `tbcli view`: the view's framebuffer, scaled down to fit
 * cli::MaxViewImageSize and encoded as PNG, with the view's camera, its pixel ratio and
 * the node and face under the mouse at the view's last pick.
 *
 * @throw cli::CliError `view_failed` if the framebuffer is empty or cannot be encoded
 */
cli::ViewCapture makeViewCapture(
  const QImage& framebuffer,
  const gl::Camera& camera,
  float pixelRatio,
  const mdl::PickResult& pickResult);

/**
 * Captures the given map view as it is on screen, with its grid, selection, handles and
 * compass; see makeViewCapture.
 *
 * @throw cli::CliError `view_failed` if the view cannot be captured, such as when it is
 * hidden
 */
cli::ViewCapture captureMapView(MapViewBase& view);

} // namespace tb::ui
