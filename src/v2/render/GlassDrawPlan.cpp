#include "v2/render/GlassDrawPlan.hpp"

#include "v2/render/MaterialSampling.hpp"
#include "v2/render/ShapeMotion.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace hfg::v2 {
namespace {

constexpr double BLUR_MIN = 2.0;
constexpr double BLUR_MEDIUM = 18.0;
constexpr double BLUR_MAX = 88.0;
constexpr double DARK_VEIL_MIN = 0.43;
constexpr double DARK_VEIL_MEDIUM = 0.43;
constexpr double DARK_VEIL_MAX = 0.72;
constexpr double DARK_SATURATION_MIN = 0.95;
constexpr double DARK_SATURATION_MEDIUM = 0.95;
constexpr double DARK_SATURATION_MAX = 0.95;
constexpr double LIGHT_VEIL_MIN = 0.54;
constexpr double LIGHT_VEIL_MEDIUM = 0.53;
constexpr double LIGHT_VEIL_MAX = 0.71;
constexpr double LIGHT_SATURATION_MIN = 0.61;
constexpr double LIGHT_SATURATION_MEDIUM = 0.49;
constexpr double LIGHT_SATURATION_MAX = 0.37;
constexpr double MAX_ROUNDING_POWER = 16.0;
constexpr double UV_TOLERANCE = 1e-9;
constexpr double PI = 3.14159265358979323846;

Result<GlassDrawPlan> failure(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<GlassDrawPlan>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

double threeStop(
    double value,
    double low,
    double middle,
    double high) {
    if (value < 0.5)
        return low + (middle - low) * (value / 0.5);
    return middle +
        (high - middle) * ((value - 0.5) / 0.5);
}

Rect scaleRect(const Rect& rect, double scale) {
    return {
        .x = rect.x * scale,
        .y = rect.y * scale,
        .width = rect.width * scale,
        .height = rect.height * scale,
    };
}

CornerRadii scaleCorners(
    const CornerRadii& corners,
    double scale) {
    return {
        .topLeft = corners.topLeft * scale,
        .topRight = corners.topRight * scale,
        .bottomRight = corners.bottomRight * scale,
        .bottomLeft = corners.bottomLeft * scale,
    };
}

Transition scaleTransition(
    const Transition& transition,
    double scale) {
    auto scaled = transition;
    scaled.travel *= scale;
    return scaled;
}

Shape scaleShape(const Shape& shape, double scale) {
    return std::visit(
        [scale](const auto& value) -> Shape {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, RoundedRectShape>) {
                return RoundedRectShape{
                    .radius = value.radius * scale,
                };
            } else if constexpr (std::is_same_v<T, RingShape>) {
                return RingShape{
                    .outerRadius = value.outerRadius * scale,
                    .thickness = value.thickness * scale,
                };
            } else {
                CompoundShape scaled;
                if (value.base)
                    scaled.base = CompoundBase{
                        .corners = scaleCorners(
                            value.base->corners,
                            scale),
                    };
                if (value.cutout)
                    scaled.cutout = CompoundCutout{
                        .rect = scaleRect(
                            value.cutout->rect,
                            scale),
                        .corners = scaleCorners(
                            value.cutout->corners,
                            scale),
                    };
                scaled.parts.reserve(value.parts.size());
                for (const auto& part : value.parts) {
                    std::optional<Rect> materialExtent;
                    if (part.materialExtent)
                        materialExtent =
                            scaleRect(*part.materialExtent, scale);
                    std::optional<PartTransition> transition;
                    if (part.transition)
                        transition = PartTransition{
                            .motion = scaleTransition(
                                part.transition->motion,
                                scale),
                            .protrusion =
                                part.transition->protrusion * scale,
                        };
                    scaled.parts.push_back({
                        .rect = scaleRect(part.rect, scale),
                        .corners = scaleCorners(
                            part.corners,
                            scale),
                        .junctions = scaleCorners(
                            part.junctions,
                            scale),
                        .materialExtent =
                            std::move(materialExtent),
                        .transition = std::move(transition),
                        .opacity = part.opacity,
                    });
                }
                scaled.connectors.reserve(value.connectors.size());
                for (const auto& connector : value.connectors)
                    scaled.connectors.push_back(
                        scaleRect(connector, scale));
                scaled.connectorCurve =
                    value.connectorCurve * scale;
                return scaled;
            }
        },
        shape);
}

MaterialInput materialInput(const Material& material) {
    const auto byte = [](double channel) {
        return static_cast<unsigned>(
            std::lround(std::clamp(channel, 0.0, 1.0) * 255.0));
    };
    constexpr char HEX[] = "0123456789ABCDEF";
    std::string color(7, '#');
    const std::array channels{
        byte(material.tintColor.red),
        byte(material.tintColor.green),
        byte(material.tintColor.blue),
    };
    for (std::size_t index = 0; index < channels.size(); ++index) {
        color[1U + index * 2U] =
            HEX[(channels[index] >> 4U) & 0xFU];
        color[2U + index * 2U] =
            HEX[channels[index] & 0xFU];
    }
    return {
        .glassLevel = material.glassLevel,
        .blurLevel = material.blurLevel,
        .tintLevel = material.tintLevel,
        .tintEnabled = material.tintEnabled,
        .tintColor = std::move(color),
        .lightMode = material.lightMode,
        .refraction = material.refraction,
        .rimBand = material.rimBand,
        .bevel = material.bevel,
        .rimWidth = material.rimWidth,
        .highlight = material.highlight,
        .shadow = material.shadow,
        .lightAngle = material.lightAngle,
        .specular = material.specular,
        .chroma = material.chroma,
        .edgeDepth = material.edgeDepth,
        .lens = material.lens,
        .lensBand = material.lensBand,
        .gloss = material.gloss,
    };
}

TargetInput targetInput(const Target& target) {
    return {
        .id = target.id,
        .kind = target.kind,
        .material = target.material,
        .shape = target.shape,
        .selector = target.selector,
        .geometry = target.geometry,
        .stage = target.stage,
        .transition = target.transition,
        .enabled = target.enabled,
    };
}

bool validOpacity(double value) {
    return std::isfinite(value) &&
        value >= 0.0 &&
        value <= 1.0;
}

bool validRoundingPower(double value) {
    return std::isfinite(value) &&
        value > 0.0 &&
        value <= MAX_ROUNDING_POWER;
}

bool geometryMatches(
    const MappedGeometry& left,
    const MappedGeometry& right) {
    return left == right;
}

bool planMatchesPresentation(
    const CapturePlan& plan,
    const PlannedPresentation& presentation) {
    const auto& key = presentation.presentation.key;
    const auto stageObjectToken =
        key.stage == RenderStage::PreWindow
        ? presentation.target.attachment.objectToken
        : 0U;
    return plan.key.output == key.output &&
        plan.key.outputGeneration == key.outputGeneration &&
        plan.key.stage == key.stage &&
        plan.key.renderFormat ==
            presentation.output.snapshot.renderFormat &&
        plan.key.colorStateToken ==
            presentation.output.snapshot.colorStateToken &&
        plan.key.stageObjectToken == stageObjectToken;
}

Result<void> validateAssignment(
    const CaptureAssignment& assignment) {
    const auto& planned = assignment.presentation;
    const auto& target = planned.target;
    const auto& attachment = target.attachment;
    const auto& presentation = planned.presentation;
    const auto& output = planned.output;
    if (target.definition.id != attachment.identity.targetId ||
        target.definition.kind != attachment.kind ||
        presentation.key.identity != attachment.identity)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "assignment.presentation.identity",
            "target, attachment and presentation identities differ",
        });
    if (presentation.key.output != output.snapshot.name ||
        presentation.key.outputGeneration != output.generation)
        return Result<void>::failure({
            ErrorCode::StaleGeneration,
            "assignment.presentation.output",
            "presentation does not reference its supplied output generation",
        });
    if (presentation.key.stage != attachment.stage ||
        presentation.attachmentToken != attachment.objectToken)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "assignment.presentation.attachment",
            "presentation does not reference its supplied attachment",
        });
    if (!target.definition.enabled)
        return Result<void>::failure({
            ErrorCode::InvalidTarget,
            "assignment.presentation.target.enabled",
            "disabled targets must not reach draw planning",
        });
    if (!validOpacity(attachment.opacity) ||
        presentation.opacity != attachment.opacity)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "assignment.presentation.opacity",
            "presentation opacity differs from its attachment",
        });
    if (!validRoundingPower(target.roundingPower))
        return Result<void>::failure({
            ErrorCode::InvalidTarget,
            "assignment.presentation.rounding_power",
            "expected a finite rounding power from greater than 0 through 16",
        });

    auto validTarget = validateTarget(targetInput(target.definition));
    if (!validTarget)
        return Result<void>::failure(validTarget.error());
    auto validMaterial = validateMaterial(
        planned.material.name,
        materialInput(planned.material));
    if (!validMaterial)
        return Result<void>::failure(validMaterial.error());
    if (!(validMaterial.value() == planned.material))
        return Result<void>::failure({
            ErrorCode::InvalidMaterial,
            "assignment.presentation.material",
            "material channels are not canonical validated values",
        });

    auto mapped = mapGlobalLogicalRect(
        attachment.globalGeometry,
        output);
    if (!mapped)
        return Result<void>::failure(mapped.error());
    if (!mapped.value() ||
        !geometryMatches(*mapped.value(), presentation.geometry))
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "assignment.presentation.geometry",
            "presentation geometry is not the canonical mapping for its target and output",
        });

    auto sampling = resolveMaterialSampling(
        planned.material,
        attachment.globalGeometry.width,
        attachment.globalGeometry.height,
        output.snapshot.scale);
    if (!sampling)
        return Result<void>::failure(sampling.error());
    if (!(sampling.value() == planned.sampling))
        return Result<void>::failure({
            ErrorCode::InvalidMaterial,
            "assignment.presentation.sampling",
            "sampling footprint is not canonical for its material and geometry",
        });
    if (auto validCapture = validateCapturePlan(assignment.required);
        !validCapture)
        return validCapture;
    if (!planMatchesPresentation(
            assignment.required,
            planned))
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "assignment.required.key",
            "capture requirement differs from its presentation",
        });

    const auto& snapshot = output.snapshot;
    const auto maxPixels =
        static_cast<std::uint64_t>(snapshot.bufferWidth) *
        snapshot.bufferHeight;
    if (assignment.required.bytesPerPixel >
        std::numeric_limits<std::uint64_t>::max() /
            maxPixels)
        return Result<void>::failure({
            ErrorCode::ResourceLimited,
            "assignment.required.byte_count",
            "capture byte count overflows",
        });
    const CaptureLimits limits{
        .maxWidth = snapshot.bufferWidth,
        .maxHeight = snapshot.bufferHeight,
        .maxApronPixels = planned.sampling.apronPixels,
        .maxBytesPerPixel =
            assignment.required.bytesPerPixel,
        .maxPixels = maxPixels,
        .maxBytes =
            maxPixels * assignment.required.bytesPerPixel,
    };
    const CaptureRequest request{
        .output = output,
        .stage = presentation.key.stage,
        .coverage = planned.transitionEnvelope
            ? planned.transitionEnvelope->coverage
            : presentation.geometry.coverage,
        .apronPixels = planned.sampling.apronPixels,
        .bytesPerPixel =
            assignment.required.bytesPerPixel,
        .stageObjectToken =
            presentation.key.stage ==
                    RenderStage::PreWindow
                ? attachment.objectToken
                : 0U,
    };
    const std::array requests{request};
    auto canonical = planCaptures(requests, limits);
    if (!canonical)
        return Result<void>::failure(canonical.error());
    if (canonical.value().size() != 1U ||
        !(canonical.value().front() == assignment.required))
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "assignment.required",
            "capture requirement is not canonical for its presentation",
        });
    return Result<void>::success();
}

