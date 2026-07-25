#include "v2/render/HyprlandCaptureEnvironment.hpp"

#include "v2/core/Limits.hpp"
#include "v2/render/HyprlandCaptureFormat.hpp"

#include <hyprland/src/render/OpenGL.hpp>

#include <GLES3/gl32.h>

#include <algorithm>
#include <set>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

Result<HyprlandCaptureEnvironment> failure(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<HyprlandCaptureEnvironment>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

} // namespace

Result<HyprlandCaptureEnvironment>
inspectHyprlandCaptureEnvironment(
    std::span<const OutputGeneration> outputs,
    const CaptureBudget& budget) {
    if (outputs.size() > Limits::MAX_COMPOSITOR_OBJECTS)
        return failure(
            ErrorCode::ResourceLimited,
            "outputs",
            "output count exceeds the supported limit");
    if (!Render::GL::g_pHyprOpenGL)
        return failure(
            ErrorCode::UnsupportedOperation,
            "renderer",
            "Hyprland OpenGL renderer is unavailable");

    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    GLint maximumTextureSize = 0;
    glGetIntegerv(
        GL_MAX_TEXTURE_SIZE,
        &maximumTextureSize);
    if (maximumTextureSize <= 0)
        return failure(
            ErrorCode::UnsupportedOperation,
            "renderer.maximum_texture_size",
            "OpenGL did not report a usable texture size");

    HyprlandCaptureEnvironment environment;
    std::set<std::string_view> outputNames;
    std::set<std::uint32_t> renderFormats;
    std::uint32_t maximumBytesPerPixel = 1U;
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
            return Result<HyprlandCaptureEnvironment>::failure({
                .code = validation.error().code,
                .path = "outputs[" +
                    std::to_string(index) + "]." +
                    validation.error().path,
                .message = validation.error().message,
            });
        if (!renderFormats.insert(
                output.snapshot.renderFormat).second)
            continue;
        auto layout = hyprlandCaptureFormatLayout(
            output.snapshot.renderFormat);
        if (!layout) {
            environment.formatIssues.push_back({
                .renderFormat =
                    output.snapshot.renderFormat,
                .error = layout.error(),
            });
            continue;
        }
        maximumBytesPerPixel = std::max(
            maximumBytesPerPixel,
            layout.value().bytesPerPixel);
        environment.formats.push_back(
            std::move(layout.value()));
    }

    auto limits = resolveCaptureLimits(
        static_cast<std::uint32_t>(maximumTextureSize),
        maximumBytesPerPixel,
        budget);
    if (!limits)
        return Result<HyprlandCaptureEnvironment>::failure(
            limits.error());
    environment.limits = std::move(limits.value());
    environment.maxTotalBytes = budget.maxTotalBytes;
    return Result<HyprlandCaptureEnvironment>::success(
        std::move(environment));
}

} // namespace hfg::v2
