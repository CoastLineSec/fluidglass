#include "v2/render/GlassUniformPayload.hpp"

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace hfg::v2 {
namespace {

Result<GlassUniformPayload> invalid(std::string path, std::string message) {
  return Result<GlassUniformPayload>::failure({
      .code = ErrorCode::InvalidRequest,
      .path = std::move(path),
      .message = std::move(message),
  });
}

std::optional<float> finiteFloat(double value) {
  if (!std::isfinite(value) ||
      std::abs(value) > static_cast<double>(std::numeric_limits<float>::max()))
    return std::nullopt;
  return static_cast<float>(value);
}

bool validPositive(double value) {
  return finiteFloat(value).has_value() && value > 0.0;
}

bool validNonNegative(double value) {
  return finiteFloat(value).has_value() && value >= 0.0;
}

bool validUnit(double value) {
  return finiteFloat(value).has_value() && value >= 0.0 && value <= 1.0;
}

std::optional<UniformVec4> vector4(double x, double y, double z, double w) {
  const auto convertedX = finiteFloat(x);
  const auto convertedY = finiteFloat(y);
  const auto convertedZ = finiteFloat(z);
  const auto convertedW = finiteFloat(w);
  if (!convertedX || !convertedY || !convertedZ || !convertedW)
    return std::nullopt;
  return UniformVec4{
      *convertedX,
      *convertedY,
      *convertedZ,
      *convertedW,
  };
}

std::optional<UniformVec4> rectangle(const Rect &value) {
  if (!validPositive(value.width) || !validPositive(value.height))
    return std::nullopt;
  return vector4(value.x, value.y, value.width, value.height);
}

std::optional<UniformVec4> radii(const CornerRadii &value) {
  if (!validNonNegative(value.topLeft) || !validNonNegative(value.topRight) ||
      !validNonNegative(value.bottomRight) ||
      !validNonNegative(value.bottomLeft))
    return std::nullopt;
  return vector4(value.topLeft, value.topRight, value.bottomRight,
                 value.bottomLeft);
}

bool validPlanGeometry(const GlassDrawPlan &plan) {
  constexpr double TOLERANCE = 1e-6;
  return validPositive(plan.destinationPixels.width) &&
         validPositive(plan.destinationPixels.height) &&
         finiteFloat(plan.destinationPixels.x).has_value() &&
         finiteFloat(plan.destinationPixels.y).has_value() &&
         plan.damageCoverage.width > 0 && plan.damageCoverage.height > 0 &&
         validPositive(plan.fullSizePixels.width) &&
         validPositive(plan.fullSizePixels.height) &&
         validNonNegative(plan.clipOffsetPixels.x) &&
         validNonNegative(plan.clipOffsetPixels.y) &&
         validPositive(plan.clippedSizePixels.width) &&
         validPositive(plan.clippedSizePixels.height) &&
         plan.clipOffsetPixels.x + plan.clippedSizePixels.width <=
             plan.fullSizePixels.width + TOLERANCE &&
         plan.clipOffsetPixels.y + plan.clippedSizePixels.height <=
             plan.fullSizePixels.height + TOLERANCE;
}

bool validMaterial(const MaterialUniforms &material) {
  return validNonNegative(material.blurPixels) &&
         validNonNegative(material.refractionPixels) &&
         validNonNegative(material.rimBandPixels) &&
         validNonNegative(material.bevelPixels) &&
         validNonNegative(material.rimWidthPixels) &&
         validNonNegative(material.lensBandPixels) &&
         validUnit(material.highlight) && validUnit(material.shadow) &&
         validUnit(material.specular) && validUnit(material.chroma) &&
         validUnit(material.edgeDepth) && validUnit(material.lens) &&
         validUnit(material.gloss) && validUnit(material.tintStrength) &&
         validUnit(material.veilSaturation) &&
         validUnit(material.tintColor.red) &&
         validUnit(material.tintColor.green) &&
         validUnit(material.tintColor.blue) &&
         finiteFloat(material.lightDirection.x).has_value() &&
         finiteFloat(material.lightDirection.y).has_value();
}

} // namespace

Result<GlassUniformPayload>
buildGlassUniformPayload(const GlassDrawPlan &plan) {
  if (plan.resourceToken == 0U)
    return invalid("plan.resource_token",
                   "draw plan resource token must not be zero");
  if (auto capture = validateCapturePlan(plan.capture); !capture)
    return Result<GlassUniformPayload>::failure(capture.error());
  if (plan.key.output != plan.capture.key.output ||
      plan.key.outputGeneration != plan.capture.key.outputGeneration ||
      plan.key.stage != plan.capture.key.stage)
    return invalid("plan.capture.key", "draw and capture identities differ");
  if (!validPlanGeometry(plan))
    return invalid("plan.geometry",
                   "draw geometry is not finite, positive and bounded");
  if (!validUnit(plan.opacity))
    return invalid("plan.opacity",
                   "draw opacity must be finite from 0 through 1");
  if (!validPositive(plan.roundingPower) || plan.roundingPower > 16.0)
    return invalid(
        "plan.rounding_power",
        "rounding power must be finite from greater than 0 through 16");
  if (!validMaterial(plan.material))
    return invalid("plan.material",
                   "material uniforms are not finite bounded values");

  GlassUniformPayload result;
  for (std::size_t index = 0; index < plan.sourceCorners.size(); ++index) {
    const auto &source = plan.sourceCorners[index];
    if (!validUnit(source.u) || !validUnit(source.v))
      return invalid("plan.source_corners[" + std::to_string(index) + "]",
                     "capture coordinate must be finite from 0 through 1");
    result.sourceCorners[index] = {
        static_cast<float>(source.u),
        static_cast<float>(source.v),
    };
  }
  result.fullSize = {
      static_cast<float>(plan.fullSizePixels.width),
      static_cast<float>(plan.fullSizePixels.height),
  };
  result.clipOffset = {
      static_cast<float>(plan.clipOffsetPixels.x),
      static_cast<float>(plan.clipOffsetPixels.y),
  };
  result.clipSize = {
      static_cast<float>(plan.clippedSizePixels.width),
      static_cast<float>(plan.clippedSizePixels.height),
  };
  result.roundingPower = static_cast<float>(plan.roundingPower);

  const auto shape = std::visit(
      [&](const auto &value) -> std::optional<Error> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, RoundedRectShape>) {
          if (!validNonNegative(value.radius))
            return Error{
                ErrorCode::InvalidRequest,
                "plan.shape.radius",
                "rounded radius must be finite and non-negative",
            };
          result.shapeKind = 0;
          result.radius = static_cast<float>(value.radius);
        } else if constexpr (std::is_same_v<T, RingShape>) {
          if (!validNonNegative(value.outerRadius) ||
              !validPositive(value.thickness))
            return Error{
                ErrorCode::InvalidRequest,
                "plan.shape.ring",
                "ring dimensions must be finite and positive",
            };
          result.shapeKind = 1;
          result.ringRadius = static_cast<float>(value.outerRadius);
          result.ringThickness = static_cast<float>(value.thickness);
        } else {
          if ((!value.base && value.parts.empty()) ||
              (value.cutout && !value.base) ||
              value.parts.size() > Limits::MAX_COMPOUND_PARTS ||
              value.connectors.size() > Limits::MAX_COMPOUND_CONNECTORS)
            return Error{
                ErrorCode::InvalidRequest,
                "plan.shape.compound",
                "compound topology is invalid or exceeds its limit",
            };
          result.shapeKind = 2;
          if (value.base) {
            const auto converted = radii(value.base->corners);
            if (!converted)
              return Error{
                  ErrorCode::InvalidRequest,
                  "plan.shape.base",
                  "base radii are invalid",
              };
            result.baseEnabled = 1;
            result.baseRadii = *converted;
          }
          if (value.cutout) {
            const auto convertedRect = rectangle(value.cutout->rect);
            const auto convertedRadii = radii(value.cutout->corners);
            if (!convertedRect || !convertedRadii)
              return Error{
                  ErrorCode::InvalidRequest,
                  "plan.shape.cutout",
                  "cutout geometry is invalid",
              };
            result.cutoutEnabled = 1;
            result.cutoutRect = *convertedRect;
            result.cutoutRadii = *convertedRadii;
          }
          result.partCount = static_cast<int>(value.parts.size());
          for (std::size_t index = 0; index < value.parts.size(); ++index) {
            const auto &part = value.parts[index];
            const auto convertedRect = rectangle(part.rect);
            const auto convertedRadii = radii(part.corners);
            const auto convertedJunctions = radii(part.junctions);
            const auto convertedExtent =
                rectangle(part.materialExtent.value_or(part.rect));
            if (!convertedRect || !convertedRadii || !convertedJunctions ||
                !convertedExtent || !validUnit(part.opacity))
              return Error{
                  ErrorCode::InvalidRequest,
                  "plan.shape.parts[" + std::to_string(index) + "]",
                  "compound part fields are invalid",
              };
            result.partRects[index] = *convertedRect;
            result.partRadii[index] = *convertedRadii;
            result.partJunctions[index] = *convertedJunctions;
            result.partMaterialExtents[index] = *convertedExtent;
            result.partOpacity[index] = static_cast<float>(part.opacity);
          }
          result.connectorCount = static_cast<int>(value.connectors.size());
          for (std::size_t index = 0; index < value.connectors.size();
               ++index) {
            const auto converted = rectangle(value.connectors[index]);
            if (!converted)
              return Error{
                  ErrorCode::InvalidRequest,
                  "plan.shape.connectors[" + std::to_string(index) + "]",
                  "connector geometry is invalid",
              };
            result.connectorRects[index] = *converted;
          }
          if (!validNonNegative(value.connectorCurve))
            return Error{
                ErrorCode::InvalidRequest,
                "plan.shape.connector_curve",
                "connector curve is invalid",
            };
          result.connectorCurve = static_cast<float>(value.connectorCurve);
        }
        return std::nullopt;
      },
      plan.shapePixels);
  if (shape)
    return Result<GlassUniformPayload>::failure(*shape);

  result.blurPixels = static_cast<float>(plan.material.blurPixels);
  result.refractionPixels = static_cast<float>(plan.material.refractionPixels);
  result.edgeBandPixels = static_cast<float>(plan.material.rimBandPixels);
  result.bevelPixels = static_cast<float>(plan.material.bevelPixels);
  result.rimWidthPixels = static_cast<float>(plan.material.rimWidthPixels);
  result.lensBandPixels = static_cast<float>(plan.material.lensBandPixels);
  result.highlight = static_cast<float>(plan.material.highlight);
  result.shadow = static_cast<float>(plan.material.shadow);
  result.specular = static_cast<float>(plan.material.specular);
  result.chroma = static_cast<float>(plan.material.chroma);
  result.edgeDepth = static_cast<float>(plan.material.edgeDepth);
  result.lens = static_cast<float>(plan.material.lens);
  result.gloss = static_cast<float>(plan.material.gloss);
  result.tint = {
      static_cast<float>(plan.material.tintColor.red),
      static_cast<float>(plan.material.tintColor.green),
      static_cast<float>(plan.material.tintColor.blue),
      static_cast<float>(plan.material.tintStrength),
  };
  result.veilSaturation = static_cast<float>(plan.material.veilSaturation);
  result.lightDirection = {
      static_cast<float>(plan.material.lightDirection.x),
      static_cast<float>(plan.material.lightDirection.y),
  };
  result.opacity = static_cast<float>(plan.opacity);
  return Result<GlassUniformPayload>::success(std::move(result));
}

} // namespace hfg::v2
