#include "v2/render/CapturePlan.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

Result<std::vector<CapturePlan>> invalid(
    std::string path,
    std::string message,
    ErrorCode code = ErrorCode::InvalidRequest) {
    return Result<std::vector<CapturePlan>>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

bool validStage(RenderStage stage) {
    switch (stage) {
        case RenderStage::PostWallpaper:
        case RenderStage::PreWindow:
        case RenderStage::PostWindows:
        case RenderStage::PostLayer:
            return true;
    }
    return false;
}

bool validOutputName(std::string_view output) {
    return !output.empty() &&
        output.size() <= Limits::MAX_IDENTIFIER_BYTES &&
        std::ranges::none_of(output, [](const unsigned char character) {
            return std::iscntrl(character);
        });
}

Result<void> validateLimits(const CaptureLimits& limits) {
    if (limits.maxWidth == 0U ||
        limits.maxWidth > Limits::MAX_OUTPUT_BUFFER_DIMENSION)
        return Result<void>::failure({
            ErrorCode::ResourceLimited,
            "limits.max_width",
            "capture width limit is outside the supported range",
        });
    if (limits.maxHeight == 0U ||
        limits.maxHeight > Limits::MAX_OUTPUT_BUFFER_DIMENSION)
        return Result<void>::failure({
            ErrorCode::ResourceLimited,
            "limits.max_height",
            "capture height limit is outside the supported range",
        });
    if (limits.maxApronPixels > Limits::MAX_OUTPUT_BUFFER_DIMENSION)
        return Result<void>::failure({
            ErrorCode::ResourceLimited,
            "limits.max_apron_pixels",
            "capture apron limit is outside the supported range",
        });
    if (limits.maxBytesPerPixel == 0U || limits.maxBytesPerPixel > 64U)
        return Result<void>::failure({
            ErrorCode::ResourceLimited,
            "limits.max_bytes_per_pixel",
            "capture bytes-per-pixel limit is outside the supported range",
        });
    if (limits.maxPixels == 0U)
        return Result<void>::failure({
            ErrorCode::ResourceLimited,
            "limits.max_pixels",
            "capture pixel limit must not be zero",
        });
    if (limits.maxBytes == 0U)
        return Result<void>::failure({
            ErrorCode::ResourceLimited,
            "limits.max_bytes",
            "capture byte limit must not be zero",
        });
    return Result<void>::success();
}

std::optional<CapturePlan> makePlan(
    CaptureKey key,
    PixelRect region,
    std::uint32_t bytesPerPixel,
    const CaptureLimits& limits) {
    if (region.width <= 0 || region.height <= 0)
        return std::nullopt;
    const auto width = static_cast<std::uint64_t>(region.width);
    const auto height = static_cast<std::uint64_t>(region.height);
    if (width > limits.maxWidth || height > limits.maxHeight)
        return std::nullopt;
    if (width > std::numeric_limits<std::uint64_t>::max() / height)
        return std::nullopt;
    const auto pixels = width * height;
    if (pixels > limits.maxPixels ||
        pixels > std::numeric_limits<std::uint64_t>::max() / bytesPerPixel)
        return std::nullopt;
    const auto bytes = pixels * bytesPerPixel;
    if (bytes > limits.maxBytes)
        return std::nullopt;
    return CapturePlan{
        .key = std::move(key),
        .region = region,
        .bytesPerPixel = bytesPerPixel,
        .pixelCount = pixels,
        .byteCount = bytes,
    };
}

PixelRect unionRect(const PixelRect& left, const PixelRect& right) {
    const auto x1 = std::min<std::int64_t>(left.x, right.x);
    const auto y1 = std::min<std::int64_t>(left.y, right.y);
    const auto x2 = std::max<std::int64_t>(
        static_cast<std::int64_t>(left.x) + left.width,
        static_cast<std::int64_t>(right.x) + right.width);
    const auto y2 = std::max<std::int64_t>(
        static_cast<std::int64_t>(left.y) + left.height,
        static_cast<std::int64_t>(right.y) + right.height);
    return {
        .x = static_cast<std::int32_t>(x1),
        .y = static_cast<std::int32_t>(y1),
        .width = static_cast<std::int32_t>(x2 - x1),
        .height = static_cast<std::int32_t>(y2 - y1),
    };
}

} // namespace

