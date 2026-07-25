#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/CaptureCache.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hfg::v2 {

struct CaptureResourceBinding {
    std::size_t                  captureIndex = 0;
    std::optional<std::uint64_t> retainedToken;
    std::optional<std::size_t>   allocationIndex;

    friend bool operator==(
        const CaptureResourceBinding&,
        const CaptureResourceBinding&) = default;
};

struct CaptureResourcePlan {
    std::vector<CaptureResource>        retain;
    std::vector<CapturePlan>            allocate;
    std::vector<CaptureResource>        retire;
    std::vector<CaptureResourceBinding> bindings;
    std::uint64_t                       totalBytes = 0;
    bool                                allocateBeforeRetire = false;

    friend bool operator==(
        const CaptureResourcePlan&,
        const CaptureResourcePlan&) = default;
};

[[nodiscard]] Result<CaptureResourcePlan>
planCaptureResources(
    std::span<const CaptureResource> current,
    std::span<const CapturePlan> desired,
    std::uint64_t maxTotalBytes);

} // namespace hfg::v2
