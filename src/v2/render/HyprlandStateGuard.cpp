#include "v2/render/HyprlandStateGuard.hpp"

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <GLES3/gl32.h>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace hfg::v2 {
namespace {

struct TextureBinding {
    std::uint32_t unit = 0;
    GLint         texture2D = 0;
};

SP<CShader> findTrackedShader(
    GLuint program,
    std::span<const SP<CShader>> additionalTrackedShaders) {
    for (const auto& shader : additionalTrackedShaders)
        if (shader && shader->program() == program)
            return shader;

    const auto& renderer = Render::GL::g_pHyprOpenGL;
    if (!renderer || !renderer->m_shaders)
        return nullptr;
    for (const auto& variants : renderer->m_shaders->fragVariants)
        for (const auto& [flags, shader] : variants) {
            static_cast<void>(flags);
            if (shader && shader->program() == program)
                return shader;
        }
    return nullptr;
}

Result<std::unique_ptr<HyprlandStateGuard>> unavailable(
    std::string path,
    std::string message) {
    return Result<std::unique_ptr<HyprlandStateGuard>>::failure({
        .code = ErrorCode::UnsupportedOperation,
        .path = std::move(path),
        .message = std::move(message),
    });
}

} // namespace

struct HyprlandStateGuard::Snapshot {
    SP<Render::IFramebuffer> framebuffer;
    SP<CShader>              shader;
    GLint                    readFramebuffer = 0;
    Render::eRenderProjectionType projectionType = Render::RPT_MONITOR;
    Vector2D                 framebufferSize;
    bool                     transformDamage = true;
    std::array<GLint, 4>     viewport{};
    bool                     scissorEnabled = false;
    std::array<GLint, 4>     scissorBox{};
    bool                     blendEnabled = false;
    GLint                    blendSourceRgb = GL_ONE;
    GLint                    blendDestinationRgb = GL_ZERO;
    GLint                    blendSourceAlpha = GL_ONE;
    GLint                    blendDestinationAlpha = GL_ZERO;
    GLint                    blendEquationRgb = GL_FUNC_ADD;
    GLint                    blendEquationAlpha = GL_FUNC_ADD;
    GLint                    activeTexture = GL_TEXTURE0;
    std::vector<TextureBinding> textureBindings;
    GLint                    vertexArray = 0;
    GLint                    arrayBuffer = 0;
};

HyprlandStateGuard::HyprlandStateGuard(
    std::unique_ptr<Snapshot> snapshot) :
    m_snapshot(std::move(snapshot)) {}

HyprlandStateGuard::~HyprlandStateGuard() {
    static_cast<void>(restore());
}

Result<std::unique_ptr<HyprlandStateGuard>>
HyprlandStateGuard::captureWithoutShaderMutation(
    std::span<const std::uint32_t> textureUnits) {
    return capture(false, {}, textureUnits);
}

Result<std::unique_ptr<HyprlandStateGuard>>
HyprlandStateGuard::captureWithShaderMutation(
    std::span<const SP<CShader>> additionalTrackedShaders,
    std::span<const std::uint32_t> textureUnits) {
    return capture(true, additionalTrackedShaders, textureUnits);
}

Result<std::unique_ptr<HyprlandStateGuard>> HyprlandStateGuard::capture(
    bool shaderWillChange,
    std::span<const SP<CShader>> additionalTrackedShaders,
    std::span<const std::uint32_t> textureUnits) {
    if (!g_pHyprRenderer || !Render::GL::g_pHyprOpenGL)
        return unavailable("renderer", "Hyprland OpenGL renderer is unavailable");
    if (!g_pHyprRenderer->m_renderData.pMonitor.lock())
        return unavailable("renderer.output", "current render output is unavailable");
    if (!g_pHyprRenderer->m_renderData.currentFB)
        return unavailable("renderer.framebuffer", "current framebuffer is unavailable");

    SP<CShader> shader;
    if (shaderWillChange) {
        GLint currentProgram = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
        if (currentProgram <= 0)
            return unavailable(
                "renderer.shader",
                "current shader program is not restorable through Hyprland");
        shader = findTrackedShader(
            static_cast<GLuint>(currentProgram),
            additionalTrackedShaders);
        if (!shader)
            return unavailable(
                "renderer.shader",
                "current shader is not registered with the tracked renderer");
    }

    GLint maximumTextureUnits = 0;
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maximumTextureUnits);
    if (maximumTextureUnits <= 0)
        return unavailable(
            "renderer.texture_units",
            "OpenGL did not report usable texture units");
    std::vector<std::uint32_t> normalizedTextureUnits;
    normalizedTextureUnits.reserve(textureUnits.size());
    for (const auto unit : textureUnits) {
        if (unit >= static_cast<std::uint32_t>(maximumTextureUnits))
            return unavailable(
                "renderer.texture_units",
                "requested texture unit exceeds the OpenGL limit");
        if (!std::ranges::contains(normalizedTextureUnits, unit))
            normalizedTextureUnits.push_back(unit);
    }

    auto snapshot = std::make_unique<Snapshot>();
    snapshot->framebuffer = g_pHyprRenderer->m_renderData.currentFB;
    snapshot->shader = std::move(shader);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &snapshot->readFramebuffer);
    snapshot->projectionType = g_pHyprRenderer->m_renderData.projectionType;
    snapshot->framebufferSize = g_pHyprRenderer->m_renderData.fbSize;
    snapshot->transformDamage = g_pHyprRenderer->m_renderData.transformDamage;
    glGetIntegerv(GL_VIEWPORT, snapshot->viewport.data());
    if (snapshot->viewport[2] <= 0 || snapshot->viewport[3] <= 0)
        return unavailable(
            "renderer.viewport",
            "current viewport is empty");
    snapshot->scissorEnabled = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    glGetIntegerv(GL_SCISSOR_BOX, snapshot->scissorBox.data());
    snapshot->blendEnabled = glIsEnabled(GL_BLEND) == GL_TRUE;
    glGetIntegerv(GL_BLEND_SRC_RGB, &snapshot->blendSourceRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &snapshot->blendDestinationRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &snapshot->blendSourceAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &snapshot->blendDestinationAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &snapshot->blendEquationRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &snapshot->blendEquationAlpha);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &snapshot->activeTexture);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &snapshot->vertexArray);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &snapshot->arrayBuffer);
    snapshot->textureBindings.reserve(normalizedTextureUnits.size());
    for (const auto unit : normalizedTextureUnits) {
        glActiveTexture(GL_TEXTURE0 + unit);
        GLint binding = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        snapshot->textureBindings.push_back({unit, binding});
    }
    glActiveTexture(snapshot->activeTexture);

    return Result<std::unique_ptr<HyprlandStateGuard>>::success(
        std::unique_ptr<HyprlandStateGuard>(
            new HyprlandStateGuard(std::move(snapshot))));
}