MaterialUniforms resolveMaterialUniforms(
    const Material& material,
    double logicalWidth,
    double logicalHeight,
    double scale) {
    const auto glassLevel = material.glassLevel;
    const auto blurLevel =
        material.blurLevel.value_or(glassLevel);
    const auto tintLevel =
        material.tintLevel.value_or(glassLevel);
    const auto halfShort =
        std::min(logicalWidth, logicalHeight) *
        scale * 0.5;
    const auto band = [scale, halfShort](
                          double value,
                          double fraction) {
        return std::min(
            value * scale,
            halfShort * fraction);
    };
    const auto radians = material.lightAngle * PI / 180.0;
    return {
        .blurPixels = scale * threeStop(
            blurLevel,
            BLUR_MIN,
            BLUR_MEDIUM,
            BLUR_MAX),
        .refractionPixels = std::min(
            material.refraction * scale,
            halfShort * 0.8),
        .rimBandPixels = band(material.rimBand, 0.6),
        .bevelPixels = band(material.bevel, 0.6),
        .rimWidthPixels = std::max(
            1.0,
            material.rimWidth * scale),
        .lensBandPixels = band(material.lensBand, 0.55),
        .highlight = material.highlight,
        .shadow = material.shadow,
        .specular = material.specular,
        .chroma = material.chroma,
        .edgeDepth = material.edgeDepth,
        .lens = material.lens,
        .gloss =
            material.gloss * (1.0 - 0.90 * glassLevel),
        .tintStrength = material.lightMode
            ? threeStop(
                  tintLevel,
                  LIGHT_VEIL_MIN,
                  LIGHT_VEIL_MEDIUM,
                  LIGHT_VEIL_MAX)
            : threeStop(
                  tintLevel,
                  DARK_VEIL_MIN,
                  DARK_VEIL_MEDIUM,
                  DARK_VEIL_MAX),
        .veilSaturation = material.lightMode
            ? threeStop(
                  glassLevel,
                  LIGHT_SATURATION_MIN,
                  LIGHT_SATURATION_MEDIUM,
                  LIGHT_SATURATION_MAX)
            : threeStop(
                  glassLevel,
                  DARK_SATURATION_MIN,
                  DARK_SATURATION_MEDIUM,
                  DARK_SATURATION_MAX),
        .lightDirection = {
            .x = std::cos(radians),
            .y = std::sin(radians),
        },
        .tintColor = material.tintEnabled
            ? material.tintColor
            : RgbColor{},
    };
}

