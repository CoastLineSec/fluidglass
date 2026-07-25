#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Readiness.hpp"
#include "v2/model/Target.hpp"
#include "v2/targets/Attachment.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace hfg::v2 {

enum class LayerLevel {
    Background,
    Bottom,
    Top,
    Overlay,
};

struct LayerSurfaceSnapshot {
    std::string   namespaceName;
    std::uint64_t objectToken = 0;
    std::string   output;
    Rect          globalGeometry;
    LayerLevel    level = LayerLevel::Top;
    double        opacity = 1.0;
    bool          mapped = false;
    bool          fadingOut = false;
    bool          readyToDelete = false;

    friend bool operator==(const LayerSurfaceSnapshot&, const LayerSurfaceSnapshot&) = default;
};

[[nodiscard]] Result<std::optional<ResolvedAttachment>>
resolveLayerAttachment(
    TargetIdentity identity,
    const Target& target,
    std::span<const LayerSurfaceSnapshot> surfaces);

} // namespace hfg::v2
