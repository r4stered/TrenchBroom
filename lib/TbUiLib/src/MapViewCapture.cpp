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

#include "cli/CliError.h"
#include "gl/Camera.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/ModelUtils.h"
#include "mdl/PickResult.h"
#include "ui/MapViewBase.h"

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

} // namespace

cli::ViewCapture makeViewCapture(
  const QImage& framebuffer,
  const gl::Camera& camera,
  const float pixelRatio,
  const mdl::PickResult& pickResult)
{
  if (framebuffer.isNull())
  {
    throw cli::CliError{
      cli::ErrorCode::ViewFailed, "the map view could not be captured; is it visible?"};
  }

  const auto size =
    cli::fitImageSize(cli::ImageSize{framebuffer.width(), framebuffer.height()});
  const auto image =
    size.width == framebuffer.width()
      ? framebuffer
      : framebuffer.scaled(
          size.width, size.height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

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
    .hovered = hoveredPick(pickResult),
  };
}

cli::ViewCapture captureMapView(MapViewBase& view)
{
  // renders the view as it is on screen, at its physical size
  const auto framebuffer = view.grabFramebuffer();
  return makeViewCapture(
    framebuffer,
    view.camera(),
    static_cast<float>(view.devicePixelRatioF()),
    view.pickResult());
}

} // namespace tb::ui