Result<std::array<TextureCoordinate, 4>> sourceCorners(
    const MappedGeometry& geometry,
    const CapturePlan& capture) {
    const auto width =
        static_cast<double>(capture.region.width);
    const auto height =
        static_cast<double>(capture.region.height);
    std::array<TextureCoordinate, 4> result;
    for (std::size_t index = 0;
         index < result.size();
         ++index) {
        const auto& point =
            geometry.semanticCorners[index];
        // The capture keeps the framebuffer's top-down row order, so v runs
        // with y rather than against it.
        const auto u =
            (point.x - capture.region.x) / width;
        const auto v =
            (point.y - capture.region.y) / height;
        if (!std::isfinite(u) || !std::isfinite(v) ||
            u < -UV_TOLERANCE ||
            u > 1.0 + UV_TOLERANCE ||
            v < -UV_TOLERANCE ||
            v > 1.0 + UV_TOLERANCE)
            return Result<std::array<TextureCoordinate, 4>>::failure({
                ErrorCode::InvalidRequest,
                "assignment.presentation.geometry.semantic_corners",
                "mapped source corner lies outside its capture",
            });
        result[index] = {
            .u = std::clamp(u, 0.0, 1.0),
            .v = std::clamp(v, 0.0, 1.0),
        };
    }
    return Result<std::array<TextureCoordinate, 4>>::success(
        std::move(result));
}

} // namespace

