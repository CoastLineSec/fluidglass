#include "v2/render/PresentationScene.hpp"

#include "v2/core/Limits.hpp"
#include "v2/targets/MaterialResolver.hpp"
#include "v2/targets/TargetMotion.hpp"

#include <iterator>
#include <set>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

Result<PresentationScene> failure(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<PresentationScene>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

const OutputGeneration* findOutput(
    std::span<const OutputGeneration> outputs,
    const PresentationKey& key) {
    for (const auto& output : outputs) {
        if (output.snapshot.name == key.output &&
            output.generation == key.outputGeneration)
            return &output;
    }
    return nullptr;
}

Result<ResolvedTarget> applyPresentationMorph(
    ResolvedTarget target,
    const PresentationHandoffTracker* handoffs,
    std::uint64_t nowMs) {
    if (!handoffs)
        return Result<ResolvedTarget>::success(std::move(target));
    const auto record = handoffs->target(target.attachment.identity);
    if (!record || !record->morph ||
        record->morph->state != PresentationMorphState::Active)
        return Result<ResolvedTarget>::success(std::move(target));
    if (target.attachment.kind != TargetKind::Layer ||
        !target.definition.geometry ||
        !std::holds_alternative<RoundedRectShape>(
            target.definition.shape))
        return Result<ResolvedTarget>::failure({
            .code = ErrorCode::UnsupportedTarget,
            .path = "presentation.morph",
            .message = "active morph no longer references a rounded layer target",
        });
    auto resolved = resolvePresentationMorph(*record->morph, nowMs);
    if (!resolved)
        return Result<ResolvedTarget>::failure(resolved.error());
    const auto destination = *target.definition.geometry;
    const auto originX =
        target.attachment.globalGeometry.x - destination.x;
    const auto originY =
        target.attachment.globalGeometry.y - destination.y;
    const auto global = [originX, originY](const Rect& rect) {
        return Rect{
            .x = originX + rect.x,
            .y = originY + rect.y,
            .width = rect.width,
            .height = rect.height,
        };
    };
    target.definition.geometry = resolved.value().current.rect;
    target.definition.shape = RoundedRectShape{
        .radius = resolved.value().current.radius,
    };
    target.attachment.globalGeometry =
        global(resolved.value().current.rect);
    target.transitionEnvelopeGlobal =
        global(resolved.value().envelope);
    target.transitionAnchorMs = record->morph->anchorMs;
    target.transitionActive =
        target.transitionActive || resolved.value().active;
    return Result<ResolvedTarget>::success(std::move(target));
}

} // namespace

