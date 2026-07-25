#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/CaptureEnvironment.hpp"
#include "v2/render/CaptureScene.hpp"
#include "v2/render/OutputGeneration.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace hfg::v2 {

struct CaptureFormatIssue {
    std::uint32_t renderFormat = 0;
    Error         error;
};

struct HyprlandCaptureEnvironment {
    std::vector<CaptureFormatLayout> formats;
    CaptureLimits                    limits;
    std::uint64_t                    maxTotalBytes = 0;
    std::vector<CaptureFormatIssue>  formatIssues;
};

[[nodiscard]] Result<HyprlandCaptureEnvironment>
inspectHyprlandCaptureEnvironment(
    std::span<const OutputGeneration> outputs,
    const CaptureBudget& budget);

} // namespace hfg::v2
