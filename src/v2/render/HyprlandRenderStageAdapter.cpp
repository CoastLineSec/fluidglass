#include "v2/render/HyprlandRenderStageAdapter.hpp"

#include <hyprland/src/render/Renderer.hpp>

#include <limits>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

template <typename T>
Result<T> failure(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<T>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

std::optional<RenderHookStage> hookStage(
    eRenderStage stage) {
    switch (stage) {
        case RENDER_POST_WALLPAPER:
            return RenderHookStage::PostWallpaper;
        case RENDER_PRE_WINDOW:
            return RenderHookStage::PreWindow;
        case RENDER_POST_WINDOWS:
            return RenderHookStage::PostWindows;
        case RENDER_LAST_MOMENT:
            return RenderHookStage::LastMoment;
        default:
            return std::nullopt;
    }
}

} // namespace

HyprlandRenderStageAdapter::HyprlandRenderStageAdapter(
    HyprlandOutputCatalog& outputs,
    HyprlandWindowCatalog& windows)
    : m_outputs(outputs),
      m_windows(windows) {}

Result<OutputGeneration>
HyprlandRenderStageAdapter::currentOutput() const {
    if (!g_pHyprRenderer)
        return failure<OutputGeneration>(
            ErrorCode::UnsupportedOperation,
            "renderer",
            "Hyprland renderer is unavailable");
    const auto monitor =
        g_pHyprRenderer->renderData().pMonitor.lock();
    if (!monitor)
        return failure<OutputGeneration>(
            ErrorCode::UnresolvedTarget,
            "renderer.output",
            "render hook has no current output");
    const auto output = m_outputs.current(monitor->m_name);
    if (!output)
        return failure<OutputGeneration>(
            ErrorCode::StaleGeneration,
            "renderer.output",
            "current render output is absent from the output catalog");
    return Result<OutputGeneration>::success(*output);
}

Result<std::optional<RenderHookEvent>>
HyprlandRenderStageAdapter::observe(
    eRenderStage stage) {
    if (stage != RENDER_BEGIN && !hookStage(stage))
        return Result<std::optional<RenderHookEvent>>::success(
            std::nullopt);

    auto output = currentOutput();
    if (!output)
        return Result<std::optional<RenderHookEvent>>::failure(
            output.error());
    const auto& outputName = output.value().snapshot.name;

    if (stage == RENDER_BEGIN) {
        auto& lastToken = m_lastFrameTokens[outputName];
        if (lastToken ==
            std::numeric_limits<std::uint64_t>::max())
            return failure<std::optional<RenderHookEvent>>(
                ErrorCode::ResourceLimited,
                "frame_token",
                "render frame token space is exhausted");
        m_activeFrames.insert_or_assign(
            outputName,
            ActiveFrame{
                .outputGeneration =
                    output.value().generation,
                .frameToken = ++lastToken,
            });
        return Result<std::optional<RenderHookEvent>>::success(
            std::nullopt);
    }

    const auto active = m_activeFrames.find(outputName);
    if (active == m_activeFrames.end() ||
        active->second.outputGeneration !=
            output.value().generation)
        return failure<std::optional<RenderHookEvent>>(
            ErrorCode::StaleGeneration,
            "frame",
            "render stage has no current frame for this output generation");

    std::uint64_t stageObjectToken = 0;
    const auto hook = *hookStage(stage);
    if (hook == RenderHookStage::PreWindow) {
        const auto window =
            g_pHyprRenderer->renderData().currentWindow;
        if (window.expired())
            return failure<std::optional<RenderHookEvent>>(
                ErrorCode::UnresolvedTarget,
                "renderer.window",
                "pre-window hook has no current window");
        auto token = m_windows.objectTokenFor(window);
        if (!token)
            return Result<std::optional<RenderHookEvent>>::failure(
                token.error());
        stageObjectToken = token.value();
    }

    return Result<std::optional<RenderHookEvent>>::success(
        RenderHookEvent{
            .output = std::move(output.value()),
            .hook = hook,
            .frameToken = active->second.frameToken,
            .stageObjectToken = stageObjectToken,
        });
}

void HyprlandRenderStageAdapter::clear() noexcept {
    m_activeFrames.clear();
}

} // namespace hfg::v2
