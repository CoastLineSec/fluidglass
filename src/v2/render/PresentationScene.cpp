#include "v2/render/PresentationScene.hpp"

#include "v2/core/Limits.hpp"
#include "v2/targets/MaterialResolver.hpp"

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
    std::span<const OutputGeneration> outputs) {
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

        auto material = resolveTargetMaterial(
            target,
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
            target.attachment,
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
                target.attachment.globalGeometry.width,
                target.attachment.globalGeometry.height,
                output->snapshot.scale);
            if (!sampling) {
                planningFailure = sampling.error();
                break;
            }
            planned.push_back({
                .target = target,
                .material = material.value(),
                .presentation = std::move(presentation),
                .output = *output,
                .sampling = std::move(sampling.value()),
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
