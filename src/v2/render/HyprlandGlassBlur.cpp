#include "v2/render/HyprlandGlassBlur.hpp"

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace hfg::v2 {
namespace {

/** Half-resolution taps per side. Reach is 2x this at full resolution. */
constexpr int MAX_TAPS = 24;

Result<void> failure(ErrorCode code, std::string path, std::string message) {
  return Result<void>::failure({
      .code = code,
      .path = std::move(path),
      .message = std::move(message),
  });
}

std::string_view blurVertexSource() {
  return R"GLSL(#version 320 es
in vec2 pos;
in vec2 texcoord;
out vec2 vTexcoord;
void main() {
    vTexcoord = texcoord;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";
}

std::string_view blurFragmentSource() {
  return R"GLSL(#version 320 es
precision highp float;
in vec2 vTexcoord;
uniform sampler2D uSource;
uniform vec2 uDirection;
uniform float uRadius;
out vec4 fragColor;

void main() {
    float radius = max(uRadius, 0.0);
    if (radius < 0.5) {
        fragColor = texture(uSource, vTexcoord);
        return;
    }

    // Gaussian truncated at two standard deviations, which is where the tail
    // stops contributing visibly. Weights are accumulated and divided out so a
    // clamped tap count stays energy-preserving instead of darkening the edge.
    float sigma = radius * 0.5;
    float twoSigmaSquared = 2.0 * sigma * sigma;
    int taps = int(min(ceil(radius), float(MAX_TAPS)));
    float step = radius / max(float(taps), 1.0);

    vec4 total = texture(uSource, vTexcoord);
    float weightSum = 1.0;
    for (int index = 1; index <= MAX_TAPS; ++index) {
        if (index > taps)
            break;
        float offset = float(index) * step;
        float weight = exp(-(offset * offset) / twoSigmaSquared);
        vec2 delta = uDirection * offset;
        total += texture(uSource, vTexcoord + delta) * weight;
        total += texture(uSource, vTexcoord - delta) * weight;
        weightSum += weight * 2.0;
    }
    fragColor = total / weightSum;
}
)GLSL";
}

} // namespace

HyprlandGlassBlur::~HyprlandGlassBlur() { reset(); }

Result<void> HyprlandGlassBlur::ensureShader() {
  if (m_shader && m_shader->program() != 0U)
    return Result<void>::success();
  if (!Render::GL::g_pHyprOpenGL)
    return failure(ErrorCode::UnsupportedOperation, "renderer",
                   "Hyprland OpenGL renderer is unavailable");

  auto shader = makeShared<CShader>();
  // The version directive has to stay first, so the tap bound is injected
  // after it rather than prepended to the whole source.
  auto source = std::string(blurFragmentSource());
  const auto insertAt = source.find('\n');
  if (insertAt == std::string::npos)
    return failure(ErrorCode::InternalError, "renderer.blur_shader",
                   "blur fragment source is malformed");
  source.insert(insertAt + 1,
                "#define MAX_TAPS " + std::to_string(MAX_TAPS) + "\n");

  if (!shader || !shader->createProgram(std::string(blurVertexSource()), source,
                                        true, true))
    return failure(ErrorCode::UnsupportedOperation, "renderer.blur_shader",
                   "glass blur shader compilation or linking failed");
  shader->setUsesCustomUV(true);
  m_shader = std::move(shader);

  const auto program = m_shader->program();
  m_source = glGetUniformLocation(program, "uSource");
  m_direction = glGetUniformLocation(program, "uDirection");
  m_radius = glGetUniformLocation(program, "uRadius");
  if (m_source < 0 || m_direction < 0 || m_radius < 0) {
    reset();
    return failure(ErrorCode::InternalError, "renderer.blur_shader",
                   "glass blur shader is missing required uniforms");
  }

  const auto vertexArray = m_shader->getUniformLocation(SHADER_SHADER_VAO);
  const auto vertexBuffer = m_shader->getUniformLocation(SHADER_SHADER_VBO);
  if (vertexArray <= 0 || vertexBuffer <= 0) {
    reset();
    return failure(ErrorCode::UnsupportedOperation,
                   "renderer.blur_shader.vertex_buffer",
                   "glass blur shader did not allocate a tracked quad");
  }

  GLint previousVertexArray = 0;
  GLint previousArrayBuffer = 0;
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
  glBindVertexArray(static_cast<GLuint>(vertexArray));
  glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(vertexBuffer));
  auto vertices = Render::GL::fullVerts;
  vertices[0].u = 0.0F;
  vertices[0].v = 0.0F;
  vertices[1].u = 0.0F;
  vertices[1].v = 1.0F;
  vertices[2].u = 1.0F;
  vertices[2].v = 0.0F;
  vertices[3].u = 1.0F;
  vertices[3].v = 1.0F;
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(),
               GL_STATIC_DRAW);
  glBindVertexArray(static_cast<GLuint>(previousVertexArray));
  glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousArrayBuffer));
  return Result<void>::success();
}

