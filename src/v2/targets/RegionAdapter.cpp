#include "v2/targets/RegionAdapter.hpp"

#include <utility>

namespace hfg::v2 {
namespace {

Result<std::optional<ResolvedAttachment>> invalid(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<std::optional<ResolvedAttachment>>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

} // namespace

Result<std::optional<ResolvedAttachment>> resolveRegionAttachment(
    TargetIdentity identity,
    const Target& target,
    const OutputGeneration& output) {
    if (identity.owner.empty() ||
        identity.targetId.empty() ||
        identity.targetId != target.id)
        return invalid(
            ErrorCode::InvalidRequest,
            "identity",
            "owner and matching target id are required");
    if (target.kind != TargetKind::Region)
        return invalid(
            ErrorCode::InvalidTarget,
            "target.kind",
            "region adapter requires a region target");
    const auto* selector = std::get_if<RegionSelector>(&target.selector);
    if (!selector)
        return invalid(
            ErrorCode::InvalidTarget,
            "target.selector",
            "region target requires an output selector");
    if (!target.geometry)
        return invalid(
            ErrorCode::InvalidTarget,
            "target.geometry",
            "region target requires output-local geometry");
    if (!target.stage)
        return invalid(
            ErrorCode::InvalidTarget,
            "target.stage",
            "region target requires a render stage");
    if (output.generation == 0U)
        return invalid(
            ErrorCode::StaleGeneration,
            "output.generation",
            "output generation must not be zero");
    if (auto validation = validateOutputSnapshot(output.snapshot); !validation)
        return Result<std::optional<ResolvedAttachment>>::failure(
            validation.error());
    if (selector->output != output.snapshot.name)
        return invalid(
            ErrorCode::UnresolvedTarget,
            "target.selector.output",
            "selected output is not the supplied output generation");
    if (!target.enabled)
        return Result<std::optional<ResolvedAttachment>>::success(
            std::nullopt);

    return Result<std::optional<ResolvedAttachment>>::success(
        ResolvedAttachment{
            .identity = std::move(identity),
            .kind = TargetKind::Region,
            .objectToken = output.snapshot.objectToken,
            .globalGeometry = Rect{
                .x = output.snapshot.logicalX + target.geometry->x,
                .y = output.snapshot.logicalY + target.geometry->y,
                .width = target.geometry->width,
                .height = target.geometry->height,
            },
            .stage = *target.stage,
            .outputFilter = output.snapshot.name,
            .opacity = 1.0,
        });
}

} // namespace hfg::v2
