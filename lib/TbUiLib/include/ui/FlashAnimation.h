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

#pragma once

#include "ui/Animation.h"

class QWidget;

namespace tb
{
namespace render
{
class MapRenderer;
}

namespace ui
{
/** What a FlashAnimation flashes. */
enum class Flashed
{
  /** The selection, by whitening the selection colors. */
  Selection,
  /**
   * The nodes that were given to MapRenderer::flashNodes, by tinting them with the
   * selection colors. They are cleared when the animation ends.
   */
  Nodes,
};

/** Flashes the selection or the given nodes in a map view, without moving its camera. */
class FlashAnimation : public Animation
{
private:
  static const Type SelectionAnimationType;
  static const Type NodesAnimationType;

  render::MapRenderer& m_renderer;
  QWidget* m_view;
  Flashed m_flashed;

public:
  FlashAnimation(
    render::MapRenderer& renderer, QWidget* view, double duration, Flashed flashed);

private:
  void doUpdate(double progress) override;
};


} // namespace ui
} // namespace tb
