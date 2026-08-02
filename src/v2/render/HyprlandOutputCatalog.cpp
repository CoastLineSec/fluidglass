#include "v2/render/HyprlandOutputCatalog.hpp"

#include "v2/core/Limits.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/helpers/cm/ColorManagement.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <drm_fourcc.h>
#include <wayland-server-protocol.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
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

std::optional<std::uint32_t> exactDimension(double value) {
    if (!std::isfinite(value) ||
        value <= 0.0 ||
        value > static_cast<double>(
                    std::numeric_limits<std::uint32_t>::max()) ||
        std::round(value) != value)
        return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

std::optional<OutputTransform> outputTransform(
    wl_output_transform transform) {
    switch (transform) {
        case WL_OUTPUT_TRANSFORM_NORMAL:
            return OutputTransform::Normal;
        case WL_OUTPUT_TRANSFORM_90:
            return OutputTransform::Rotate90;
        case WL_OUTPUT_TRANSFORM_180:
            return OutputTransform::Rotate180;
        case WL_OUTPUT_TRANSFORM_270:
            return OutputTransform::Rotate270;
        case WL_OUTPUT_TRANSFORM_FLIPPED:
            return OutputTransform::Flipped;
        case WL_OUTPUT_TRANSFORM_FLIPPED_90:
            return OutputTransform::Flipped90;
        case WL_OUTPUT_TRANSFORM_FLIPPED_180:
            return OutputTransform::Flipped180;
        case WL_OUTPUT_TRANSFORM_FLIPPED_270:
            return OutputTransform::Flipped270;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> refreshMilliHz(
    const PHLMONITOR& monitor) {
    if (monitor->m_currentMode)
        return monitor->m_currentMode->refreshRate;
    const auto refresh =
        static_cast<double>(monitor->m_refreshRate) * 1000.0;
    if (!std::isfinite(refresh) ||
        refresh < 0.0 ||
        refresh > static_cast<double>(
                      std::numeric_limits<std::uint32_t>::max()))
        return std::nullopt;
    return static_cast<std::uint32_t>(std::llround(refresh));
}

} // namespace

Result<std::uint64_t> HyprlandOutputCatalog::objectTokenFor(
    PHLMONITORREF monitor) {
    const auto existing = m_objectTokens.find(monitor);
    if (existing != m_objectTokens.end())
        return Result<std::uint64_t>::success(existing->second);
    if (m_lastObjectToken == std::numeric_limits<std::uint64_t>::max())
        return failure<std::uint64_t>(
            ErrorCode::ResourceLimited,
            "output.object_token",
            "output object token space is exhausted");
    const auto token = ++m_lastObjectToken;
    m_objectTokens.emplace(std::move(monitor), token);
    return Result<std::uint64_t>::success(token);
}

Result<std::uint64_t> HyprlandOutputCatalog::modeTokenFor(
    std::uint64_t objectToken,
    const SP<Aquamarine::SOutputMode>& mode,
    std::uint32_t bufferWidth,
    std::uint32_t bufferHeight,
    std::uint32_t refreshMilliHzValue) {
    const auto existing = m_modes.find(objectToken);
    if (existing != m_modes.end() &&
        existing->second.mode == mode &&
        existing->second.bufferWidth == bufferWidth &&
        existing->second.bufferHeight == bufferHeight &&
        existing->second.refreshMilliHz == refreshMilliHzValue)
        return Result<std::uint64_t>::success(existing->second.token);
    if (m_lastModeToken == std::numeric_limits<std::uint64_t>::max())
        return failure<std::uint64_t>(
            ErrorCode::ResourceLimited,
            "output.mode_token",
            "output mode token space is exhausted");

    const auto token = ++m_lastModeToken;
    m_modes.insert_or_assign(
        objectToken,
        ModeState{
            .mode = mode,
            .bufferWidth = bufferWidth,
            .bufferHeight = bufferHeight,
            .refreshMilliHz = refreshMilliHzValue,
            .token = token,
        });
    return Result<std::uint64_t>::success(token);
}

Result<OutputSnapshot> HyprlandOutputCatalog::snapshotFor(
    const PHLMONITOR& monitor) {
    if (!monitor ||
        !monitor->m_enabled ||
        !monitor->m_output ||
        !monitor->m_output->state)
        return failure<OutputSnapshot>(
            ErrorCode::UnresolvedTarget,
            "output",
            "Hyprland output is not active");

    const auto width = exactDimension(monitor->m_pixelSize.x);
    const auto height = exactDimension(monitor->m_pixelSize.y);
    if (!width || !height)
        return failure<OutputSnapshot>(
            ErrorCode::UnsupportedOperation,
            "output.buffer_size",
            "Hyprland reported a non-integral or invalid render-buffer size");
    const auto transform = outputTransform(monitor->m_transform);
    if (!transform)
        return failure<OutputSnapshot>(
            ErrorCode::UnsupportedOperation,
            "output.transform",
            "Hyprland reported an unsupported output transform");
    const auto refresh = refreshMilliHz(monitor);
    if (!refresh)
        return failure<OutputSnapshot>(
            ErrorCode::UnsupportedOperation,
            "output.refresh",
            "Hyprland reported an invalid output refresh rate");

    PHLMONITORREF reference = monitor;
    const auto objectToken = objectTokenFor(reference);
    if (!objectToken)
        return Result<OutputSnapshot>::failure(objectToken.error());
    const auto modeToken = modeTokenFor(
        objectToken.value(),
        monitor->m_currentMode,
        *width,
        *height,
        *refresh);
    if (!modeToken)
        return Result<OutputSnapshot>::failure(modeToken.error());

    const auto imageDescription =
        monitor->workBufferImageDescription();
    if (!imageDescription)
        return failure<OutputSnapshot>(
            ErrorCode::UnsupportedOperation,
            "output.color_state",
            "Hyprland render buffer has no color description");
    const auto outputFormat =
        monitor->m_output->state->state().drmFormat;
    const auto renderFormat = monitor->useFP16()
        ? DRM_FORMAT_ABGR16161616F
        : outputFormat;

    OutputSnapshot snapshot{
        .name = monitor->m_name,
        .objectToken = objectToken.value(),
        .modeToken = modeToken.value(),
        .bufferWidth = *width,
        .bufferHeight = *height,
        .logicalX = monitor->m_position.x,
        .logicalY = monitor->m_position.y,
        .logicalWidth = monitor->m_size.x,
        .logicalHeight = monitor->m_size.y,
        .scale = static_cast<double>(monitor->m_scale),
        .transform = *transform,
        .renderFormat = renderFormat,
        .colorStateToken = imageDescription->id(),
    };
    if (auto validation = validateOutputSnapshot(snapshot); !validation)
        return Result<OutputSnapshot>::failure(validation.error());
    return Result<OutputSnapshot>::success(std::move(snapshot));
}

void HyprlandOutputCatalog::pruneExpiredObjects() noexcept {
    auto entry = m_objectTokens.begin();
    while (entry != m_objectTokens.end()) {
        if (!entry->first.expired()) {
            ++entry;
            continue;
        }
        m_modes.erase(entry->second);
        entry = m_objectTokens.erase(entry);
    }
}

Result<OutputCatalogRefresh> HyprlandOutputCatalog::refresh() {
    if (!g_pCompositor)
        return failure<OutputCatalogRefresh>(
            ErrorCode::UnsupportedOperation,
            "compositor",
            "Hyprland compositor is unavailable");

    pruneExpiredObjects();
    std::vector<OutputSnapshot> snapshots;
    std::set<std::string, std::less<>> seenNames;
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor || !monitor->m_enabled)
            continue;
        auto snapshot = snapshotFor(monitor);
        if (!snapshot)
            // A monitor mid-modeset, mid-hotplug or DPMS-transitioning can
            // fail its snapshot transiently. It is simply not a current
            // output this refresh — failing the whole catalog here used to
            // tear down every output's glass for one monitor's flux.
            continue;
        if (!seenNames.insert(snapshot.value().name).second)
            return failure<OutputCatalogRefresh>(
                ErrorCode::InternalError,
                "outputs",
                "Hyprland reported duplicate active output names");
        snapshots.push_back(std::move(snapshot.value()));
        if (snapshots.size() > Limits::MAX_COMPOSITOR_OBJECTS)
            return failure<OutputCatalogRefresh>(
                ErrorCode::ResourceLimited,
                "outputs",
                "active output count exceeds the supported limit");
    }

    auto candidate = m_generations;
    std::vector<OutputGeneration> retired;
    for (auto& snapshot : snapshots) {
        auto update = candidate.update(std::move(snapshot));
        if (!update)
            return Result<OutputCatalogRefresh>::failure(
                update.error());
        if (update.value().retired)
            retired.push_back(*update.value().retired);
    }
    for (const auto& generation : candidate.currents()) {
        if (seenNames.contains(generation.snapshot.name))
            continue;
        auto removed = candidate.remove(generation.snapshot.name);
        if (removed)
            retired.push_back(std::move(*removed));
    }

    m_generations = std::move(candidate);
    return Result<OutputCatalogRefresh>::success({
        .current = m_generations.currents(),
        .retired = std::move(retired),
    });
}

std::optional<OutputGeneration> HyprlandOutputCatalog::current(
    std::string_view name) const {
    return m_generations.current(name);
}

Result<PHLMONITOR> HyprlandOutputCatalog::monitorFor(
    std::uint64_t objectToken) {
    if (objectToken == 0U)
        return failure<PHLMONITOR>(
            ErrorCode::InvalidRequest,
            "object_token",
            "output object token must not be zero");

    pruneExpiredObjects();
    for (const auto& [reference, token] : m_objectTokens) {
        if (token != objectToken)
            continue;
        const auto monitor = reference.lock();
        if (monitor && monitor->m_enabled)
            return Result<PHLMONITOR>::success(monitor);
    }
    return failure<PHLMONITOR>(
        ErrorCode::UnresolvedTarget,
        "object_token",
        "output object token is no longer active");
}

void HyprlandOutputCatalog::clear() noexcept {
    m_generations.clearCurrent();
    m_objectTokens.clear();
    m_modes.clear();
}

} // namespace hfg::v2
