#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Target.hpp"
#include "v2/render/Geometry.hpp"
#include "v2/render/OutputGeneration.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hfg::v2 {

struct CaptureLimits {
    std::uint32_t maxWidth = 0;
    std::uint32_t maxHeight = 0;
    std::uint32_t maxApronPixels = 0;
    std::uint32_t maxBytesPerPixel = 0;
    std::uint64_t maxPixels = 0;
    std::uint64_t maxBytes = 0;

    friend bool operator==(const CaptureLimits&, const CaptureLimits&) = default;
};

struct CaptureKey {
    std::string   output;
    std::uint64_t outputGeneration = 0;
    RenderStage   stage = RenderStage::PostWindows;
    std::uint32_t renderFormat = 0;
    std::uint64_t colorStateToken = 0;
    std::uint64_t stageObjectToken = 0;

    friend bool operator==(const CaptureKey&, const CaptureKey&) = default;
};

struct CaptureRequest {
    OutputGeneration output;
    RenderStage       stage = RenderStage::PostWindows;
    PixelRect         coverage;
    std::uint32_t     apronPixels = 0;
    std::uint32_t     bytesPerPixel = 0;
    std::uint64_t     stageObjectToken = 0;
};

struct CapturePlan {
    CaptureKey   key;
    PixelRect    region;
    std::uint32_t bytesPerPixel = 0;
    std::uint64_t pixelCount = 0;
    std::uint64_t byteCount = 0;

    friend bool operator==(const CapturePlan&, const CapturePlan&) = default;
};

[[nodiscard]] Result<void> validateCapturePlan(const CapturePlan& plan);

[[nodiscard]] Result<std::vector<CapturePlan>> planCaptures(
    std::span<const CaptureRequest> requests,
    const CaptureLimits& limits);

[[nodiscard]] bool capturePlanCovers(
    const CapturePlan& available,
    const CapturePlan& required) noexcept;

} // namespace hfg::v2
