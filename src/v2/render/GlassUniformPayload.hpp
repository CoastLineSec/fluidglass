#pragma once

#include "v2/core/Limits.hpp"
#include "v2/core/Result.hpp"
#include "v2/render/GlassDrawPlan.hpp"

#include <array>

namespace hfg::v2 {

struct UniformVec2 {
  float x = 0.0F;
  float y = 0.0F;

  friend bool operator==(const UniformVec2 &, const UniformVec2 &) = default;
};

struct UniformVec4 {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  float w = 0.0F;

  friend bool operator==(const UniformVec4 &, const UniformVec4 &) = default;
};

struct GlassUniformPayload {
  std::array<UniformVec2, 4> sourceCorners;
  UniformVec2 fullSize;
  UniformVec2 clipOffset;
  UniformVec2 clipSize;

  int shapeKind = 0;
  float radius = 0.0F;
  float roundingPower = 2.0F;
  float ringRadius = 0.0F;
  float ringThickness = 0.0F;
  int baseEnabled = 0;
  UniformVec4 baseRadii;
  int cutoutEnabled = 0;
  UniformVec4 cutoutRect;
  UniformVec4 cutoutRadii;
  int partCount = 0;
  std::array<UniformVec4, Limits::MAX_COMPOUND_PARTS> partRects;
  std::array<UniformVec4, Limits::MAX_COMPOUND_PARTS> partRadii;
  std::array<UniformVec4, Limits::MAX_COMPOUND_PARTS> partJunctions;
  std::array<UniformVec4, Limits::MAX_COMPOUND_PARTS> partMaterialExtents;
  std::array<float, Limits::MAX_COMPOUND_PARTS> partOpacity;
  int connectorCount = 0;
  std::array<UniformVec4, Limits::MAX_COMPOUND_CONNECTORS> connectorRects;
  float connectorCurve = 0.0F;

  float blurPixels = 0.0F;
  float refractionPixels = 0.0F;
  float edgeBandPixels = 0.0F;
  float bevelPixels = 0.0F;
  float rimWidthPixels = 0.0F;
  float lensBandPixels = 0.0F;
  float highlight = 0.0F;
  float shadow = 0.0F;
  float specular = 0.0F;
  float chroma = 0.0F;
  float edgeDepth = 0.0F;
  float lens = 0.0F;
  float gloss = 0.0F;
  UniformVec4 tint;
  float veilSaturation = 0.0F;
  UniformVec2 lightDirection;
  float opacity = 0.0F;

  friend bool operator==(const GlassUniformPayload &,
                         const GlassUniformPayload &) = default;
};

[[nodiscard]] Result<GlassUniformPayload>
buildGlassUniformPayload(const GlassDrawPlan &plan);

} // namespace hfg::v2