Result<PresentationScene>
buildPresentationScene(
    const TargetScene& targets,
    const ConfigSnapshot* config,
    std::span<const SessionSnapshot> sessions,
    std::span<const OutputGeneration> outputs,
    std::uint64_t nowMs,
    const PresentationHandoffTracker* handoffs) {
    if (targets.effective.size() >
        Limits::MAX_COMPOSITOR_OBJECTS)
        return failure(
            ErrorCode::ResourceLimited,
            "targets.effective",
            "effective target count exceeds the supported limit");
    if (outputs.size() > Limits::MAX_COMPOSITOR_OBJECTS)
        return failure(
            ErrorCode::ResourceLimited,
            "outputs",
            "output count exceeds the supported limit");
    std::set<std::string_view> outputNames;
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        const auto& output = outputs[index];
        if (!outputNames.insert(output.snapshot.name).second)
            return failure(
                ErrorCode::StaleGeneration,
                "outputs[" + std::to_string(index) + "]",
                "more than one current generation has the same output name");
        if (output.generation == 0U)
            return failure(
                ErrorCode::StaleGeneration,
                "outputs[" + std::to_string(index) + "].generation",
                "current output generation must not be zero");
        if (auto validation =
                validateOutputSnapshot(output.snapshot);
            !validation)
            return Result<PresentationScene>::failure({
                .code = validation.error().code,
                .path = "outputs[" +
                    std::to_string(index) + "]." +
                    validation.error().path,
                .message = validation.error().message,
            });
    }

    PresentationScene scene{
        .presentations = {},
        .handoffs = {},
        .inactive = targets.inactive,
        .suppressed = targets.suppressed,
        .failures = targets.failures,
    };
    std::set<TargetIdentity> identities;
    for (const auto& target : targets.effective) {
        const auto& identity = target.attachment.identity;
        if (target.definition.id != identity.targetId ||
            target.definition.kind != target.attachment.kind)
            return failure(
                ErrorCode::InvalidTarget,
                "targets.effective",
                "resolved target definition and attachment identity differ");
        if (!identities.insert(identity).second)
            return failure(
                ErrorCode::InvalidTarget,
                "targets.effective",
                "effective target identities must be unique");

        auto movingTarget = resolveTargetMotion(
            target,
            nowMs);
        if (!movingTarget) {
            scene.failures.push_back({
                .identity = identity,
                .error = movingTarget.error(),
            });
            continue;
        }
        movingTarget = applyPresentationMorph(
            std::move(movingTarget.value()),
            handoffs,
            nowMs);
        if (!movingTarget) {
            scene.failures.push_back({
                .identity = identity,
                .error = movingTarget.error(),
            });
            continue;
        }
        auto material = resolveTargetMaterial(
            movingTarget.value(),
            config,
            sessions);
        if (!material) {
            scene.failures.push_back({
                .identity = identity,
                .error = material.error(),
            });
            continue;
        }
        auto presentations = resolvePresentations(
            movingTarget.value().attachment,
            outputs);
        if (!presentations) {
            scene.failures.push_back({
                .identity = identity,
                .error = presentations.error(),
            });
            continue;
        }
        if (presentations.value().empty()) {
            scene.inactive.push_back(identity);
            continue;
        }

        std::vector<PlannedPresentation> planned;
        planned.reserve(presentations.value().size());
        std::optional<Error> planningFailure;
        for (auto& presentation : presentations.value()) {
            const auto* output = findOutput(
                outputs,
                presentation.key);
            if (!output) {
                planningFailure = Error{
                    .code = ErrorCode::StaleGeneration,
                    .path = "presentation.output",
                    .message = "presentation references a non-current output generation",
                };
                break;
            }
            auto sampling = resolveMaterialSampling(
                material.value(),
                movingTarget.value()
                    .attachment.globalGeometry.width,
                movingTarget.value()
                    .attachment.globalGeometry.height,
                output->snapshot.scale);
            if (!sampling) {
                planningFailure = sampling.error();
                break;
            }
            std::optional<MappedGeometry> transitionEnvelope;
            if (movingTarget.value().transitionEnvelopeGlobal) {
                auto mappedEnvelope = mapGlobalLogicalRect(
                    *movingTarget.value().transitionEnvelopeGlobal,
                    *output);
                if (!mappedEnvelope) {
                    planningFailure = mappedEnvelope.error();
                    break;
                }
                if (!mappedEnvelope.value()) {
                    planningFailure = Error{
                        .code = ErrorCode::UnresolvedTarget,
                        .path = "presentation.morph.envelope",
                        .message = "morph envelope does not intersect its output",
                    };
                    break;
                }
                transitionEnvelope =
                    std::move(*mappedEnvelope.value());
            }
            planned.push_back({
                .target = movingTarget.value(),
                .material = material.value(),
                .presentation = std::move(presentation),
                .output = *output,
                .sampling = std::move(sampling.value()),
                .transitionEnvelope = std::move(transitionEnvelope),
                .motionTimeMs = nowMs,
            });
        }
        if (planningFailure) {
            scene.failures.push_back({
                .identity = identity,
                .error = std::move(*planningFailure),
            });
            continue;
        }
        if (planned.size() >
            Limits::MAX_CAPTURE_REQUESTS -
                scene.presentations.size()) {
            scene.failures.push_back({
                .identity = identity,
                .error = Error{
                    .code = ErrorCode::ResourceLimited,
                    .path = "presentations",
                    .message = "presentation count exceeds the supported limit",
                },
            });
            continue;
        }
        scene.presentations.insert(
            scene.presentations.end(),
            std::make_move_iterator(planned.begin()),
            std::make_move_iterator(planned.end()));
    }
    return Result<PresentationScene>::success(std::move(scene));
}

} // namespace hfg::v2
