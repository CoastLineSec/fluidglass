#include "v2/render/HyprlandCaptureResource.hpp"

#include "v2/render/HyprlandCaptureFormat.hpp"

#include <hyprgraphics/egl/Egl.hpp>
#include <hyprland/src/render/OpenGL.hpp>

#include <utility>

namespace hfg::v2 {
namespace {

Result<std::unique_ptr<HyprlandCaptureResource>> failure(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<std::unique_ptr<HyprlandCaptureResource>>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

} // namespace

HyprlandCaptureResource::HyprlandCaptureResource(
    CapturePlan plan,
    GLuint framebuffer,
    GLuint texture) :
    m_plan(std::move(plan)),
    m_framebuffer(framebuffer),
    m_texture(texture) {}

HyprlandCaptureResource::~HyprlandCaptureResource() {
    release();
}

Result<std::unique_ptr<HyprlandCaptureResource>>
HyprlandCaptureResource::allocate(CapturePlan plan) {
    if (auto validation = validateCapturePlan(plan); !validation)
        return failure(
            validation.error().code,
            validation.error().path,
            validation.error().message);
    if (!Render::GL::g_pHyprOpenGL)
        return failure(
            ErrorCode::UnsupportedOperation,
            "renderer",
            "Hyprland OpenGL renderer is unavailable");

    const auto* format = Hyprgraphics::Egl::getPixelFormatFromDRM(
        plan.key.renderFormat);
    const auto layout =
        hyprlandCaptureFormatLayout(plan.key.renderFormat);
    if (!format ||
        !layout ||
        layout.value().bytesPerPixel != plan.bytesPerPixel)
        return failure(
            ErrorCode::UnsupportedOperation,
            "plan.key.render_format",
            "render format cannot be represented by a matching capture texture");

    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    GLint maximumTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumTextureSize);
    if (maximumTextureSize <= 0 ||
        plan.region.width > maximumTextureSize ||
        plan.region.height > maximumTextureSize)
        return failure(
            ErrorCode::ResourceLimited,
            "plan.region",
            "capture dimensions exceed the OpenGL texture limit");

    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    GLint previousDrawFramebuffer = 0;
    GLint previousReadFramebuffer = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);

    GLuint texture = 0;
    GLuint framebuffer = 0;
    glGenTextures(1, &texture);
    glGenFramebuffers(1, &framebuffer);
    if (texture != 0U && framebuffer != 0U) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            format->glInternalFormat != 0
                ? format->glInternalFormat
                : format->glFormat,
            plan.region.width,
            plan.region.height,
            0,
            format->glFormat,
            format->glType,
            nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D,
            texture,
            0);
    }
    const auto framebufferStatus = framebuffer != 0U
        ? glCheckFramebufferStatus(GL_FRAMEBUFFER)
        : GL_FRAMEBUFFER_UNDEFINED;

    glBindFramebuffer(
        GL_READ_FRAMEBUFFER,
        static_cast<GLuint>(previousReadFramebuffer));
    glBindFramebuffer(
        GL_DRAW_FRAMEBUFFER,
        static_cast<GLuint>(previousDrawFramebuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(previousActiveTexture);

    if (texture == 0U ||
        framebuffer == 0U ||
        framebufferStatus != GL_FRAMEBUFFER_COMPLETE) {
        if (framebuffer != 0U)
            glDeleteFramebuffers(1, &framebuffer);
        if (texture != 0U)
            glDeleteTextures(1, &texture);
        return failure(
            ErrorCode::UnsupportedOperation,
            "plan.key.render_format",
            "render format is not framebuffer-renderable on this GPU");
    }

    return Result<std::unique_ptr<HyprlandCaptureResource>>::success(
        std::unique_ptr<HyprlandCaptureResource>(
            new HyprlandCaptureResource(
                std::move(plan),
                framebuffer,
                texture)));
}

const CapturePlan& HyprlandCaptureResource::plan() const noexcept {
    return m_plan;
}

GLuint HyprlandCaptureResource::framebuffer() const noexcept {
    return m_framebuffer;
}

GLuint HyprlandCaptureResource::texture() const noexcept {
    return m_texture;
}

bool HyprlandCaptureResource::allocated() const noexcept {
    return m_framebuffer != 0U && m_texture != 0U;
}

bool HyprlandCaptureResource::initialized() const noexcept {
    return allocated() && m_initialized;
}

void HyprlandCaptureResource::markInitialized() noexcept {
    m_initialized = allocated();
}

void HyprlandCaptureResource::invalidate() noexcept {
    m_initialized = false;
}

void HyprlandCaptureResource::release() noexcept {
    if (m_framebuffer == 0U && m_texture == 0U)
        return;
    if (!Render::GL::g_pHyprOpenGL) {
        m_framebuffer = 0U;
        m_texture = 0U;
        m_initialized = false;
        return;
    }
    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    if (m_framebuffer != 0U)
        glDeleteFramebuffers(1, &m_framebuffer);
    if (m_texture != 0U)
        glDeleteTextures(1, &m_texture);
    m_framebuffer = 0U;
    m_texture = 0U;
    m_initialized = false;
}

} // namespace hfg::v2
