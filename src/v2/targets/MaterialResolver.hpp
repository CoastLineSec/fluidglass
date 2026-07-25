#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Config.hpp"
#include "v2/model/Material.hpp"
#include "v2/model/Session.hpp"
#include "v2/targets/Attachment.hpp"

#include <span>

namespace hfg::v2 {

[[nodiscard]] Result<Material> resolveTargetMaterial(
    const ResolvedTarget& target,
    const ConfigSnapshot* config,
    std::span<const SessionSnapshot> sessions);

} // namespace hfg::v2