Result<void> HyprlandGlassBlur::ensureTargets(std::int32_t width,
                                              std::int32_t height) {
  if (width <= 0 || height <= 0)
    return failure(ErrorCode::InvalidRequest, "blur.size",
                   "blur targets require positive dimensions");
  if (m_texture[0] != 0U && m_width == width && m_height == height)
    return Result<void>::success();

  releaseTargets();

  GLint maximumTextureSize = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumTextureSize);
  if (maximumTextureSize <= 0 || width > maximumTextureSize ||
      height > maximumTextureSize)
    return failure(ErrorCode::ResourceLimited, "blur.size",
                   "blur dimensions exceed the OpenGL texture limit");

  GLint previousTexture = 0;
  GLint previousDrawFramebuffer = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);

  bool complete = true;
  for (std::size_t index = 0; index < 2U; ++index) {
    glGenTextures(1, &m_texture[index]);
    glGenFramebuffers(1, &m_framebuffer[index]);
    if (m_texture[index] == 0U || m_framebuffer[index] == 0U) {
      complete = false;
      break;
    }
    glBindTexture(GL_TEXTURE_2D, m_texture[index]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_framebuffer[index]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_texture[index], 0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) !=
        GL_FRAMEBUFFER_COMPLETE) {
      complete = false;
      break;
    }
  }

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                    static_cast<GLuint>(previousDrawFramebuffer));
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));

  if (!complete) {
    releaseTargets();
    return failure(ErrorCode::UnsupportedOperation, "blur.targets",
                   "blur attachments are not renderable on this GPU");
  }

  m_width = width;
  m_height = height;
  return Result<void>::success();
}

Result<GLuint> HyprlandGlassBlur::execute(GLuint sourceTexture,
                                          std::int32_t sourceWidth,
                                          std::int32_t sourceHeight,
                                          double radiusPixels) {
  if (sourceTexture == 0U)
    return Result<GLuint>::failure({
        ErrorCode::InvalidRequest,
        "blur.source",
        "blur requires an allocated source texture",
    });
  if (!g_pHyprRenderer)
    return Result<GLuint>::failure({
        ErrorCode::UnsupportedOperation,
        "renderer",
        "Hyprland renderer is unavailable",
    });
  if (auto ready = ensureShader(); !ready)
    return Result<GLuint>::failure(ready.error());

  const auto width = std::max(1, (sourceWidth + 1) / 2);
  const auto height = std::max(1, (sourceHeight + 1) / 2);
  if (auto ready = ensureTargets(width, height); !ready)
    return Result<GLuint>::failure(ready.error());

  // The passes run at half resolution, so the reach halves with them. The
  // downsample itself contributes part of the smoothing, which is why this
  // matches v1's appearance at a fraction of the sample count.
  const auto radius = radiusPixels * 0.5;

  GLint previousViewport[4] = {0, 0, 0, 0};
  GLint previousDrawFramebuffer = 0;
  GLint previousActiveTexture = GL_TEXTURE0;
  glGetIntegerv(GL_VIEWPORT, previousViewport);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);

  const auto active = Render::GL::g_pHyprOpenGL->useShader(m_shader);
  if (!active || active->program() == 0U) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFramebuffer));
    return Result<GLuint>::failure({
        ErrorCode::UnsupportedOperation,
        "renderer.blur_shader",
        "Hyprland rejected the glass blur shader",
    });
  }

  // Hyprland caches the blend flag, so mutations must use its state-aware
  // setter.
  g_pHyprRenderer->blend(false);

  // The scissor box is restored exactly, so whatever Hyprland believes about
  // it after this stays true.
  const auto scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
  GLint previousScissor[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_SCISSOR_BOX, previousScissor);
  glDisable(GL_SCISSOR_TEST);

  glViewport(0, 0, width, height);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(m_source, 0);
  glBindVertexArray(
      static_cast<GLuint>(active->getUniformLocation(SHADER_SHADER_VAO)));

  // Horizontal into 0, then vertical from 0 into 1. Offsets are in the target's
  // own normalized space, which is why each pass steps by its own texel size.
  const struct {
    GLuint framebuffer;
    GLuint texture;
    float directionX;
    float directionY;
  } passes[2] = {
      {m_framebuffer[0], sourceTexture, 1.0F / static_cast<float>(width), 0.0F},
      {m_framebuffer[1], m_texture[0], 0.0F, 1.0F / static_cast<float>(height)},
  };

  for (const auto &pass : passes) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pass.framebuffer);
    glBindTexture(GL_TEXTURE_2D, pass.texture);
    glUniform2f(m_direction, pass.directionX, pass.directionY);
    glUniform1f(m_radius, static_cast<float>(radius));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                    static_cast<GLuint>(previousDrawFramebuffer));
  glViewport(previousViewport[0], previousViewport[1], previousViewport[2],
             previousViewport[3]);
  glScissor(previousScissor[0], previousScissor[1], previousScissor[2],
            previousScissor[3]);
  if (scissorWasEnabled)
    glEnable(GL_SCISSOR_TEST);
  glActiveTexture(previousActiveTexture);
  return Result<GLuint>::success(m_texture[1]);
}

void HyprlandGlassBlur::releaseTargets() noexcept {
  for (std::size_t index = 0; index < 2U; ++index) {
    if (m_framebuffer[index] != 0U)
      glDeleteFramebuffers(1, &m_framebuffer[index]);
    if (m_texture[index] != 0U)
      glDeleteTextures(1, &m_texture[index]);
    m_framebuffer[index] = 0U;
    m_texture[index] = 0U;
  }
  m_width = 0;
  m_height = 0;
}

void HyprlandGlassBlur::reset() noexcept {
  releaseTargets();
  m_shader.reset();
  m_source = -1;
  m_direction = -1;
  m_radius = -1;
}

} // namespace hfg::v2
