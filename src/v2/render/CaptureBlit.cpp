#include "v2/render/CaptureBlit.hpp"

#include <cstdint>
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

    const auto sourceBottom = static_cast<std::int64_t>(
        output.snapshot.bufferHeight) - bottom;
    const auto sourceTop = static_cast<std::int64_t>(
        output.snapshot.bufferHeight) - plan.region.y;
    return Result<CaptureBlit>::success({
        .source = BlitRect{
            .x0 = plan.region.x,
            .y0 = static_cast<std::int32_t>(sourceBottom),
            .x1 = static_cast<std::int32_t>(right),
            .y1 = static_cast<std::int32_t>(sourceTop),
        },
        .destination = BlitRect{
            .x0 = 0,
            .y0 = 0,
            .x1 = plan.region.width,
            .y1 = plan.region.height,
        },
    });
}

} // namespace hfg::v2
