#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Config.hpp"
#include "v2/model/Session.hpp"
#include "v2/render/OutputGeneration.hpp"
#include "v2/targets/Attachment.hpp"
#include "v2/targets/LayerAdapter.hpp"
#include "v2/targets/TargetResolver.hpp"
#include "v2/targets/WindowAdapter.hpp"

#include <span>
#include <vector>

namespace hfg::v2 {

struct TargetScene {
    std::vector<ResolvedTarget>          effective;
    std::vector<InactiveTarget>          inactive;
    std::vector<TargetIdentity>          suppressed;
    std::vector<TargetResolutionFailure> failures;

    friend bool operator==(const TargetScene&, const TargetScene&) = default;
};

[[nodiscard]] Result<TargetScene>
buildTargetScene(
    const ConfigSnapshot* config,
    std::span<const SessionSnapshot> sessions,
    std::span<const WindowSnapshot> windows,
    std::span<const LayerSurfaceSnapshot> layers,
    std::span<const OutputGeneration> outputs);

} // namespace hfg::v2
