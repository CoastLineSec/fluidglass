#include "v2/render/HyprlandGlassDrawExecutor.hpp"

#include "v2/core/Limits.hpp"
#include "v2/render/GlassUniformPayload.hpp"
#include "v2/render/HyprlandStateGuard.hpp"

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <GLES3/gl32.h>

#include <array>
#include <span>

namespace hfg::v2 {
namespace {

Result<bool> restoreFailure(HyprlandStateGuard &state, const Error &original) {
  if (auto restored = state.restore(); !restored)
    return Result<bool>::failure(restored.error());
  return Result<bool>::failure(original);
}

void uploadVec2(GLint location, const UniformVec2 &value) {
  glUniform2f(location, value.x, value.y);
}

void uploadVec4(GLint location, const UniformVec4 &value) {
  glUniform4f(location, value.x, value.y, value.z, value.w);
}

template <std::size_t Size>
std::array<float, Size * 4U>
flatten(const std::array<UniformVec4, Size> &values) {
  std::array<float, Size * 4U> flattened{};
  for (std::size_t index = 0; index < Size; ++index) {
    const auto offset = index * 4U;
    flattened[offset] = values[index].x;
    flattened[offset + 1U] = values[index].y;
    flattened[offset + 2U] = values[index].z;
    flattened[offset + 3U] = values[index].w;
  }
  return flattened;
}

void uploadUniforms(const GlassShaderUniforms &locations,
                    const GlassUniformPayload &values) {
  glUniform1i(locations.capture, 0);
  uploadVec2(locations.sourceTL, values.sourceCorners[0]);
  uploadVec2(locations.sourceTR, values.sourceCorners[1]);
  uploadVec2(locations.sourceBR, values.sourceCorners[2]);
  uploadVec2(locations.sourceBL, values.sourceCorners[3]);
  uploadVec2(locations.fullSize, values.fullSize);
  uploadVec2(locations.clipOffset, values.clipOffset);
  uploadVec2(locations.clipSize, values.clipSize);

  glUniform1i(locations.shapeKind, values.shapeKind);
  glUniform1f(locations.radius, values.radius);
  glUniform1f(locations.roundingPower, values.roundingPower);
  glUniform1f(locations.ringRadius, values.ringRadius);
  glUniform1f(locations.ringThickness, values.ringThickness);
  glUniform1i(locations.baseEnabled, values.baseEnabled);
  uploadVec4(locations.baseRadii, values.baseRadii);
  glUniform1i(locations.cutoutEnabled, values.cutoutEnabled);
  uploadVec4(locations.cutoutRect, values.cutoutRect);
  uploadVec4(locations.cutoutRadii, values.cutoutRadii);

  const auto partRects = flatten(values.partRects);
  const auto partRadii = flatten(values.partRadii);
  const auto partJunctions = flatten(values.partJunctions);
  const auto partMaterialExtents = flatten(values.partMaterialExtents);
  glUniform1i(locations.partCount, values.partCount);
  glUniform4fv(locations.partRects,
               static_cast<GLsizei>(Limits::MAX_COMPOUND_PARTS),
               partRects.data());
  glUniform4fv(locations.partRadii,
               static_cast<GLsizei>(Limits::MAX_COMPOUND_PARTS),
               partRadii.data());
  glUniform4fv(locations.partJunctions,
               static_cast<GLsizei>(Limits::MAX_COMPOUND_PARTS),
               partJunctions.data());
  glUniform4fv(locations.partMaterialExtents,
               static_cast<GLsizei>(Limits::MAX_COMPOUND_PARTS),
               partMaterialExtents.data());
  glUniform1fv(locations.partOpacity,
               static_cast<GLsizei>(Limits::MAX_COMPOUND_PARTS),
               values.partOpacity.data());

  const auto connectorRects = flatten(values.connectorRects);
  glUniform1i(locations.connectorCount, values.connectorCount);
  glUniform4fv(locations.connectorRects,
               static_cast<GLsizei>(Limits::MAX_COMPOUND_CONNECTORS),
               connectorRects.data());
  glUniform1f(locations.connectorCurve, values.connectorCurve);

  glUniform1f(locations.refractionPixels, values.refractionPixels);
  glUniform1f(locations.edgeBandPixels, values.edgeBandPixels);
  glUniform1f(locations.bevelPixels, values.bevelPixels);
  glUniform1f(locations.rimWidthPixels, values.rimWidthPixels);
  glUniform1f(locations.lensBandPixels, values.lensBandPixels);
  glUniform1f(locations.highlight, values.highlight);
  glUniform1f(locations.shadow, values.shadow);
  glUniform1f(locations.specular, values.specular);
  glUniform1f(locations.chroma, values.chroma);
  glUniform1f(locations.edgeDepth, values.edgeDepth);
  glUniform1f(locations.lens, values.lens);
  glUniform1f(locations.gloss, values.gloss);
  uploadVec4(locations.tint, values.tint);
  glUniform1f(locations.veilSaturation, values.veilSaturation);
  uploadVec2(locations.lightDirection, values.lightDirection);
  glUniform1f(locations.opacity, values.opacity);
}

Result<void> validateRuntime(const GlassDrawPlan &plan,
                             std::uint64_t resourceToken,
                             const HyprlandCaptureResource &resource,
                             const OutputGeneration &output) {
  if (resourceToken == 0U || resourceToken != plan.resourceToken)
    return Result<void>::failure({
        ErrorCode::StaleGeneration,
        "resource.token",
        "selected capture token differs from the draw plan",
    });
  if (!resource.allocated() || !(resource.plan() == plan.capture))
    return Result<void>::failure({
        ErrorCode::StaleGeneration,
        "resource.plan",
        "selected capture allocation differs from the draw plan",
    });
  if (auto valid = validateOutputSnapshot(output.snapshot); !valid)
    return valid;
  if (plan.key.output != output.snapshot.name ||
      plan.key.outputGeneration != output.generation ||
      plan.capture.key.output != output.snapshot.name ||
      plan.capture.key.outputGeneration != output.generation ||
      plan.capture.key.renderFormat != output.snapshot.renderFormat ||
      plan.capture.key.colorStateToken != output.snapshot.colorStateToken)
    return Result<void>::failure({
        ErrorCode::StaleGeneration,
        "output",
        "draw or capture plan differs from the output generation",
    });
  if (!g_pHyprRenderer || !Render::GL::g_pHyprOpenGL)
    return Result<void>::failure({
        ErrorCode::UnsupportedOperation,
        "renderer",
        "Hyprland OpenGL renderer is unavailable",
    });

  const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
  if (!monitor || monitor->m_name != output.snapshot.name)
    return Result<void>::failure({
        ErrorCode::StaleGeneration,
        "renderer.output",
        "current render output differs from the draw plan",
    });
  const auto framebuffer = g_pHyprRenderer->m_renderData.currentFB;
  if (!framebuffer || !framebuffer->isAllocated() ||
      framebuffer->m_drmFormat != output.snapshot.renderFormat)
    return Result<void>::failure({
        ErrorCode::UnsupportedOperation,
        "renderer.framebuffer",
        "current framebuffer does not preserve the planned render format",
    });
  const auto texture = framebuffer->getTexture();
  if (!texture || !texture->ok() ||
      texture->m_size.x != output.snapshot.bufferWidth ||
      texture->m_size.y != output.snapshot.bufferHeight)
    return Result<void>::failure({
        ErrorCode::StaleGeneration,
        "renderer.framebuffer_size",
        "current framebuffer size differs from the output generation",
    });
  const auto description = framebuffer->imageDescription();
  if (!description || description->id() != output.snapshot.colorStateToken)
    return Result<void>::failure({
        ErrorCode::StaleGeneration,
        "renderer.color_state",
        "current framebuffer color state differs from the output generation",
    });
  return Result<void>::success();
}

} // namespace

Result<bool> drawGlass(const GlassDrawPlan &plan, std::uint64_t resourceToken,
                       const HyprlandCaptureResource &resource,
                       const OutputGeneration &output,
                       HyprlandGlassShader &shader,
                       HyprlandGlassBlur &blur) {
  if (auto valid = validateRuntime(plan, resourceToken, resource, output);
      !valid)
    return Result<bool>::failure(valid.error());

  auto payload = buildGlassUniformPayload(plan);
  if (!payload)
    return Result<bool>::failure(payload.error());

  CRegion damage{g_pHyprRenderer->m_renderData.damage};
  damage.intersect(plan.damageCoverage.x, plan.damageCoverage.y,
                   plan.damageCoverage.width, plan.damageCoverage.height);
  if (damage.empty())
    return Result<bool>::success(false);

  constexpr std::array<std::uint32_t, 1> TEXTURE_UNITS{0U};
  auto state = HyprlandStateGuard::captureWithShaderMutation(
      std::span<const SP<CShader>>{}, TEXTURE_UNITS);
  if (!state)
    return Result<bool>::failure(state.error());

  if (auto ready = shader.ensure(); !ready)
    return restoreFailure(*state.value(), ready.error());
  const auto &tracked = shader.shader();
  if (!tracked || tracked->program() == 0U)
    return restoreFailure(
        *state.value(),
        Error{
            ErrorCode::InternalError,
            "renderer.shader",
            "v2 glass shader is not available after compilation",
        });

  CBox box{
      plan.destinationPixels.x,
      plan.destinationPixels.y,
      plan.destinationPixels.width,
      plan.destinationPixels.height,
  };
  g_pHyprRenderer->m_renderData.renderModif.applyToBox(box);
  const auto projection = g_pHyprRenderer->projectBoxToTarget(box);

  const auto active = Render::GL::g_pHyprOpenGL->useShader(tracked);
  if (!active || active->program() == 0U)
    return restoreFailure(*state.value(),
                          Error{
                              ErrorCode::UnsupportedOperation,
                              "renderer.shader",
                              "Hyprland rejected the v2 glass shader",
                          });

  // Frost first, then draw. A failed blur is not fatal: the sharp capture is
  // still a correct backdrop, so the surface degrades to unfrosted glass
  // rather than losing its material, matching v1's fallback.
  GLuint backdrop = resource.texture();
  if (payload.value().blurPixels >= 0.5F) {
    auto frosted = blur.execute(
        resource.texture(),
        plan.capture.region.width,
        plan.capture.region.height,
        payload.value().blurPixels);
    if (frosted)
      backdrop = frosted.value();
  }

  // The blur passes rebind the program and vertex array, so the glass shader
  // has to be reselected before its own uniforms are uploaded.
  const auto redrawn = Render::GL::g_pHyprOpenGL->useShader(tracked);
  if (!redrawn || redrawn->program() == 0U)
    return restoreFailure(*state.value(),
                          Error{
                              ErrorCode::UnsupportedOperation,
                              "renderer.shader",
                              "Hyprland rejected the v2 glass shader",
                          });

  active->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, projection.getMatrix());
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, backdrop);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  uploadUniforms(shader.uniforms(), payload.value());

  g_pHyprRenderer->blend(true);
  glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
  glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                      GL_ONE_MINUS_SRC_ALPHA);
  glBindVertexArray(
      static_cast<GLuint>(active->getUniformLocation(SHADER_SHADER_VAO)));

  bool drew = false;
  damage.forEachRect([&](const auto &rect) {
    Render::GL::g_pHyprOpenGL->scissor(
        &rect,
        g_pHyprRenderer->m_renderData.transformDamage);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    drew = true;
  });

  if (auto restored = state.value()->restore(); !restored)
    return Result<bool>::failure(restored.error());
  return Result<bool>::success(drew);
}

} // namespace hfg::v2
