#include "v2/targets/TargetResolver.hpp"

#include "v2/core/Limits.hpp"
#include "v2/targets/RegionAdapter.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <utility>

namespace hfg::v2 {
namespace {

Result<TargetResolutionBatch> invalid(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<TargetResolutionBatch>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

std::optional<OutputGeneration> selectedOutput(
    std::span<const OutputGeneration> outputs,
    std::string_view name,
    bool& ambiguous) {
    std::optional<OutputGeneration> result;
    ambiguous = false;
    for (const auto& output : outputs) {
        if (output.snapshot.name != name)
            continue;
        if (result) {
            ambiguous = true;
            return std::nullopt;
        }
        result = output;
    }
    return result;
}

} // namespace

Result<TargetResolutionBatch>
resolveSessionTargets(
    std::span<const SessionSnapshot> sessions,
    std::span<const WindowSnapshot> windows,
    std::span<const LayerSurfaceSnapshot> layers,
    std::span<const OutputGeneration> outputs) {
    if (sessions.size() > Limits::MAX_SESSIONS)
        return invalid(
            ErrorCode::ResourceLimited,
            "sessions",
            "session count exceeds the supported limit");
    if (windows.size() > Limits::MAX_COMPOSITOR_OBJECTS ||
        layers.size() > Limits::MAX_COMPOSITOR_OBJECTS ||
        outputs.size() > Limits::MAX_COMPOSITOR_OBJECTS)
        return invalid(
            ErrorCode::ResourceLimited,
            "compositor",
            "compositor object count exceeds the supported limit");

    std::size_t targetCount = 0U;
    std::set<std::string_view> owners;
    for (const auto& session : sessions) {
        if (session.owner.empty())
            return invalid(
                ErrorCode::InvalidRequest,
                "sessions.owner",
                "session owner must not be empty");
        if (!owners.insert(session.owner).second)
            return invalid(
                ErrorCode::InvalidRequest,
                "sessions.owner",
                "session owners must be unique");
        if (session.targets.size() >
            Limits::MAX_TARGETS_PER_SESSION)
            return invalid(
                ErrorCode::ResourceLimited,
                "sessions",
                "session target count exceeds the supported limit");
        if (targetCount >
            Limits::MAX_DYNAMIC_TARGETS -
                session.targets.size())
            return invalid(
                ErrorCode::ResourceLimited,
                "sessions",
                "dynamic target count exceeds the supported limit");
        targetCount += session.targets.size();
    }

    TargetResolutionBatch batch;
    batch.resolved.reserve(targetCount);
    for (const auto& session : sessions) {
        std::set<std::string_view> targetIds;
        for (const auto& target : session.targets) {
            if (target.id.empty() ||
                !targetIds.insert(target.id).second)
                return invalid(
                    ErrorCode::InvalidTarget,
                    "sessions.targets.id",
                    "target ids must be non-empty and unique within a session");
            TargetIdentity identity{
                .owner = session.owner,
                .targetId = target.id,
            };
            Result<std::optional<ResolvedAttachment>> attachment =
                Result<std::optional<ResolvedAttachment>>::failure({
                    .code = ErrorCode::UnsupportedTarget,
                    .path = "target.kind",
                    .message = "unsupported target kind",
                });

            switch (target.kind) {
                case TargetKind::Window:
                    attachment = resolveWindowAttachment(
                        identity,
                        target,
                        windows);
                    break;
                case TargetKind::Layer:
                    attachment = resolveLayerAttachment(
                        identity,
                        target,
                        layers);
                    break;
                case TargetKind::Region: {
                    const auto* selector =
                        std::get_if<RegionSelector>(
                            &target.selector);
                    if (!selector) {
                        attachment =
                            Result<std::optional<ResolvedAttachment>>::failure({
                                .code = ErrorCode::InvalidTarget,
                                .path = "target.selector",
                                .message = "region target requires an output selector",
                            });
                        break;
                    }
                    bool ambiguous = false;
                    const auto output = selectedOutput(
                        outputs,
                        selector->output,
                        ambiguous);
                    if (ambiguous) {
                        attachment =
                            Result<std::optional<ResolvedAttachment>>::failure({
                                .code = ErrorCode::StaleGeneration,
                                .path = "outputs",
                                .message = "more than one current output generation has the selected connector",
                            });
                    } else if (!output) {
                        attachment =
                            Result<std::optional<ResolvedAttachment>>::failure({
                                .code = ErrorCode::UnresolvedTarget,
                                .path = "target.selector.output",
                                .message = "selected output is unavailable",
                            });
                    } else {
                        attachment = resolveRegionAttachment(
                            identity,
                            target,
                            *output);
                    }
                    break;
                }
            }

            if (!attachment) {
                batch.failures.push_back({
                    .identity = std::move(identity),
                    .error = attachment.error(),
                });
                continue;
            }
            if (!attachment.value()) {
                batch.inactive.push_back(std::move(identity));
                continue;
            }
            batch.resolved.push_back({
                .definition = target,
                .attachment =
                    std::move(*attachment.value()),
                .roundingPower = 2.0,
                .transitionAnchorMs =
                    session.transitionAnchorMs,
                .transitionActive = false,
            });
        }
    }
    return Result<TargetResolutionBatch>::success(
        std::move(batch));
}

} // namespace hfg::v2
