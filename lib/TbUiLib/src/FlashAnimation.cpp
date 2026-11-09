/*
 Copyright (C) 2010 Kristian Duske

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

#include "ui/FlashAnimation.h"

#include <QWidget>

#include "base/Color.h"
#include "render/MapRenderer.h"

#include <cmath>

namespace tb::ui
{
// flashing the selection and flashing nodes don't replace each other
const Animation::Type FlashAnimation::SelectionAnimationType = Animation::freeType();
const Animation::Type FlashAnimation::NodesAnimationType = Animation::freeType();

FlashAnimation::FlashAnimation(
  render::MapRenderer& renderer,
  QWidget* view,
  const double duration,
  const Flashed flashed)
  : Animation{
      flashed == Flashed::Selection ? SelectionAnimationType : NodesAnimationType,
      Curve::EaseInEaseOut,
      duration}
  , m_renderer{renderer}
  , m_view{view}
  , m_flashed{flashed}
{
}

void FlashAnimation::doUpdate(const double progress)
{
  static constexpr auto flashCount = 2.0;
  static constexpr auto white = RgbaF{1.0f, 1.0f, 1.0f, 1.0f};

  if (progress < 1.0)
  {
    const auto flashProgress = progress * flashCount - std::floor(progress * flashCount);

    // factor ranges from 0 to 1 and then back to 0
    const auto factor =
      flashProgress < 0.5 ? 2.0 * flashProgress : 1.0 - 2.0 * (flashProgress - 0.5);
    if (m_flashed == Flashed::Selection)
    {
      m_renderer.overrideSelectionColors(white, float(factor * 0.8));
    }
    else
    {
      m_renderer.setFlashIntensity(float(factor));
    }
  }
  else if (m_flashed == Flashed::Selection)
  {
    m_renderer.restoreSelectionColors();
  }
  else
  {
    m_renderer.clearFlashedNodes();
  }

  m_view->update();
}

} // namespace tb::ui
