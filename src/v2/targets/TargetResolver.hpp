#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Session.hpp"
#include "v2/render/OutputGeneration.hpp"
#include "v2/targets/Attachment.hpp"
#include "v2/targets/LayerAdapter.hpp"
#include "v2/targets/WindowAdapter.hpp"

#include <span>
#include <vector>

namespace hfg::v2 {

struct TargetResolutionFailure {
    TargetIdentity identity;
    Error          error;

    friend bool operator==(const TargetResolutionFailure&, const TargetResolutionFailure&) = default;
};

struct TargetResolutionBatch {
    std::vector<ResolvedTarget>          resolved;
    std::vector<TargetIdentity>          inactive;
    std::vector<TargetResolutionFailure> failures;

    friend bool operator==(const TargetResolutionBatch&, const TargetResolutionBatch&) = default;
};

[[nodiscard]] Result<TargetResolutionBatch>
resolveSessionTargets(
    std::span<const SessionSnapshot> sessions,
    std::span<const WindowSnapshot> windows,
    std::span<const LayerSurfaceSnapshot> layers,
    std::span<const OutputGeneration> outputs);

} // namespace hfg::v2
