#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Material.hpp"
#include "v2/model/Target.hpp"
#include "v2/render/CaptureCache.hpp"
#include "v2/render/CaptureScene.hpp"

#include <array>
#include <cstdint>

namespace hfg::v2 {

struct TextureCoordinate {
    double u = 0.0;
    double v = 0.0;

    friend bool operator==(
        const TextureCoordinate&,
        const TextureCoordinate&) = default;
};

struct DrawSize {
    double width = 0.0;
    double height = 0.0;

    friend bool operator==(const DrawSize&, const DrawSize&) = default;
};

struct MaterialUniforms {
    double   blurPixels = 0.0;
    double   refractionPixels = 0.0;
    double   rimBandPixels = 0.0;
    double   bevelPixels = 0.0;
    double   rimWidthPixels = 0.0;
    double   lensBandPixels = 0.0;
    double   highlight = 0.0;
    double   shadow = 0.0;
    double   specular = 0.0;
    double   chroma = 0.0;
    double   edgeDepth = 0.0;
    double   lens = 0.0;
    double   gloss = 0.0;
    double   tintStrength = 0.0;
    double   veilSaturation = 0.0;
    Point    lightDirection;
    RgbColor tintColor;

    friend bool operator==(
        const MaterialUniforms&,
        const MaterialUniforms&) = default;
};

struct GlassDrawPlan {
    PresentationKey                 key;
    std::uint64_t                   resourceToken = 0;
    CapturePlan                     capture;
    Rect                            destination;
    Rect                            destinationPixels;
    PixelRect                       damageCoverage;
    PixelRect                       captureDamageCoverage;
    Rect                            continuationDamage = {};
    std::array<TextureCoordinate, 4> sourceCorners;
    DrawSize                        fullSizePixels;
    Point                           clipOffsetPixels;
    DrawSize                        clippedSizePixels;
    Shape                           shapePixels;
    double                          roundingPower = 2.0;
    MaterialUniforms                material;
    double                          opacity = 1.0;
    bool                            transitionActive = false;

    friend bool operator==(
        const GlassDrawPlan&,
        const GlassDrawPlan&) = default;
};

[[nodiscard]] Result<GlassDrawPlan>
buildGlassDrawPlan(
    const CaptureAssignment& assignment,
    const CaptureResource& resource);

} // namespace hfg::v2