Result<void> validateCapturePlan(const CapturePlan& plan) {
    if (!validOutputName(plan.key.output))
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "plan.key.output",
            "expected a non-empty bounded output name",
        });
    if (plan.key.outputGeneration == 0U)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "plan.key.output_generation",
            "output generation must not be zero",
        });
    if (!validStage(plan.key.stage))
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "plan.key.stage",
            "unsupported capture stage",
        });
    if (plan.key.stage == RenderStage::PreWindow &&
        plan.key.stageObjectToken == 0U)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "plan.key.stage_object_token",
            "pre-window capture requires an exact window object token",
        });
    if (plan.key.stage != RenderStage::PreWindow &&
        plan.key.stageObjectToken != 0U)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "plan.key.stage_object_token",
            "global render stages must not carry an object token",
        });
    if (plan.key.renderFormat == 0U)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "plan.key.render_format",
            "render format must not be zero",
        });
    if (plan.region.x < 0 || plan.region.y < 0 ||
        plan.region.width <= 0 || plan.region.height <= 0)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "plan.region",
            "expected a non-empty pixel rectangle",
        });
    if (plan.bytesPerPixel == 0U || plan.bytesPerPixel > 64U)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "plan.bytes_per_pixel",
            "capture format size is outside the supported range",
        });

    const auto width = static_cast<std::uint64_t>(plan.region.width);
    const auto height = static_cast<std::uint64_t>(plan.region.height);
    if (width > std::numeric_limits<std::uint64_t>::max() / height)
        return Result<void>::failure({
            ErrorCode::ResourceLimited,
            "plan.pixel_count",
            "capture pixel count overflows",
        });
    const auto pixels = width * height;
    if (pixels != plan.pixelCount ||
        pixels > std::numeric_limits<std::uint64_t>::max() / plan.bytesPerPixel)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "plan.pixel_count",
            "capture pixel count does not match its region",
        });
    if (pixels * plan.bytesPerPixel != plan.byteCount)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "plan.byte_count",
            "capture byte count does not match its format and region",
        });
    return Result<void>::success();
}

