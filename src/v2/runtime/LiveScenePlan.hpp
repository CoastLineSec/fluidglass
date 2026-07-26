#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/DirectScanoutLeasePlan.hpp"
#include "v2/render/PresentationScene.hpp"
#include "v2/targets/WindowAttachmentPlan.hpp"

#include <span>
#include <vector>

namespace hfg::v2 {

struct LiveSceneBindings {
    std::vector<WindowAttachmentState> windowAttachments;
    std::vector<DirectScanoutLease> directScanoutLeases;

    friend bool operator==(const LiveSceneBindings&,
                           const LiveSceneBindings&) = default;
};

[[nodiscard]] Result<LiveSceneBindings>
planLiveSceneBindings(std::span<const PlannedPresentation> presentations);

} // namespace hfg::v2
