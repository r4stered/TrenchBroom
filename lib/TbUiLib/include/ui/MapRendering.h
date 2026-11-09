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

#pragma once

namespace tb
{
namespace gl
{
class Camera;
class Gl;
} // namespace gl

namespace mdl
{
class Map;
}

namespace render
{
class RenderContext;
}

namespace ui
{

/**
 * Applies the map view preferences, the given map's grid and soft map bounds, and the
 * given pixel ratio to the given render context.
 *
 * The render context is rendered from the camera it was created with, which need not
 * belong to a map view.
 */
void setupMapRenderContext(
  render::RenderContext& renderContext, const mdl::Map& map, float pixelRatio);

/**
 * Sets the GL viewport to the given camera's viewport scaled by the given pixel ratio,
 * and sets the GL state that map rendering expects.
 */
void setupMapRenderGL(gl::Gl& gl, const gl::Camera& camera, float pixelRatio);

} // namespace ui
} // namespace tb
