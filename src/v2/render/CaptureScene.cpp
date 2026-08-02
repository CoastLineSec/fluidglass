#include "v2/render/CaptureScene.hpp"

#include "v2/core/Limits.hpp"

#include <array>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

Result<CaptureScene> failure(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<CaptureScene>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

std::optional<std::uint32_t> bytesPerPixel(
    std::span<const CaptureFormatLayout> formats,
    std::uint32_t renderFormat) {
    for (const auto& format : formats) {
        if (format.renderFormat == renderFormat)
            return format.bytesPerPixel;
    }
    return std::nullopt;
}

} // namespace

Result<CaptureScene> buildCaptureScene(
    const PresentationScene& presentations,
    std::span<const CaptureFormatLayout> formats,
    const CaptureLimits& limits) {
    const std::array<CaptureRequest, 0> noRequests{};
    if (auto validLimits = planCaptures(noRequests, limits);
        !validLimits)
        return Result<CaptureScene>::failure(
            validLimits.error());
    if (formats.size() > Limits::MAX_CAPTURE_REQUESTS)
        return failure(
            ErrorCode::ResourceLimited,
            "formats",
            "capture format count exceeds the supported limit");
    std::set<std::uint32_t> formatIds;
    for (std::size_t index = 0; index < formats.size(); ++index) {
        const auto& format = formats[index];
        if (format.renderFormat == 0U ||
            format.bytesPerPixel == 0U ||
            format.bytesPerPixel > 64U)
            return failure(
                ErrorCode::InvalidRequest,
                "formats[" + std::to_string(index) + "]",
                "capture format and byte size must be valid");
        if (!formatIds.insert(format.renderFormat).second)
            return failure(
                ErrorCode::InvalidRequest,
                "formats[" + std::to_string(index) + "]",
                "capture formats must be unique");
    }
    if (presentations.presentations.size() >
        Limits::MAX_CAPTURE_REQUESTS)
        return failure(
            ErrorCode::ResourceLimited,
            "presentations",
            "presentation count exceeds the supported capture limit");

    CaptureScene scene{
        .captures = {},
        .assignments = {},
        .inactive = presentations.inactive,
        .suppressed = presentations.suppressed,
        .targetFailures = presentations.failures,
        .captureFailures = {},
    };
    std::vector<CaptureRequest> acceptedRequests;
    std::vector<CaptureAssignment> acceptedAssignments;
    std::set<PresentationKey> presentationKeys;
    acceptedRequests.reserve(
        presentations.presentations.size());
    acceptedAssignments.reserve(
        presentations.presentations.size());
    for (const auto& presentation :
         presentations.presentations) {
        const auto& key = presentation.presentation.key;
        if (key.identity !=
                presentation.target.attachment.identity ||
            key.output != presentation.output.snapshot.name ||
            key.outputGeneration !=
                presentation.output.generation ||
            key.stage !=
                presentation.target.attachment.stage)
            return failure(
                ErrorCode::InvalidRequest,
                "presentations",
                "presentation identity differs from its target or output");
        if (!presentationKeys.insert(key).second)
            return failure(
                ErrorCode::InvalidRequest,
                "presentations",
                "presentation keys must be unique");
        const auto formatSize = bytesPerPixel(
            formats,
            presentation.output.snapshot.renderFormat);
        if (!formatSize) {
            scene.captureFailures.push_back({
                .key = key,
                .error = Error{
                    .code = ErrorCode::UnsupportedOperation,
                    .path = "output.render_format",
                    .message = "render format has no exact capture layout",
                },
            });
            continue;
        }
        CaptureRequest request{
            .output = presentation.output,
            .stage = presentation.presentation.key.stage,
            .coverage = presentation.transitionEnvelope
                ? presentation.transitionEnvelope->coverage
                : presentation.presentation.geometry.coverage,
            .apronPixels =
                presentation.sampling.apronPixels,
            .bytesPerPixel = *formatSize,
            .stageObjectToken =
                presentation.presentation.key.stage ==
                        RenderStage::PreWindow
                    ? presentation.target.attachment.objectToken
                    : 0U,
        };
        if (request.stage == RenderStage::PreWindow &&
            presentation.target.attachment.kind !=
                TargetKind::Window) {
            scene.captureFailures.push_back({
                .key = key,
                .error = Error{
                    .code = ErrorCode::UnsupportedOperation,
                    .path = "target.stage",
                    .message = "pre-window capture requires an exact window target",
                },
            });
            continue;
        }
        const std::array oneRequest{request};
        auto required = planCaptures(oneRequest, limits);
        if (!required) {
            scene.captureFailures.push_back({
                .key = key,
                .error = required.error(),
            });
            continue;
        }
        if (required.value().size() != 1U)
            return failure(
                ErrorCode::InternalError,
                "capture",
                "one presentation did not produce one capture requirement");
        acceptedRequests.push_back(std::move(request));
        acceptedAssignments.push_back({
            .presentation = presentation,
            .required = std::move(required.value().front()),
            .captureIndex = 0U,
        });
    }

    auto captures = planCaptures(acceptedRequests, limits);
    if (!captures)
        return Result<CaptureScene>::failure(captures.error());
    scene.captures = std::move(captures.value());
    for (auto& assignment : acceptedAssignments) {
        std::optional<std::size_t> covering;
        for (std::size_t index = 0;
             index < scene.captures.size();
             ++index) {
            if (!capturePlanCovers(
                    scene.captures[index],
                    assignment.required))
                continue;
            if (covering &&
                scene.captures[index].pixelCount >=
                    scene.captures[*covering].pixelCount)
                continue;
            covering = index;
        }
        if (!covering)
            return failure(
                ErrorCode::InternalError,
                "capture",
                "planned capture does not cover its presentation");
        assignment.captureIndex = *covering;
        scene.assignments.push_back(std::move(assignment));
    }
    return Result<CaptureScene>::success(std::move(scene));
}

} // namespace hfg::v2
