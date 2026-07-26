#include "v2/render/HyprlandGlassShader.hpp"

#include "v2/render/GlassShaderSource.hpp"

#include <hyprland/src/render/OpenGL.hpp>

#include <array>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

Result<void> failure(ErrorCode code, std::string path, std::string message) {
  return Result<void>::failure({
      .code = code,
      .path = std::move(path),
      .message = std::move(message),
  });
}

} // namespace

Result<void> HyprlandGlassShader::ensure() {
  if (ready())
    return Result<void>::success();
  if (!Render::GL::g_pHyprOpenGL)
    return failure(ErrorCode::UnsupportedOperation, "renderer",
                   "Hyprland OpenGL renderer is unavailable");

  auto shader = makeShared<CShader>();
  if (!shader || !shader->createProgram(
                     std::string(glassVertexShaderSource()),
                     std::string(glassFragmentShaderSource()), true, true))
    return failure(ErrorCode::UnsupportedOperation, "renderer.shader",
                   "v2 glass shader compilation or linking failed");
  shader->setUsesCustomUV(true);
  m_shader = std::move(shader);
  if (auto cached = cacheUniforms(); !cached) {
    reset();
    return cached;
  }

  const auto vertexArray = m_shader->getUniformLocation(SHADER_SHADER_VAO);
  const auto vertexBuffer = m_shader->getUniformLocation(SHADER_SHADER_VBO);
  if (vertexArray <= 0 || vertexBuffer <= 0) {
    reset();
    return failure(ErrorCode::UnsupportedOperation,
                   "renderer.shader.vertex_buffer",
                   "v2 glass shader did not allocate a tracked quad");
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

void HyprlandGlassShader::reset() noexcept {
  m_shader.reset();
  m_uniforms = {};
}

bool HyprlandGlassShader::ready() const noexcept {
  return m_shader && m_shader->program() != 0U;
}

const SP<CShader> &HyprlandGlassShader::shader() const noexcept {
  return m_shader;
}

const GlassShaderUniforms &HyprlandGlassShader::uniforms() const noexcept {
  return m_uniforms;
}

Result<void> HyprlandGlassShader::cacheUniforms() {
  if (!ready())
    return failure(ErrorCode::InternalError, "renderer.shader",
                   "cannot cache uniforms for an unavailable shader");
  const auto program = m_shader->program();
  const auto uniform = [program](const char *name) {
    return glGetUniformLocation(program, name);
  };
  m_uniforms = {
      .capture = uniform("uCapture"),
      .sourceTL = uniform("uSourceTL"),
      .sourceTR = uniform("uSourceTR"),
      .sourceBR = uniform("uSourceBR"),
      .sourceBL = uniform("uSourceBL"),
      .fullSize = uniform("uFullSize"),
      .clipOffset = uniform("uClipOffset"),
      .clipSize = uniform("uClipSize"),
      .shapeKind = uniform("uShapeKind"),
      .radius = uniform("uRadius"),
      .roundingPower = uniform("uRoundingPower"),
      .ringRadius = uniform("uRingRadius"),
      .ringThickness = uniform("uRingThickness"),
      .baseEnabled = uniform("uBaseEnabled"),
      .baseRadii = uniform("uBaseRadii"),
      .cutoutEnabled = uniform("uCutoutEnabled"),
      .cutoutRect = uniform("uCutoutRect"),
      .cutoutRadii = uniform("uCutoutRadii"),
      .partCount = uniform("uPartCount"),
      .partRects = uniform("uPartRects[0]"),
      .partRadii = uniform("uPartRadii[0]"),
      .partJunctions = uniform("uPartJunctions[0]"),
      .partMaterialExtents = uniform("uPartMaterialExtents[0]"),
      .partOpacity = uniform("uPartOpacity[0]"),
      .connectorCount = uniform("uConnectorCount"),
      .connectorRects = uniform("uConnectorRects[0]"),
      .connectorCurve = uniform("uConnectorCurve"),
      .blurPixels = uniform("uBlurPixels"),
      .refractionPixels = uniform("uRefractionPixels"),
      .edgeBandPixels = uniform("uEdgeBandPixels"),
      .bevelPixels = uniform("uBevelPixels"),
      .rimWidthPixels = uniform("uRimWidthPixels"),
      .lensBandPixels = uniform("uLensBandPixels"),
      .highlight = uniform("uHighlight"),
      .shadow = uniform("uShadow"),
      .specular = uniform("uSpecular"),
      .chroma = uniform("uChroma"),
      .edgeDepth = uniform("uEdgeDepth"),
      .lens = uniform("uLens"),
      .gloss = uniform("uGloss"),
      .tint = uniform("uTint"),
      .veilSaturation = uniform("uVeilSaturation"),
      .lightDirection = uniform("uLightDirection"),
      .opacity = uniform("uOpacity"),
  };

  const std::array required{
      m_uniforms.capture,
      m_uniforms.sourceTL,
      m_uniforms.sourceTR,
      m_uniforms.sourceBR,
      m_uniforms.sourceBL,
      m_uniforms.fullSize,
      m_uniforms.clipOffset,
      m_uniforms.clipSize,
      m_uniforms.shapeKind,
      m_uniforms.radius,
      m_uniforms.roundingPower,
      m_uniforms.ringRadius,
      m_uniforms.ringThickness,
      m_uniforms.baseEnabled,
      m_uniforms.baseRadii,
      m_uniforms.cutoutEnabled,
      m_uniforms.cutoutRect,
      m_uniforms.cutoutRadii,
      m_uniforms.partCount,
      m_uniforms.partRects,
      m_uniforms.partRadii,
      m_uniforms.partJunctions,
      m_uniforms.partMaterialExtents,
      m_uniforms.partOpacity,
      m_uniforms.connectorCount,
      m_uniforms.connectorRects,
      m_uniforms.connectorCurve,
      m_uniforms.blurPixels,
      m_uniforms.refractionPixels,
      m_uniforms.edgeBandPixels,
      m_uniforms.bevelPixels,
      m_uniforms.rimWidthPixels,
      m_uniforms.lensBandPixels,
      m_uniforms.highlight,
      m_uniforms.shadow,
      m_uniforms.specular,
      m_uniforms.chroma,
      m_uniforms.edgeDepth,
      m_uniforms.lens,
      m_uniforms.gloss,
      m_uniforms.tint,
      m_uniforms.veilSaturation,
      m_uniforms.lightDirection,
      m_uniforms.opacity,
  };
  for (const auto location : required)
    if (location < 0)
      return failure(ErrorCode::UnsupportedOperation,
                     "renderer.shader.uniforms",
                     "v2 glass shader is missing a required active uniform");
  return Result<void>::success();
}

} // namespace hfg::v2
