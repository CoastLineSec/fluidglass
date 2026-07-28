#pragma once

#include "v2/core/Result.hpp"

#include <hyprland/src/render/Shader.hpp>

#include <GLES3/gl32.h>

namespace hfg::v2 {

struct GlassShaderUniforms {
  GLint capture = -1;
  GLint sourceTL = -1;
  GLint sourceTR = -1;
  GLint sourceBR = -1;
  GLint sourceBL = -1;
  GLint fullSize = -1;
  GLint clipOffset = -1;
  GLint clipSize = -1;
  GLint shapeKind = -1;
  GLint radius = -1;
  GLint roundingPower = -1;
  GLint ringRadius = -1;
  GLint ringThickness = -1;
  GLint baseEnabled = -1;
  GLint baseRadii = -1;
  GLint cutoutEnabled = -1;
  GLint cutoutRect = -1;
  GLint cutoutRadii = -1;
  GLint partCount = -1;
  GLint partRects = -1;
  GLint partRadii = -1;
  GLint partJunctions = -1;
  GLint partMaterialExtents = -1;
  GLint partOpacity = -1;
  GLint connectorCount = -1;
  GLint connectorRects = -1;
  GLint connectorCurve = -1;
  GLint refractionPixels = -1;
  GLint edgeBandPixels = -1;
  GLint bevelPixels = -1;
  GLint rimWidthPixels = -1;
  GLint lensBandPixels = -1;
  GLint highlight = -1;
  GLint shadow = -1;
  GLint specular = -1;
  GLint chroma = -1;
  GLint edgeDepth = -1;
  GLint lens = -1;
  GLint gloss = -1;
  GLint tint = -1;
  GLint veilSaturation = -1;
  GLint lightDirection = -1;
  GLint opacity = -1;
};

class HyprlandGlassShader {
public:
  [[nodiscard]] Result<void> ensure();
  void reset() noexcept;

  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] const SP<CShader> &shader() const noexcept;
  [[nodiscard]] const GlassShaderUniforms &uniforms() const noexcept;

private:
  [[nodiscard]] Result<void> cacheUniforms();

  SP<CShader> m_shader;
  GlassShaderUniforms m_uniforms;
};

} // namespace hfg::v2
