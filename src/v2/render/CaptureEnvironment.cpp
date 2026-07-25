#include "v2/render/CaptureEnvironment.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

Result<CaptureLimits> failure(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<CaptureLimits>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

} // namespace

Result<CaptureLimits> resolveCaptureLimits(
    std::uint32_t maximumTextureDimension,
    std::uint32_t maximumBytesPerPixel,
    const CaptureBudget& budget) {
    if (maximumTextureDimension == 0U)
        return failure(
            ErrorCode::UnsupportedOperation,
            "adapter.maximum_texture_dimension",
            "renderer did not report a usable texture dimension");
    if (maximumBytesPerPixel == 0U ||
        maximumBytesPerPixel > 64U)
        return failure(
            ErrorCode::UnsupportedOperation,
            "adapter.maximum_bytes_per_pixel",
            "renderer did not report a supported format size");
    if (budget.maxApronPixels >
        Limits::MAX_OUTPUT_BUFFER_DIMENSION)
        return failure(
            ErrorCode::ResourceLimited,
            "budget.max_apron_pixels",
            "capture apron budget exceeds the supported limit");
    if (budget.maxPixels == 0U)
        return failure(
            ErrorCode::ResourceLimited,
            "budget.max_pixels",
            "capture pixel budget must not be zero");
    if (budget.maxBytes == 0U)
        return failure(
            ErrorCode::ResourceLimited,
            "budget.max_bytes",
            "per-capture byte budget must not be zero");
    if (budget.maxTotalBytes == 0U)
        return failure(
            ErrorCode::ResourceLimited,
            "budget.max_total_bytes",
            "total capture byte budget must not be zero");

    const auto dimension = std::min(
        maximumTextureDimension,
        Limits::MAX_OUTPUT_BUFFER_DIMENSION);
    const auto dimensionPixels =
        static_cast<std::uint64_t>(dimension) *
        dimension;
    const auto maximumPixels = std::min(
        budget.maxPixels,
        dimensionPixels);
    const auto maximumRepresentableBytes =
        maximumPixels >
                std::numeric_limits<std::uint64_t>::max() /
                    maximumBytesPerPixel
            ? std::numeric_limits<std::uint64_t>::max()
            : maximumPixels * maximumBytesPerPixel;

    return Result<CaptureLimits>::success({
        .maxWidth = dimension,
        .maxHeight = dimension,
        .maxApronPixels = std::min(
            budget.maxApronPixels,
            dimension),
        .maxBytesPerPixel = maximumBytesPerPixel,
        .maxPixels = maximumPixels,
        .maxBytes = std::min(
            std::min(
                budget.maxBytes,
                budget.maxTotalBytes),
            maximumRepresentableBytes),
    });
}

} // namespace hfg::v2
