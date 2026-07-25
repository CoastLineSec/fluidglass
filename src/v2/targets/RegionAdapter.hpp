#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Readiness.hpp"
#include "v2/model/Target.hpp"
#include "v2/render/OutputGeneration.hpp"
#include "v2/targets/Attachment.hpp"

#include <optional>

namespace hfg::v2 {

[[nodiscard]] Result<std::optional<ResolvedAttachment>>
resolveRegionAttachment(
    TargetIdentity identity,
    const Target& target,
    const OutputGeneration& output);

} // namespace hfg::v2
