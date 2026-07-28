#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/CapturePlan.hpp"
#include "v2/render/OutputGeneration.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace hfg::v2 {

struct BlitRect {
    std::int32_t x0 = 0;
    std::int32_t y0 = 0;
    std::int32_t x1 = 0;
    std::int32_t y1 = 0;

    friend bool operator==(const BlitRect&, const BlitRect&) = default;
};

struct CaptureBlit {
    BlitRect source;
    BlitRect destination;

    friend bool operator==(const CaptureBlit&, const CaptureBlit&) = default;
};

[[nodiscard]] Result<CaptureBlit> captureBlitFor(
    const CapturePlan& plan,
    const OutputGeneration& output);

[[nodiscard]] Result<std::vector<CaptureBlit>> captureUpdateBlits(
    const CapturePlan& plan,
    const OutputGeneration& output,
    std::span<const PixelRect> outputDamage,
    bool initialized);

} // namespace hfg::v2
