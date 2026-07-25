#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Session.hpp"
#include "v2/targets/Attachment.hpp"
#include "v2/targets/TargetResolver.hpp"

#include <span>
#include <vector>

namespace hfg::v2 {

struct EffectiveTargetSelection {
    std::vector<ResolvedTarget>          targets;
    std::vector<TargetIdentity>          suppressed;
    std::vector<TargetResolutionFailure> conflicts;

    friend bool operator==(const EffectiveTargetSelection&, const EffectiveTargetSelection&) = default;
};

[[nodiscard]] Result<EffectiveTargetSelection>
selectEffectiveTargets(
    std::span<const ResolvedTarget> durable,
    std::span<const ResolvedTarget> leased,
    std::span<const SessionSnapshot> sessions);

} // namespace hfg::v2
