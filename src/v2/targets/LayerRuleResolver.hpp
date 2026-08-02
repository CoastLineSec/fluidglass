#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Config.hpp"
#include "v2/targets/Attachment.hpp"
#include "v2/targets/TargetResolver.hpp"
#include "v2/targets/LayerAdapter.hpp"

#include <span>
#include <vector>

namespace hfg::v2 {

[[nodiscard]] Result<RuleResolution>
resolveLayerRules(
    const ConfigSnapshot& config,
    std::span<const LayerSurfaceSnapshot> surfaces);

} // namespace hfg::v2
