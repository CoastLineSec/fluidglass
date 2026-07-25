#include "v2/render/MaterialSampling.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

constexpr double BLUR_MIN = 2.0;
constexpr double BLUR_MEDIUM = 18.0;
constexpr double BLUR_MAX = 88.0;
constexpr double ANTIALIAS_MARGIN_PIXELS = 12.0;
constexpr double MAX_SCALE = 16.0;
constexpr double MAX_LOGICAL_SIZE = 1'000'000.0;

Result<MaterialSamplingFootprint> failure(
    std::string path,
    std::string message) {
    return Result<MaterialSamplingFootprint>::failure({
        .code = ErrorCode::InvalidMaterial,
        .path = std::move(path),
        .message = std::move(message),
    });
}

bool inRange(double value, double minimum, double maximum) {
    return std::isfinite(value) &&
        value >= minimum &&
        value <= maximum;
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

} // namespace

Result<MaterialSamplingFootprint>
resolveMaterialSampling(
    const Material& material,
    double logicalWidth,
    double logicalHeight,
    double outputScale) {
    if (!inRange(logicalWidth, 0.0, MAX_LOGICAL_SIZE) ||
        logicalWidth == 0.0)
        return failure(
            "geometry.width",
            "expected a finite positive logical width");
    if (!inRange(logicalHeight, 0.0, MAX_LOGICAL_SIZE) ||
        logicalHeight == 0.0)
        return failure(
            "geometry.height",
            "expected a finite positive logical height");
    if (!inRange(outputScale, 0.0, MAX_SCALE) ||
        outputScale == 0.0)
        return failure(
            "output.scale",
            "expected a finite positive output scale");
    if (!inRange(material.glassLevel, 0.0, 1.0))
        return failure(
            "material.glass_level",
            "expected a finite value from 0 to 1");
    if (material.blurLevel &&
        !inRange(*material.blurLevel, 0.0, 1.0))
        return failure(
            "material.blur_level",
            "expected a finite value from 0 to 1");
    if (!inRange(material.refraction, 0.0, 200.0))
        return failure(
            "material.refraction",
            "expected a finite value from 0 to 200");
    if (!inRange(material.chroma, 0.0, 1.0))
        return failure(
            "material.chroma",
            "expected a finite value from 0 to 1");
    if (!inRange(material.lens, 0.0, 1.0))
        return failure(
            "material.lens",
            "expected a finite value from 0 to 1");
    if (!inRange(material.lensBand, 0.0, 200.0))
        return failure(
            "material.lens_band",
            "expected a finite value from 0 to 200");

    const auto blurLevel =
        material.blurLevel.value_or(material.glassLevel);
    const auto blurPixels = outputScale * threeStop(
        blurLevel,
        BLUR_MIN,
        BLUR_MEDIUM,
        BLUR_MAX);
    const auto halfShortAxis =
        std::min(logicalWidth, logicalHeight) *
        outputScale * 0.5;
    const auto refractionPixels = std::min(
        material.refraction * outputScale,
        halfShortAxis * 0.8);
    const auto chromaticPixels =
        refractionPixels * material.chroma;
    const auto lensBandPixels = std::min(
        material.lensBand * outputScale,
        halfShortAxis * 0.55);
    const auto lensPixels = lensBandPixels * material.lens;
    const auto apron = blurPixels * 2.0 +
        refractionPixels +
        chromaticPixels +
        lensPixels +
        ANTIALIAS_MARGIN_PIXELS;
    if (!std::isfinite(apron) ||
        apron < 0.0 ||
        apron > static_cast<double>(
                    Limits::MAX_OUTPUT_BUFFER_DIMENSION) ||
        std::ceil(apron) >
            static_cast<double>(
                std::numeric_limits<std::uint32_t>::max()))
        return Result<MaterialSamplingFootprint>::failure({
            .code = ErrorCode::ResourceLimited,
            .path = "material.sampling_apron",
            .message = "material sampling footprint exceeds the supported limit",
        });

    return Result<MaterialSamplingFootprint>::success({
        .blurPixels = blurPixels,
        .refractionPixels = refractionPixels,
        .chromaticPixels = chromaticPixels,
        .lensPixels = lensPixels,
        .apronPixels =
            static_cast<std::uint32_t>(std::ceil(apron)),
    });
}

} // namespace hfg::v2
