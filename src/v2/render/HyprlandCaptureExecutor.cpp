#include "v2/render/HyprlandCaptureExecutor.hpp"

#include "v2/render/CaptureBlit.hpp"
#include "v2/render/HyprlandStateGuard.hpp"

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <GLES3/gl32.h>

#include <array>
#include <cmath>
#include <utility>

namespace hfg::v2 {
namespace {

Result<void> failure(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<void>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

} // namespace

Result<void> captureCurrentFramebuffer(
    HyprlandCaptureResource& resource,
    const OutputGeneration& output,
    std::span<const PixelRect> outputDamage) {
    if (!resource.allocated())
        return failure(
            ErrorCode::InvalidRequest,
            "resource",
            "capture resource is not allocated");
    const auto blits = captureUpdateBlits(
        resource.plan(),
        output,
        outputDamage,
        resource.initialized());
    if (!blits) {
        resource.invalidate();
        return Result<void>::failure(blits.error());
    }
    if (blits.value().empty())
        return Result<void>::success();
    if (!g_pHyprRenderer || !Render::GL::g_pHyprOpenGL)
        return failure(
            ErrorCode::UnsupportedOperation,
            "renderer",
            "Hyprland OpenGL renderer is unavailable");
    const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!monitor || monitor->m_name != output.snapshot.name)
        return failure(
            ErrorCode::StaleGeneration,
            "renderer.output",
            "current render output does not match the capture plan");
    const auto sourceFramebuffer = g_pHyprRenderer->m_renderData.currentFB;
    if (!sourceFramebuffer ||
        !sourceFramebuffer->isAllocated() ||
        sourceFramebuffer->m_drmFormat != resource.plan().key.renderFormat)
        return failure(
            ErrorCode::UnsupportedOperation,
            "renderer.framebuffer",
            "current framebuffer does not preserve the planned render format");
    const auto sourceTexture = sourceFramebuffer->getTexture();
    if (!sourceTexture ||
        !sourceTexture->ok() ||
        sourceTexture->m_size.x != output.snapshot.bufferWidth ||
        sourceTexture->m_size.y != output.snapshot.bufferHeight)
        return failure(
            ErrorCode::StaleGeneration,
            "renderer.framebuffer_size",
            "current framebuffer size does not match the output generation");
    const auto imageDescription = sourceFramebuffer->imageDescription();
    if (!imageDescription ||
        imageDescription->id() != resource.plan().key.colorStateToken)
        return failure(
            ErrorCode::StaleGeneration,
            "renderer.color_state",
            "current framebuffer color state does not match the output generation");

    GLint sourceDrawFramebuffer = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &sourceDrawFramebuffer);
    if (sourceDrawFramebuffer == 0)
        return failure(
            ErrorCode::UnsupportedOperation,
            "renderer.framebuffer",
            "current draw framebuffer is not capturable");

    const std::array<std::uint32_t, 0> textureUnits{};
    auto state = HyprlandStateGuard::captureWithoutShaderMutation(textureUnits);
    if (!state)
        return Result<void>::failure(state.error());

    Render::GL::g_pHyprOpenGL->scissor(
        static_cast<const pixman_box32*>(nullptr),
        false);
    glBindFramebuffer(
        GL_READ_FRAMEBUFFER,
        static_cast<GLuint>(sourceDrawFramebuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resource.framebuffer());

    const auto readStatus = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    const auto drawStatus = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    GLenum blitError = GL_NO_ERROR;
    if (readStatus == GL_FRAMEBUFFER_COMPLETE &&
        drawStatus == GL_FRAMEBUFFER_COMPLETE) {
        for (const auto& blit : blits.value()) {
            glBlitFramebuffer(
                blit.source.x0,
                blit.source.y0,
                blit.source.x1,
                blit.source.y1,
                blit.destination.x0,
                blit.destination.y0,
                blit.destination.x1,
                blit.destination.y1,
                GL_COLOR_BUFFER_BIT,
                GL_NEAREST);
            blitError = glGetError();
            if (blitError != GL_NO_ERROR)
                break;
        }
    }

    if (auto restored = state.value()->restore(); !restored)
        return restored;
    if (readStatus != GL_FRAMEBUFFER_COMPLETE) {
        resource.invalidate();
        return failure(
            ErrorCode::InternalError,
            "renderer.source_framebuffer",
            "source framebuffer is incomplete");
    }
    if (drawStatus != GL_FRAMEBUFFER_COMPLETE) {
        resource.invalidate();
        return failure(
            ErrorCode::InternalError,
            "resource.framebuffer",
            "capture framebuffer became incomplete");
    }
    if (blitError != GL_NO_ERROR) {
        resource.invalidate();
        return failure(
            ErrorCode::InternalError,
            "renderer.blit",
            "OpenGL rejected the bounded capture copy");
    }
    resource.markInitialized();
    return Result<void>::success();
}

} // namespace hfg::v2
