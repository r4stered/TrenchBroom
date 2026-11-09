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

#include "ui/MapViewCapture.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QPainter>
#include <QPen>

#include "cli/CliError.h"
#include "gl/Camera.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/ModelUtils.h"
#include "mdl/PickResult.h"
#include "ui/MapViewBase.h"

#include <algorithm>
#include <cmath>

namespace tb::ui
{
namespace
{

/** The node and face under the mouse, if a node was. */
std::optional<cli::HoveredPick> hoveredPick(const mdl::PickResult& pickResult)
{
  using namespace mdl::HitFilters;

  const auto& hit = pickResult.first(type(mdl::nodeHitType()));
  auto* node = mdl::hitToNode(hit);
  if (!node)
  {
    return std::nullopt;
  }

  const auto faceHandle = mdl::hitToFaceHandle(hit);
  return cli::HoveredPick{
    .node = node,
    .faceIndex = faceHandle ? std::optional{faceHandle->faceIndex()} : std::nullopt,
    .point = hit.hitPoint(),
  };
}

/**
 * Draws a crosshair centered on the given point, with a gap at the point so that what is
 * under the cursor stays visible, in a light color with a dark outline so that it shows
 * on any background.
 */
void drawCrosshair(QImage& image, const cli::ImagePoint& point, const double scale)
{
  const auto gap = std::max(3, int(std::lround(4.0 * scale)));
  const auto arm = std::max(8, int(std::lround(16.0 * scale)));
  const auto width = std::max(1, int(std::lround(scale)));

  auto painter = QPainter{&image};
  const auto drawArms = [&](const QPen& pen) {
    painter.setPen(pen);
    painter.drawLine(point.x - gap - arm, point.y, point.x - gap, point.y);
    painter.drawLine(point.x + gap, point.y, point.x + gap + arm, point.y);
    painter.drawLine(point.x, point.y - gap - arm, point.x, point.y - gap);
    painter.drawLine(point.x, point.y + gap, point.x, point.y + gap + arm);
  };
  drawArms(QPen{QColor{0, 0, 0}, qreal(width + 2), Qt::SolidLine, Qt::SquareCap});
  drawArms(QPen{QColor{255, 255, 0}, qreal(width), Qt::SolidLine, Qt::SquareCap});
}

} // namespace

cli::ViewCapture makeViewCapture(
  const QImage& framebuffer,
  const gl::Camera& camera,
  const float pixelRatio,
  const mdl::PickResult& pickResult,
  const std::optional<QPointF>& cursor)
{
  if (framebuffer.isNull())
  {
    throw cli::CliError{
      cli::ErrorCode::ViewFailed, "the map view could not be captured; is it visible?"};
  }

  const auto size =
    cli::fitImageSize(cli::ImageSize{framebuffer.width(), framebuffer.height()});
  auto image =
    size.width == framebuffer.width()
      ? framebuffer
      : framebuffer.scaled(
          size.width, size.height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

  // a length in logical pixels of the view as one in the image's pixels, divided last
  // so that a cursor on a whole pixel lands on one in the image
  const auto toImage = [&](const double logical) {
    return logical * double(pixelRatio) * double(size.width)
           / double(framebuffer.width());
  };
  auto cursorInImage = std::optional<cli::ImagePoint>{};
  auto hovered = hoveredPick(pickResult);
  if (cursor)
  {
    const auto x = int(std::floor(toImage(cursor->x())));
    const auto y = int(std::floor(toImage(cursor->y())));
    if (x >= 0 && y >= 0 && x < size.width && y < size.height)
    {
      cursorInImage = cli::ImagePoint{x, y};
      // drawn on the copy only, since a QImage shares its pixels until written
      drawCrosshair(image, *cursorInImage, toImage(1.0));
    }
    else
    {
      // the view's last pick is not under a cursor that is elsewhere
      hovered = std::nullopt;
    }
  }

  auto bytes = QByteArray{};
  auto buffer = QBuffer{&bytes};
  buffer.open(QIODevice::WriteOnly);
  if (!image.save(&buffer, "PNG"))
  {
    throw cli::CliError{
      cli::ErrorCode::ViewFailed, "the map view could not be encoded as PNG"};
  }

  return cli::ViewCapture{
    .png = bytes.toStdString(),
    .image = size,
    .camera = cli::cameraPose(camera),
    .pixelRatio = pixelRatio,
    .hovered = hovered,
    .cursor = cursorInImage,
  };
}

cli::ViewCapture captureMapView(MapViewBase& view, const std::optional<QPointF>& cursor)
{
  // renders the view as it is on screen, at its physical size
  const auto framebuffer = view.grabFramebuffer();
  return makeViewCapture(
    framebuffer,
    view.camera(),
    static_cast<float>(view.devicePixelRatioF()),
    view.pickResult(),
    cursor);
}

} // namespace tb::ui
