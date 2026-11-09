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

#include "ui/OffscreenMapRendering.h"

#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>

#include "base/PreferenceManager.h"
#include "cli/CliError.h"
#include "gl/Camera.h"
#include "gl/GlManager.h"
#include "gl/OrthographicCamera.h"
#include "mdl/Map.h"
#include "mdl/PickResult.h"
#include "prefs/Preferences.h"
#include "render/GridRenderer.h"
#include "render/MapRenderer.h"
#include "render/RenderBatch.h"
#include "render/RenderContext.h"
#include "ui/AppController.h"
#include "ui/GlFunctions.h"
#include "ui/GlQt.h"
#include "ui/MapDocument.h"
#include "ui/MapRendering.h"
#include "ui/MapViewCapture.h"

#include <memory>

namespace tb::ui
{
namespace
{

/** The samples per pixel of an agent camera's image when multisampling is on. */
constexpr auto MultisampleSamples = 4;

/** Releases the offscreen context when the render is done, however it ends. */
class OffscreenContextScope
{
private:
  QOpenGLContext& m_context;

public:
  explicit OffscreenContextScope(QOpenGLContext& context)
    : m_context{context}
  {
  }

  ~OffscreenContextScope() { m_context.doneCurrent(); }

  OffscreenContextScope(const OffscreenContextScope&) = delete;
  OffscreenContextScope& operator=(const OffscreenContextScope&) = delete;
};

/**
 * A framebuffer object of the given size with a depth and stencil buffer, multisampled
 * if multisampling is on and the GL can resolve a multisampled framebuffer object into an
 * image, which needs the framebuffer blit extension, else without multisampling.
 */
std::unique_ptr<QOpenGLFramebufferObject> makeFramebuffer(const cli::ImageSize size)
{
  const auto makeWithSamples = [&](const int samples) {
    auto format = QOpenGLFramebufferObjectFormat{};
    format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    format.setSamples(samples);
    return std::make_unique<QOpenGLFramebufferObject>(size.width, size.height, format);
  };

  if (
    pref(Preferences::EnableMSAA) && QOpenGLFramebufferObject::hasOpenGLFramebufferBlit())
  {
    if (auto framebuffer = makeWithSamples(MultisampleSamples); framebuffer->isValid())
    {
      return framebuffer;
    }
  }
  return makeWithSamples(0);
}

void clearBackground(gl::Gl& gl)
{
  const auto backgroundColor = pref(Preferences::BackgroundColor).to<RgbaF>();
  gl.clearColor(
    backgroundColor.get<ColorChannel::r>(),
    backgroundColor.get<ColorChannel::g>(),
    backgroundColor.get<ColorChannel::b>(),
    backgroundColor.get<ColorChannel::a>());
  gl.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

} // namespace

cli::ViewCapture renderMapOffscreen(
  AppController& appController, MapDocument& document, const cli::CameraPose& pose)
{
  auto* context = appController.makeOffscreenContextCurrent();
  if (!context)
  {
    throw cli::CliError{
      cli::ErrorCode::ViewFailed,
      "the editor cannot render yet; it renders once a map view has been shown"};
  }
  const auto contextScope = OffscreenContextScope{*context};

  auto framebuffer = makeFramebuffer(pose.viewport);
  if (!framebuffer->isValid() || !framebuffer->bind())
  {
    throw cli::CliError{
      cli::ErrorCode::ViewFailed, "the editor cannot create an offscreen framebuffer"};
  }

  auto& glManager = appController.glManager();
  auto gl = GlQt{getGlFunctions("renderMapOffscreen", context)};
  const auto camera = cli::makeCamera(pose);
  const auto* orthographicCamera =
    dynamic_cast<const gl::OrthographicCamera*>(camera.get());
  {
    auto renderContext = render::RenderContext{
      gl,
      orthographicCamera ? render::RenderMode::Render2D : render::RenderMode::Render3D,
      *camera,
      glManager.fontManager(),
      glManager.shaderManager()};
    // the image is the viewport, one pixel per logical pixel
    setupMapRenderContext(renderContext, document.map(), 1.0f);
    setupMapRenderGL(gl, *camera, 1.0f);
    clearBackground(gl);

    auto renderBatch = render::RenderBatch{glManager.vboManager()};
    if (orthographicCamera)
    {
      renderBatch.addOneShot(
        std::make_unique<render::GridRenderer>(
          *orthographicCamera, document.map().worldBounds()));
    }
    document.mapRenderer().render(renderContext, renderBatch);
    renderBatch.render(renderContext);
  }
  framebuffer->release();

  // resolves the samples of a multisampled framebuffer; the map has no transparent
  // background, so the image needs no alpha channel
  const auto image = framebuffer->toImage().convertToFormat(QImage::Format_RGB32);
  return makeViewCapture(image, *camera, 1.0f, mdl::PickResult{});
}

} // namespace tb::ui
