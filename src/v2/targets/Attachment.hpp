#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Readiness.hpp"
#include "v2/model/Target.hpp"
#include "v2/render/Geometry.hpp"
#include "v2/render/OutputGeneration.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hfg::v2 {

struct ResolvedAttachment {
    TargetIdentity             identity;
    TargetKind                 kind = TargetKind::Region;
    std::uint64_t              objectToken = 0;
    Rect                       globalGeometry;
    RenderStage                stage = RenderStage::PostWindows;
    std::optional<std::string> outputFilter;
    double                     opacity = 1.0;

    friend bool operator==(const ResolvedAttachment&, const ResolvedAttachment&) = default;
};

struct ResolvedPresentation {
    PresentationKey key;
    std::uint64_t    attachmentToken = 0;
    MappedGeometry   geometry;
    double           opacity = 1.0;

    friend bool operator==(const ResolvedPresentation&, const ResolvedPresentation&) = default;
};

[[nodiscard]] Result<std::vector<ResolvedPresentation>> resolvePresentations(
    const ResolvedAttachment& attachment,
    std::span<const OutputGeneration> outputs);

} // namespace hfg::v2
