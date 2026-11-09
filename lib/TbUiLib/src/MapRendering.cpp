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

#include "ui/MapRendering.h"

#include "base/PreferenceManager.h"
#include "gl/Camera.h"
#include "gl/GlInterface.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/Map_World.h"
#include "prefs/Preferences.h"
#include "render/RenderContext.h"

#include "vm/bbox.h"

namespace tb::ui
{

void setupMapRenderContext(
  render::RenderContext& renderContext, const mdl::Map& map, const float pixelRatio)
{
  const auto& grid = map.grid();

  renderContext.setFilterMode(
    pref(Preferences::TextureMinFilter), pref(Preferences::TextureMagFilter));
  renderContext.setShowMaterials(
    pref(Preferences::FaceRenderMode) == Preferences::FaceRenderModeTextured);
  renderContext.setShowFaces(
    pref(Preferences::FaceRenderMode) != Preferences::FaceRenderModeSkip);
  renderContext.setShowEdges(pref(Preferences::ShowEdges));
  renderContext.setShadeFaces(pref(Preferences::ShadeFaces));
  renderContext.setShowPointEntities(pref(Preferences::ShowPointEntities));
  renderContext.setShowPointEntityModels(pref(Preferences::ShowPointEntityModels));
  renderContext.setShowEntityClassnames(pref(Preferences::ShowEntityClassnames));
  renderContext.setShowGroupBounds(pref(Preferences::ShowGroupBounds));
  renderContext.setShowBrushEntityBounds(pref(Preferences::ShowBrushEntityBounds));
  renderContext.setShowPointEntityBounds(pref(Preferences::ShowPointEntityBounds));
  renderContext.setShowFog(pref(Preferences::ShowFog));
  renderContext.setShowGrid(grid.visible());
  renderContext.setGridSize(grid.actualSize());
  renderContext.setDpiScale(pixelRatio);
  renderContext.setSoftMapBounds(
    pref(Preferences::ShowSoftMapBounds)
      ? vm::bbox3f{softMapBounds(map).bounds.value_or(vm::bbox3d{})}
      : vm::bbox3f{});
}

void setupMapRenderGL(gl::Gl& gl, const gl::Camera& camera, const float pixelRatio)
{
  const auto& viewport = camera.viewport();
  const auto x = static_cast<int>(viewport.x * pixelRatio);
  const auto y = static_cast<int>(viewport.y * pixelRatio);
  const auto width = static_cast<int>(viewport.width * pixelRatio);
  const auto height = static_cast<int>(viewport.height * pixelRatio);
  gl.viewport(x, y, width, height);

  if (pref(Preferences::EnableMSAA))
  {
    gl.enable(GL_MULTISAMPLE);
  }
  else
  {
    gl.disable(GL_MULTISAMPLE);
  }
  gl.enable(GL_BLEND);
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  gl.shadeModel(GL_SMOOTH);
}

} // namespace tb::ui
