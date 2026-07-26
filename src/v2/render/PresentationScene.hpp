#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Config.hpp"
#include "v2/model/Material.hpp"
#include "v2/model/Session.hpp"
#include "v2/render/MaterialSampling.hpp"
#include "v2/render/OutputGeneration.hpp"
#include "v2/targets/Attachment.hpp"
#include "v2/targets/TargetResolver.hpp"
#include "v2/targets/TargetScene.hpp"

#include <span>
#include <vector>

namespace hfg::v2 {

struct PlannedPresentation {
    ResolvedTarget             target;
    Material                   material;
    ResolvedPresentation       presentation;
    OutputGeneration           output;
    MaterialSamplingFootprint  sampling;
    std::uint64_t              motionTimeMs = 0;

    friend bool operator==(
        const PlannedPresentation&,
        const PlannedPresentation&) = default;
};

struct PresentationScene {
    std::vector<PlannedPresentation>     presentations;
    std::vector<TargetIdentity>          inactive;
    std::vector<TargetIdentity>          suppressed;
    std::vector<TargetResolutionFailure> failures;

    friend bool operator==(
        const PresentationScene&,
        const PresentationScene&) = default;
};

[[nodiscard]] Result<PresentationScene>
buildPresentationScene(
    const TargetScene& targets,
    const ConfigSnapshot* config,
    std::span<const SessionSnapshot> sessions,
    std::span<const OutputGeneration> outputs,
    std::uint64_t nowMs);

} // namespace hfg::v2