Result<std::vector<CapturePlan>> planCaptures(
    std::span<const CaptureRequest> requests,
    const CaptureLimits& limits) {
    if (auto validation = validateLimits(limits); !validation)
        return Result<std::vector<CapturePlan>>::failure(validation.error());
    if (requests.size() > Limits::MAX_CAPTURE_REQUESTS)
        return invalid(
            "requests",
            "capture request count exceeds the supported limit",
            ErrorCode::ResourceLimited);

    std::vector<CapturePlan> plans;
    plans.reserve(requests.size());
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto& request = requests[index];
        const auto path = "requests[" + std::to_string(index) + "]";
        if (request.output.generation == 0U)
            return invalid(path + ".output_generation", "output generation must not be zero");
        if (auto validation = validateOutputSnapshot(request.output.snapshot); !validation)
            return Result<std::vector<CapturePlan>>::failure({
                .code = validation.error().code,
                .path = path + "." + validation.error().path,
                .message = validation.error().message,
            });
        if (!validStage(request.stage))
            return invalid(path + ".stage", "unsupported capture stage");
        if (request.stage == RenderStage::PreWindow &&
            request.stageObjectToken == 0U)
            return invalid(
                path + ".stage_object_token",
                "pre-window capture requires an exact window object token");
        if (request.stage != RenderStage::PreWindow &&
            request.stageObjectToken != 0U)
            return invalid(
                path + ".stage_object_token",
                "global render stages must not carry an object token");
        if (request.coverage.x < 0 || request.coverage.y < 0 ||
            request.coverage.width <= 0 || request.coverage.height <= 0)
            return invalid(path + ".coverage", "expected a non-empty pixel rectangle");

        const auto right = static_cast<std::int64_t>(request.coverage.x) +
            request.coverage.width;
        const auto bottom = static_cast<std::int64_t>(request.coverage.y) +
            request.coverage.height;
        if (right > request.output.snapshot.bufferWidth ||
            bottom > request.output.snapshot.bufferHeight)
            return invalid(path + ".coverage", "pixel rectangle exceeds the output buffer");
        if (request.apronPixels > limits.maxApronPixels)
            return invalid(
                path + ".apron_pixels",
                "capture apron exceeds the adapter limit",
                ErrorCode::ResourceLimited);
        if (request.bytesPerPixel == 0U ||
            request.bytesPerPixel > limits.maxBytesPerPixel)
            return invalid(
                path + ".bytes_per_pixel",
                "capture format size exceeds the adapter limit",
                ErrorCode::ResourceLimited);

        const auto apron = static_cast<std::int64_t>(request.apronPixels);
        const auto expandedLeft = std::max<std::int64_t>(0, request.coverage.x - apron);
        const auto expandedTop = std::max<std::int64_t>(0, request.coverage.y - apron);
        const auto expandedRight = std::min<std::int64_t>(
            request.output.snapshot.bufferWidth,
            right + apron);
        const auto expandedBottom = std::min<std::int64_t>(
            request.output.snapshot.bufferHeight,
            bottom + apron);
        const PixelRect expanded{
            .x = static_cast<std::int32_t>(expandedLeft),
            .y = static_cast<std::int32_t>(expandedTop),
            .width = static_cast<std::int32_t>(expandedRight - expandedLeft),
            .height = static_cast<std::int32_t>(expandedBottom - expandedTop),
        };
        const CaptureKey key{
            .output = request.output.snapshot.name,
            .outputGeneration = request.output.generation,
            .stage = request.stage,
            .renderFormat = request.output.snapshot.renderFormat,
            .colorStateToken = request.output.snapshot.colorStateToken,
            .stageObjectToken = request.stageObjectToken,
        };
        auto required = makePlan(key, expanded, request.bytesPerPixel, limits);
        if (!required)
            return invalid(
                path + ".coverage",
                "capture allocation exceeds the adapter resource limits",
                ErrorCode::ResourceLimited);

        std::optional<std::size_t> bestPlan;
        std::optional<CapturePlan> bestMerged;
        std::uint64_t bestGrowth = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t planIndex = 0; planIndex < plans.size(); ++planIndex) {
            const auto& plan = plans[planIndex];
            if (plan.key != required->key ||
                plan.bytesPerPixel != required->bytesPerPixel)
                continue;
            const auto region = unionRect(plan.region, required->region);
            auto merged = makePlan(plan.key, region, plan.bytesPerPixel, limits);
            if (!merged)
                continue;
            const auto growth = merged->pixelCount - plan.pixelCount;
            if (!bestPlan || growth < bestGrowth) {
                bestPlan = planIndex;
                bestMerged = std::move(merged);
                bestGrowth = growth;
            }
        }

        if (bestPlan)
            plans[*bestPlan] = std::move(*bestMerged);
        else
            plans.emplace_back(std::move(*required));
    }

    return Result<std::vector<CapturePlan>>::success(std::move(plans));
}

bool capturePlanCovers(
    const CapturePlan& available,
    const CapturePlan& required) noexcept {
    if (available.key != required.key ||
        available.bytesPerPixel != required.bytesPerPixel)
        return false;
    const auto availableRight = static_cast<std::int64_t>(available.region.x) +
        available.region.width;
    const auto availableBottom = static_cast<std::int64_t>(available.region.y) +
        available.region.height;
    const auto requiredRight = static_cast<std::int64_t>(required.region.x) +
        required.region.width;
    const auto requiredBottom = static_cast<std::int64_t>(required.region.y) +
        required.region.height;
    return available.region.x <= required.region.x &&
        available.region.y <= required.region.y &&
        availableRight >= requiredRight &&
        availableBottom >= requiredBottom;
}

} // namespace hfg::v2
