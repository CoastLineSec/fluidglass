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

} // namespace

Result<PresentationScene>
buildPresentationScene(
    const TargetScene& targets,
    const ConfigSnapshot* config,
    std::span<const SessionSnapshot> sessions,
    std::span<const OutputGeneration> outputs,
    std::uint64_t nowMs) {
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
            // The target resolved and its geometry is valid — it simply lands
            // on no current output. The attachment still names its owning
            // output where one exists, so liveness can keep that row
            // accounted for while the surface is parked off-screen.
            scene.inactive.push_back({
                .identity = identity,
                .reason = TargetInactiveReason::Offscreen,
                .output = movingTarget.value().attachment.outputFilter,
                .stage = movingTarget.value().attachment.stage,
            });
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

ReadinessState readinessFailureState(const Error& error, bool captureBoundary) {
    if (error.code == ErrorCode::ResourceLimited)
        return ReadinessState::ResourceLimited;
    if (captureBoundary)
        return ReadinessState::CaptureFailed;
    if (error.code == ErrorCode::UnsupportedOperation ||
        error.code == ErrorCode::UnsupportedTarget)
        return ReadinessState::Unsupported;
    return ReadinessState::Unresolved;
}

void reconcilePresentationReadiness(
    ReadinessTracker& readiness,
    const PresentationScene& scene,
    std::span<const SessionSnapshot> sessions,
    const std::set<std::pair<PresentationKey, std::uint64_t>>&
        previousMembership,
    std::span<const KnownOutput> knownOutputs) {
    // Config-rule targets exist only in the resolved scene, so the scene is
    // where they enter readiness — and where they leave it.
    std::set<TargetIdentity> configSeen;
    const auto ensureConfigAccepted = [&](const TargetIdentity& identity) {
        if (identity.owner != CONFIG_TARGET_OWNER)
            return;
        configSeen.insert(identity);
        if (!readiness.target(identity))
            static_cast<void>(readiness.accept(identity));
    };
    for (const auto& planned : scene.presentations)
        ensureConfigAccepted(planned.presentation.key.identity);
    for (const auto& failed : scene.failures)
        ensureConfigAccepted(failed.identity);
    for (const auto& inactive : scene.inactive)
        ensureConfigAccepted(inactive.identity);
    for (const auto& suppressed : scene.suppressed)
        ensureConfigAccepted(suppressed);

    std::set<PresentationKey> currentKeys;
    for (const auto& planned : scene.presentations) {
        const auto& key = planned.presentation.key;
        const auto targetRecord = readiness.target(key.identity);
        if (!targetRecord)
            continue;
        if (targetRecord->state != ReadinessState::Accepted)
            static_cast<void>(readiness.accept(key.identity));
        currentKeys.insert(key);
        const auto unchanged = previousMembership.contains(
            {key, planned.presentation.attachmentToken});
        if (!unchanged && readiness.presentation(key))
            readiness.erasePresentation(key);
        if (!readiness.presentation(key)) {
            static_cast<void>(readiness.resolvePresentation(key));
            static_cast<void>(
                readiness.transition(key, ReadinessState::Attached));
        }
    }

    // Off-screen targets keep a presentation-level record on their owning
    // output so its liveness row stays accounted for; those keys must survive
    // the stale-presentation sweep or they would churn every refresh.
    std::set<PresentationKey> inactiveKeys;
    const auto inactiveKeyFor =
        [&](const InactiveTarget& inactive) -> std::optional<PresentationKey> {
        if (!inactive.output)
            return std::nullopt;
        for (const auto& known : knownOutputs)
            if (known.name == *inactive.output)
                return PresentationKey{
                    .identity = inactive.identity,
                    .output = known.name,
                    .outputGeneration = known.generation,
                    .stage = inactive.stage,
                };
        return std::nullopt;
    };
    for (const auto& inactive : scene.inactive)
        if (const auto key = inactiveKeyFor(inactive))
            inactiveKeys.insert(*key);

    const auto erasePresentationsOutsideScene =
        [&](const TargetIdentity& identity) {
            for (const auto& [key, record] : readiness.presentations(identity)) {
                static_cast<void>(record);
                if (!currentKeys.contains(key) && !inactiveKeys.contains(key))
                    readiness.erasePresentation(key);
            }
        };
    for (const auto& session : sessions)
        for (const auto& target : session.targets)
            erasePresentationsOutsideScene({
                .owner = session.owner,
                .targetId = target.id,
            });

    for (const auto& failed : scene.failures)
        if (readiness.target(failed.identity))
            static_cast<void>(readiness.failTarget(
                failed.identity, readinessFailureState(failed.error),
                failed.error.message));

    // A target that resolves but contributes no presentation is neither
    // planned above nor failed here, so nothing would ever move it off
    // "accepted". Left unreported it is indistinguishable from a target still
    // being resolved, and a client waiting on a drawn presentation waits
    // forever with no failure and no timeout to observe.
    const auto reportInactive = [&](const TargetIdentity& identity,
                                    TargetInactiveReason reason) {
        const auto record = readiness.target(identity);
        if (!record)
            return;
        auto detail = std::string(targetInactiveReasonDetail(reason));
        // Re-recording an unchanged fact would bump the sequence every
        // refresh, turning a permanently inactive target into permanent
        // churn for any client diffing on it.
        if (record->state == ReadinessState::Inactive &&
            record->detail == detail)
            return;
        static_cast<void>(readiness.failTarget(
            identity, ReadinessState::Inactive, std::move(detail)));
    };
    for (const auto& inactive : scene.inactive) {
        reportInactive(inactive.identity, inactive.reason);
        if (const auto key = inactiveKeyFor(inactive))
            static_cast<void>(readiness.markPresentationInactive(
                *key,
                std::string(targetInactiveReasonDetail(inactive.reason))));
    }
    for (const auto& suppressed : scene.suppressed)
        reportInactive(suppressed, TargetInactiveReason::Suppressed);

    for (const auto& identity : readiness.targetIdentities())
        if (identity.owner == CONFIG_TARGET_OWNER &&
            !configSeen.contains(identity))
            readiness.erase(identity);
}

} // namespace hfg::v2