Result<GlassDrawPlan>
buildGlassDrawPlan(
    const CaptureAssignment& assignment,
    const CaptureResource& resource) {
    if (auto valid = validateAssignment(assignment); !valid)
        return Result<GlassDrawPlan>::failure(valid.error());
    if (resource.token == 0U)
        return failure(
            ErrorCode::InvalidRequest,
            "resource.token",
            "capture resource token must not be zero");
    if (auto valid = validateCapturePlan(resource.plan); !valid)
        return Result<GlassDrawPlan>::failure(valid.error());
    const auto& output =
        assignment.presentation.output.snapshot;
    const auto resourceRight =
        static_cast<std::int64_t>(resource.plan.region.x) +
        resource.plan.region.width;
    const auto resourceBottom =
        static_cast<std::int64_t>(resource.plan.region.y) +
        resource.plan.region.height;
    if (resourceRight > output.bufferWidth ||
        resourceBottom > output.bufferHeight)
        return failure(
            ErrorCode::InvalidRequest,
            "resource.plan.region",
            "capture resource exceeds its output buffer");
    if (!capturePlanCovers(
            resource.plan,
            assignment.required))
        return failure(
            ErrorCode::StaleGeneration,
            "resource.plan",
            "capture resource does not cover the presentation requirement");

    auto corners = sourceCorners(
        assignment.presentation.presentation.geometry,
        resource.plan);
    if (!corners)
        return Result<GlassDrawPlan>::failure(corners.error());
    const auto& damageGeometry =
        assignment.presentation.transitionEnvelope
        ? *assignment.presentation.transitionEnvelope
        : assignment.presentation.presentation.geometry;
    auto damageCoverage = mapBufferPixelRectToOutput(
        damageGeometry.coverage,
        output);
    if (!damageCoverage)
        return Result<GlassDrawPlan>::failure({
            .code = damageCoverage.error().code,
            .path = "assignment.presentation.geometry.coverage." +
                damageCoverage.error().path,
            .message = damageCoverage.error().message,
        });
    auto captureDamageCoverage = mapBufferPixelRectToOutput(
        resource.plan.region,
        output);
    if (!captureDamageCoverage)
        return Result<GlassDrawPlan>::failure({
            .code = captureDamageCoverage.error().code,
            .path = "resource.plan.region." +
                captureDamageCoverage.error().path,
            .message = captureDamageCoverage.error().message,
        });

    const auto& planned = assignment.presentation;
    auto movingShape = resolveShapeMotion(
        planned.target.definition.shape,
        planned.target.transitionAnchorMs,
        planned.motionTimeMs);
    if (!movingShape)
        return Result<GlassDrawPlan>::failure(
            movingShape.error());
    const auto& geometry = planned.presentation.geometry;
    const auto& full = planned.target.attachment.globalGeometry;
    const auto scale = planned.output.snapshot.scale;
    return Result<GlassDrawPlan>::success({
        .key = planned.presentation.key,
        .resourceToken = resource.token,
        .capture = resource.plan,
        .destination = geometry.outputLocal,
        .destinationPixels = {
            .x = geometry.outputLocal.x * scale,
            .y = geometry.outputLocal.y * scale,
            .width = geometry.outputLocal.width * scale,
            .height = geometry.outputLocal.height * scale,
        },
        .damageCoverage = damageCoverage.value(),
        .captureDamageCoverage =
            captureDamageCoverage.value(),
        .continuationDamage =
            planned.target.transitionEnvelopeGlobal.value_or(full),
        .sourceCorners = std::move(corners.value()),
        .fullSizePixels = {
            .width = full.width * scale,
            .height = full.height * scale,
        },
        .clipOffsetPixels = {
            .x = (geometry.clippedGlobal.x - full.x) * scale,
            .y = (geometry.clippedGlobal.y - full.y) * scale,
        },
        .clippedSizePixels = {
            .width = geometry.clippedGlobal.width * scale,
            .height = geometry.clippedGlobal.height * scale,
        },
        .shapePixels = scaleShape(
            movingShape.value().shape,
            scale),
        .roundingPower = planned.target.roundingPower,
        .material = resolveMaterialUniforms(
            planned.material,
            full.width,
            full.height,
            scale),
        .opacity = planned.presentation.opacity,
        .transitionActive =
            planned.target.transitionActive ||
            movingShape.value().active,
    });
}

} // namespace hfg::v2
