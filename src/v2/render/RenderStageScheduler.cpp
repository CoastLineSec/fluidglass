#include "v2/render/RenderStageScheduler.hpp"

#include "v2/core/Limits.hpp"

#include <set>
#include <optional>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

Result<std::vector<CaptureResource>> failure(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<std::vector<CaptureResource>>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

std::optional<RenderStage> renderStageFor(
    RenderHookStage hook) {
    switch (hook) {
        case RenderHookStage::PostWallpaper:
            return RenderStage::PostWallpaper;
        case RenderHookStage::PreWindow:
            return RenderStage::PreWindow;
        case RenderHookStage::PostWindows:
            return RenderStage::PostWindows;
        case RenderHookStage::LastMoment:
            return RenderStage::PostLayer;
    }
    return std::nullopt;
}

} // namespace

Result<std::vector<CaptureResource>>
RenderStageScheduler::schedule(
    std::span<const CaptureResource> resources,
    const RenderHookEvent& event) {
    if (auto validation =
            validateOutputSnapshot(event.output.snapshot);
        !validation)
        return failure(
            validation.error().code,
            "event." + validation.error().path,
            validation.error().message);
    if (event.output.generation == 0U)
        return failure(
            ErrorCode::StaleGeneration,
            "event.output.generation",
            "render-hook output generation must not be zero");
    if (event.frameToken == 0U)
        return failure(
            ErrorCode::InvalidRequest,
            "event.frame_token",
            "render-hook frame token must not be zero");
    const auto stage = renderStageFor(event.hook);
    if (!stage)
        return failure(
            ErrorCode::UnsupportedOperation,
            "event.hook",
            "render hook does not map to a supported capture stage");
    if (*stage == RenderStage::PreWindow &&
        event.stageObjectToken == 0U)
        return failure(
            ErrorCode::InvalidRequest,
            "event.stage_object_token",
            "pre-window hook requires an exact window object token");
    if (*stage != RenderStage::PreWindow &&
        event.stageObjectToken != 0U)
        return failure(
            ErrorCode::InvalidRequest,
            "event.stage_object_token",
            "global render hooks must not carry an object token");
    if (resources.size() > Limits::MAX_CAPTURE_REQUESTS)
        return failure(
            ErrorCode::ResourceLimited,
            "resources",
            "capture resource count exceeds the supported limit");

    std::set<std::uint64_t> resourceTokens;
    for (std::size_t index = 0;
         index < resources.size();
         ++index) {
        const auto& resource = resources[index];
        if (resource.token == 0U)
            return failure(
                ErrorCode::InvalidRequest,
                "resources[" + std::to_string(index) +
                    "].token",
                "capture resource token must not be zero");
        if (!resourceTokens.insert(resource.token).second)
            return failure(
                ErrorCode::InvalidRequest,
                "resources[" + std::to_string(index) +
                    "].token",
                "capture resource tokens must be unique");
        if (auto validation =
                validateCapturePlan(resource.plan);
            !validation)
            return failure(
                validation.error().code,
                "resources[" + std::to_string(index) +
                    "]." + validation.error().path,
                validation.error().message);
    }

    auto& frame = m_frames[event.output.snapshot.name];
    if (frame.generation > event.output.generation ||
        (frame.generation == event.output.generation &&
            frame.frameToken > event.frameToken))
        return failure(
            ErrorCode::StaleGeneration,
            "event",
            "render hook belongs to a retired output generation or frame");
    if (frame.generation != event.output.generation ||
        frame.frameToken != event.frameToken) {
        frame = {
            .generation = event.output.generation,
            .frameToken = event.frameToken,
            .capturedTokens = {},
        };
    }

    std::vector<CaptureResource> scheduled;
    for (const auto& resource : resources) {
        const auto& key = resource.plan.key;
        if (key.output != event.output.snapshot.name ||
            key.outputGeneration !=
                event.output.generation ||
            key.stage != *stage ||
            key.stageObjectToken !=
                event.stageObjectToken ||
            frame.capturedTokens.contains(resource.token))
            continue;
        frame.capturedTokens.insert(resource.token);
        scheduled.push_back(resource);
    }
    return Result<std::vector<CaptureResource>>::success(
        std::move(scheduled));
}

void RenderStageScheduler::clearOutput(
    std::string_view output) {
    m_frames.erase(output);
}

void RenderStageScheduler::clear() noexcept {
    m_frames.clear();
}

} // namespace hfg::v2
