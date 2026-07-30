#include "v2/render/PresentationScene.hpp"

#include "v2/core/Limits.hpp"
#include "v2/targets/MaterialResolver.hpp"
#include "v2/targets/TargetMotion.hpp"

#include <cmath>
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
    PresentationHandoffTracker* handoffs,
    std::span<const OutputGeneration> outputs,
    std::uint64_t nowMs) {
    if (!handoffs)
        return Result<ResolvedTarget>::success(std::move(target));
    const auto record = handoffs->target(target.attachment.identity);
    if (!record || !record->morph ||
        (record->morph->state != PresentationMorphState::Active &&
         record->morph->state != PresentationMorphState::Settling))
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
    const auto surfaceOriginX =
        target.attachment.globalGeometry.x - destination.x;
    const auto surfaceOriginY =
        target.attachment.globalGeometry.y - destination.y;
    const auto coordinateSpace = record->morph->coordinateSpace;
    const OutputGeneration* output = nullptr;
    if (coordinateSpace ==
        PresentationHandoffRequest::MorphCoordinateSpace::OutputLocal) {
        if (!target.attachment.outputFilter)
            return Result<ResolvedTarget>::failure({
                .code = ErrorCode::UnresolvedTarget,
                .path = "presentation.morph.output",
                .message = "output-local morph has no resolved output",
            });
        const auto found = std::ranges::find(
            outputs,
            *target.attachment.outputFilter,
            [](const OutputGeneration& candidate) {
                return candidate.snapshot.name;
            });
        if (found == outputs.end())
            return Result<ResolvedTarget>::failure({
                .code = ErrorCode::StaleGeneration,
                .path = "presentation.morph.output",
                .message = "output-local morph references a non-current output",
            });
        output = &*found;
    }
    const auto global = [&](const Rect& rect) {
        if (output)
            return Rect{
                .x = output->snapshot.logicalX + rect.x,
                .y = output->snapshot.logicalY + rect.y,
                .width = rect.width,
                .height = rect.height,
            };
        return Rect{
            .x = surfaceOriginX + rect.x,
            .y = surfaceOriginY + rect.y,
            .width = rect.width,
            .height = rect.height,
        };
    };
    const auto contains = [](const Rect& bounds, const Rect& rect) {
        constexpr double EPSILON = 0.001;
        return rect.x >= bounds.x - EPSILON &&
            rect.y >= bounds.y - EPSILON &&
            rect.x + rect.width <=
                bounds.x + bounds.width + EPSILON &&
            rect.y + rect.height <=
                bounds.y + bounds.height + EPSILON;
    };
    const auto intersects = [](const Rect& left, const Rect& right) {
        return left.x < right.x + right.width &&
            left.x + left.width > right.x &&
            left.y < right.y + right.height &&
            left.y + left.height > right.y;
    };
    const auto nearlyEqual = [](const Rect& left, const Rect& right) {
        constexpr double EPSILON = 0.001;
        return std::abs(left.x - right.x) <= EPSILON &&
            std::abs(left.y - right.y) <= EPSILON &&
            std::abs(left.width - right.width) <= EPSILON &&
            std::abs(left.height - right.height) <= EPSILON;
    };
    if (output) {
        const Rect outputBounds{
            .x = output->snapshot.logicalX,
            .y = output->snapshot.logicalY,
            .width = output->snapshot.logicalWidth,
            .height = output->snapshot.logicalHeight,
        };
        const auto currentGlobal = global(resolved.value().current.rect);
        const auto envelopeGlobal = global(resolved.value().envelope);
        if (!contains(outputBounds, currentGlobal) ||
            !contains(outputBounds, envelopeGlobal))
            return Result<ResolvedTarget>::failure({
                .code = ErrorCode::InvalidTarget,
                .path = "presentation.morph.output",
                .message = "output-local morph is outside its output",
            });
        if (!target.attachment.containerGlobalGeometry ||
            !intersects(*target.attachment.containerGlobalGeometry,
                        currentGlobal))
            return Result<ResolvedTarget>::failure({
                .code = ErrorCode::UnresolvedTarget,
                .path = "presentation.morph.surface",
                .message = "output-local morph does not intersect its live layer surface",
            });
        const auto destinationGlobal =
            global(record->morph->destination.rect);
        const auto destinationMatches =
            nearlyEqual(target.attachment.globalGeometry,
                        destinationGlobal) &&
            std::get<RoundedRectShape>(target.definition.shape).radius ==
                record->morph->destination.radius;
        if (resolved.value().progress >= 1.0 &&
            destinationMatches) {
            handoffs->settleMorph(target.attachment.identity);
            return Result<ResolvedTarget>::success(std::move(target));
        }
    }
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
        target.transitionActive || resolved.value().active ||
        record->morph->state == PresentationMorphState::Settling;
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
    PresentationHandoffTracker* handoffs) {
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
            outputs,
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
