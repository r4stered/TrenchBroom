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

namespace tb::ui
{
class AppController;
class MapDocument;

/**
 * Renders the given document's map as an agent camera with the given pose sees it, into
 * an image of the size of the pose's viewport, for `tbcli view`.
 *
 * Renders on the calling (GUI) thread with the app's offscreen GL context, which shares
 * its resources with the map views, into a multisampled framebuffer object if
 * multisampling is on and the GL can resolve it, else into one without multisampling.
 * Draws the map as the map views do, with the mapper's hidden, isolated and selected
 * nodes, and the grid for an orthographic camera, but no tools, so that the mapper's
 * views and cameras are left as they are.
 *
 * @throw cli::CliError `view_failed` if no map view has initialized GL yet, or the view
 * cannot be rendered or encoded
 */
cli::ViewCapture renderMapOffscreen(
  AppController& appController, MapDocument& document, const cli::CameraPose& pose);

} // namespace tb::ui
