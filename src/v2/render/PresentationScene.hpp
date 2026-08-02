#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Config.hpp"
#include "v2/model/Material.hpp"
#include "v2/model/PresentationHandoff.hpp"
#include "v2/model/Session.hpp"
#include "v2/model/VisibilityTransition.hpp"
#include "v2/render/MaterialSampling.hpp"
#include "v2/render/OutputGeneration.hpp"
#include "v2/targets/Attachment.hpp"
#include "v2/targets/TargetResolver.hpp"
#include "v2/targets/TargetScene.hpp"

#include "v2/model/Readiness.hpp"

#include <set>
#include <span>
#include <utility>
#include <vector>

namespace hfg::v2 {

struct PlannedPresentation {
    ResolvedTarget             target;
    Material                   material;
    ResolvedPresentation       presentation;
    OutputGeneration           output;
    MaterialSamplingFootprint  sampling;
    std::optional<MappedGeometry> transitionEnvelope = std::nullopt;
    std::uint64_t              motionTimeMs = 0;

    friend bool operator==(
        const PlannedPresentation&,
        const PlannedPresentation&) = default;
};

struct PresentationHandoffPair {
    PresentationKey successor;
    PresentationKey fallback;

    friend bool operator==(const PresentationHandoffPair&,
                           const PresentationHandoffPair&) = default;
};

struct PresentationScene {
    std::vector<PlannedPresentation>     presentations;
    std::vector<PresentationHandoffPair> handoffs = {};
    std::vector<InactiveTarget>          inactive;
    std::vector<TargetIdentity>          suppressed;
    std::vector<TargetResolutionFailure> failures;

    friend bool operator==(
        const PresentationScene&,
        const PresentationScene&) = default;
};

/**
 * Folds a resolved presentation scene into the readiness tracker.
 *
 * Session targets are accepted by session.replace; config-rule targets never
 * pass through a session, so they are accepted here on first sight and their
 * records are dropped when the rule stops matching anything. Re-reporting an
 * unchanged inactive target is skipped so a permanently inactive target does
 * not bump the readiness sequence on every refresh.
 */
/** Maps a resolution or render error onto the readiness vocabulary. */
[[nodiscard]] ReadinessState readinessFailureState(
    const Error& error,
    bool captureBoundary = false);

void reconcilePresentationReadiness(
    ReadinessTracker& readiness,
    const PresentationScene& scene,
    std::span<const SessionSnapshot> sessions,
    const std::set<std::pair<PresentationKey, std::uint64_t>>&
        previousMembership);

[[nodiscard]] Result<PresentationScene>
buildPresentationScene(
    const TargetScene& targets,
    const ConfigSnapshot* config,
    std::span<const SessionSnapshot> sessions,
    std::span<const OutputGeneration> outputs,
    std::uint64_t nowMs,
    PresentationHandoffTracker* handoffs = nullptr,
    VisibilityTransitionTracker* visibility = nullptr);

} // namespace hfg::v2
