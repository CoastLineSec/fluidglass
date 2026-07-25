#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/CapturePlan.hpp"

#include <cstdint>

namespace hfg::v2 {

struct CaptureBudget {
    std::uint32_t maxApronPixels = 0;
    std::uint64_t maxPixels = 0;
    std::uint64_t maxBytes = 0;
    std::uint64_t maxTotalBytes = 0;

    friend bool operator==(
        const CaptureBudget&,
        const CaptureBudget&) = default;
};

[[nodiscard]] Result<CaptureLimits> resolveCaptureLimits(
    std::uint32_t maximumTextureDimension,
    std::uint32_t maximumBytesPerPixel,
    const CaptureBudget& budget);

} // namespace hfg::v2
