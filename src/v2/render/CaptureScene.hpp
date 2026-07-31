#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/CapturePlan.hpp"
#include "v2/render/PresentationScene.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hfg::v2 {

struct CaptureFormatLayout {
    std::uint32_t renderFormat = 0;
    std::uint32_t bytesPerPixel = 0;

    friend bool operator==(
        const CaptureFormatLayout&,
        const CaptureFormatLayout&) = default;
};

struct PresentationCaptureFailure {
    PresentationKey key;
    Error           error;

    friend bool operator==(
        const PresentationCaptureFailure&,
        const PresentationCaptureFailure&) = default;
};

struct CaptureAssignment {
    PlannedPresentation presentation;
    CapturePlan         required;
    std::size_t         captureIndex = 0;

    friend bool operator==(
        const CaptureAssignment&,
        const CaptureAssignment&) = default;
};

struct CaptureScene {
    std::vector<CapturePlan>                captures;
    std::vector<CaptureAssignment>          assignments;
    std::vector<PresentationHandoffPair>    handoffs = {};
    std::vector<InactiveTarget>             inactive;
    std::vector<TargetIdentity>             suppressed;
    std::vector<TargetResolutionFailure>    targetFailures;
    std::vector<PresentationCaptureFailure> captureFailures;

    friend bool operator==(
        const CaptureScene&,
        const CaptureScene&) = default;
};

[[nodiscard]] Result<CaptureScene> buildCaptureScene(
    const PresentationScene& presentations,
    std::span<const CaptureFormatLayout> formats,
    const CaptureLimits& limits);

} // namespace hfg::v2
