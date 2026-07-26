#pragma once

#include "v2/core/Result.hpp"
#include "v2/targets/Attachment.hpp"

#include <cstdint>

namespace hfg::v2 {

[[nodiscard]] Result<ResolvedTarget>
resolveTargetMotion(
    const ResolvedTarget& target,
    std::uint64_t nowMs);

} // namespace hfg::v2
