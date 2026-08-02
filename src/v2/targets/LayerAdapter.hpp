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
    /**
     * The part of the surface the client actually presents, surface-local.
     *
     * Derived from the surface's committed input region. A shell whose layer
     * surface is larger than its visible panel — headroom so the panel can
     * slide without resizing — already describes the visible rectangle there,
     * because that is what makes clicks land on the panel and not on the empty
     * space beside it. It is committed atomically with the surface content, so
     * it cannot disagree with what was drawn.
     *
     * Absent when the client set no input region, which means the whole
     * surface.
     */
    std::optional<Rect> contentGeometry = std::nullopt;
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