Result<void> HyprlandStateGuard::restore() {
    if (m_restored)
        return Result<void>::success();
    if (!m_snapshot)
        return Result<void>::failure({
            ErrorCode::InternalError,
            "renderer.state",
            "renderer state snapshot is unavailable",
        });
    if (!g_pHyprRenderer || !Render::GL::g_pHyprOpenGL)
        return Result<void>::failure({
            ErrorCode::InternalError,
            "renderer",
            "Hyprland OpenGL renderer disappeared before state restoration",
        });

    g_pHyprRenderer->bindFB(m_snapshot->framebuffer);
    glBindFramebuffer(
        GL_READ_FRAMEBUFFER,
        static_cast<GLuint>(m_snapshot->readFramebuffer));
    g_pHyprRenderer->m_renderData.fbSize = m_snapshot->framebufferSize;
    g_pHyprRenderer->m_renderData.transformDamage = m_snapshot->transformDamage;
    g_pHyprRenderer->setProjectionType(m_snapshot->projectionType);
    if (m_snapshot->shader)
        Render::GL::g_pHyprOpenGL->useShader(m_snapshot->shader);

    g_pHyprRenderer->blend(m_snapshot->blendEnabled);
    glBlendFuncSeparate(
        m_snapshot->blendSourceRgb,
        m_snapshot->blendDestinationRgb,
        m_snapshot->blendSourceAlpha,
        m_snapshot->blendDestinationAlpha);
    glBlendEquationSeparate(
        m_snapshot->blendEquationRgb,
        m_snapshot->blendEquationAlpha);
    if (m_snapshot->scissorEnabled)
        Render::GL::g_pHyprOpenGL->scissor(
            m_snapshot->scissorBox[0],
            m_snapshot->scissorBox[1],
            m_snapshot->scissorBox[2],
            m_snapshot->scissorBox[3],
            false);
    else
        Render::GL::g_pHyprOpenGL->scissor(
            static_cast<const pixman_box32*>(nullptr),
            false);
    g_pHyprRenderer->setViewport(
        m_snapshot->viewport[0],
        m_snapshot->viewport[1],
        m_snapshot->viewport[2],
        m_snapshot->viewport[3]);

    for (const auto& binding : m_snapshot->textureBindings) {
        glActiveTexture(GL_TEXTURE0 + binding.unit);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(binding.texture2D));
    }
    glActiveTexture(m_snapshot->activeTexture);
    glBindVertexArray(static_cast<GLuint>(m_snapshot->vertexArray));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(m_snapshot->arrayBuffer));

    m_restored = true;
    return Result<void>::success();
}

} // namespace hfg::v2
