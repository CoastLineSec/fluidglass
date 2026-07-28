#include "v2/render/CaptureBlit.hpp"

#include "v2/render/Geometry.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace hfg::v2 {
namespace {

Result<CaptureBlit> invalid(
    std::string path,
    std::string message,
    ErrorCode code = ErrorCode::InvalidRequest) {
    return Result<CaptureBlit>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

std::optional<PixelRect> intersection(
    const PixelRect& left,
    const PixelRect& right) {
    const auto x1 = std::max(left.x, right.x);
    const auto y1 = std::max(left.y, right.y);
    const auto x2 = std::min<std::int64_t>(
        static_cast<std::int64_t>(left.x) + left.width,
        static_cast<std::int64_t>(right.x) + right.width);
    const auto y2 = std::min<std::int64_t>(
        static_cast<std::int64_t>(left.y) + left.height,
        static_cast<std::int64_t>(right.y) + right.height);
    if (x2 <= x1 || y2 <= y1)
        return std::nullopt;
    return PixelRect{
        .x = x1,
        .y = y1,
        .width = static_cast<std::int32_t>(x2 - x1),
        .height = static_cast<std::int32_t>(y2 - y1),
    };
}

/**
 * Builds one blit from the render framebuffer into the capture texture.
 * Source and destination use the same top-down buffer-row convention, so the
 * blit preserves vertical orientation.
 */
CaptureBlit blitForRegion(
    const PixelRect& region,
    const CapturePlan& plan) {
    const auto sourceRight =
        static_cast<std::int32_t>(region.x + region.width);
    const auto sourceBottom =
        static_cast<std::int32_t>(region.y + region.height);
    return {
        .source = {
            .x0 = region.x,
            .y0 = region.y,
            .x1 = sourceRight,
            .y1 = sourceBottom,
        },
        .destination = {
            .x0 = static_cast<std::int32_t>(
                region.x - plan.region.x),
            .y0 = static_cast<std::int32_t>(
                region.y - plan.region.y),
            .x1 = static_cast<std::int32_t>(
                sourceRight - plan.region.x),
            .y1 = static_cast<std::int32_t>(
                sourceBottom - plan.region.y),
        },
    };
}

} // namespace

Result<CaptureBlit> captureBlitFor(
    const CapturePlan& plan,
    const OutputGeneration& output) {
    if (auto validation = validateCapturePlan(plan); !validation)
        return Result<CaptureBlit>::failure(validation.error());
    if (output.generation == 0U)
        return invalid("output.generation", "output generation must not be zero");
    if (auto validation = validateOutputSnapshot(output.snapshot); !validation)
        return Result<CaptureBlit>::failure(validation.error());
    if (plan.key.output != output.snapshot.name ||
        plan.key.outputGeneration != output.generation ||
        plan.key.renderFormat != output.snapshot.renderFormat ||
        plan.key.colorStateToken != output.snapshot.colorStateToken)
        return invalid(
            "plan.key",
            "capture plan is incompatible with the output generation",
            ErrorCode::StaleGeneration);

    const auto right = static_cast<std::int64_t>(plan.region.x) +
        plan.region.width;
    const auto bottom = static_cast<std::int64_t>(plan.region.y) +
        plan.region.height;
    if (right > output.snapshot.bufferWidth ||
        bottom > output.snapshot.bufferHeight)
        return invalid(
            "plan.region",
            "capture region exceeds the output buffer");

    return Result<CaptureBlit>::success(
        blitForRegion(plan.region, plan));
}

Result<std::vector<CaptureBlit>> captureUpdateBlits(
    const CapturePlan& plan,
    const OutputGeneration& output,
    std::span<const PixelRect> outputDamage,
    bool initialized) {
    auto full = captureBlitFor(plan, output);
    if (!full)
        return Result<std::vector<CaptureBlit>>::failure(full.error());
    if (!initialized)
        return Result<std::vector<CaptureBlit>>::success(
            std::vector{full.value()});

    std::vector<CaptureBlit> result;
    result.reserve(outputDamage.size());
    for (std::size_t index = 0; index < outputDamage.size(); ++index) {
        auto bufferDamage = mapOutputPixelRectToBuffer(
            outputDamage[index],
            output.snapshot);
        if (!bufferDamage)
            return Result<std::vector<CaptureBlit>>::failure({
                .code = bufferDamage.error().code,
                .path = "output_damage[" +
                    std::to_string(index) + "]." +
                    bufferDamage.error().path,
                .message = bufferDamage.error().message,
            });
        const auto clipped = intersection(
            bufferDamage.value(),
            plan.region);
        if (!clipped)
            continue;
        result.push_back(blitForRegion(*clipped, plan));
    }
    return Result<std::vector<CaptureBlit>>::success(
        std::move(result));
}

} // namespace hfg::v2
