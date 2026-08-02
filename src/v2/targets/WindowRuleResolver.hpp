#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Config.hpp"
#include "v2/targets/Attachment.hpp"
#include "v2/targets/TargetResolver.hpp"
#include "v2/targets/WindowAdapter.hpp"

#include <span>
#include <vector>

namespace hfg::v2 {

[[nodiscard]] Result<RuleResolution>
resolveWindowRules(
    const ConfigSnapshot& config,
    std::span<const WindowSnapshot> windows);

} // namespace hfg::v2
