#include "v2/targets/HyprlandLayerCatalog.hpp"

#include "v2/core/Limits.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <utility>

namespace hfg::v2 {
namespace {

Result<std::vector<LayerSurfaceSnapshot>> unavailable(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<std::vector<LayerSurfaceSnapshot>>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

bool validNamespace(std::string_view value) {
    return !value.empty() &&
        value.size() <= Limits::MAX_IDENTIFIER_BYTES &&
        std::ranges::none_of(value, [](const unsigned char character) {
            return std::iscntrl(character);
        });
}

std::optional<LayerLevel> layerLevel(std::uint32_t level) {
    switch (level) {
        case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
            return LayerLevel::Background;
        case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
            return LayerLevel::Bottom;
        case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
            return LayerLevel::Top;
        case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
            return LayerLevel::Overlay;
        default:
            return std::nullopt;
    }
}

} // namespace

Result<std::uint64_t> HyprlandLayerCatalog::tokenFor(
    PHLLSREF surface) {
    const auto existing = m_tokens.find(surface);
    if (existing != m_tokens.end())
        return Result<std::uint64_t>::success(existing->second);
    if (m_lastToken == std::numeric_limits<std::uint64_t>::max())
        return Result<std::uint64_t>::failure({
            ErrorCode::ResourceLimited,
            "layer.object_token",
            "layer object token space is exhausted",
        });
    const auto token = ++m_lastToken;
    m_tokens.emplace(std::move(surface), token);
    return Result<std::uint64_t>::success(token);
}

Result<std::vector<LayerSurfaceSnapshot>>
HyprlandLayerCatalog::snapshots(std::string_view namespaceName) {
    if (!validNamespace(namespaceName))
        return unavailable(
            ErrorCode::InvalidRequest,
            "namespace",
            "expected a non-empty bounded layer namespace");
    if (!g_pCompositor)
        return unavailable(
            ErrorCode::UnsupportedOperation,
            "compositor",
            "Hyprland compositor is unavailable");

    std::erase_if(m_tokens, [](const auto& entry) {
        return entry.first.expired();
    });
    std::vector<LayerSurfaceSnapshot> result;
    for (const auto& monitor : g_pCompositor->m_monitors) {
        if (!monitor)
            continue;
        for (const auto& level : monitor->m_layerSurfaceLayers) {
            for (const auto& reference : level) {
                const auto surface = reference.lock();
                if (!surface ||
                    surface->m_namespace != namespaceName)
                    continue;
                const auto mappedLevel = layerLevel(surface->m_layer);
                if (!mappedLevel)
                    return unavailable(
                        ErrorCode::UnsupportedOperation,
                        "layer.level",
                        "Hyprland reported an unsupported layer level");
                auto token = tokenFor(reference);
                if (!token)
                    return Result<std::vector<LayerSurfaceSnapshot>>::failure(
                        token.error());
                const auto position = surface->m_realPosition->value();
                const auto size = surface->m_realSize->value();
                result.push_back({
                    .namespaceName = surface->m_namespace,
                    .objectToken = token.value(),
                    .output = monitor->m_name,
                    .globalGeometry = Rect{
                        .x = position.x,
                        .y = position.y,
                        .width = size.x,
                        .height = size.y,
                    },
                    .level = *mappedLevel,
                    .opacity = std::clamp(
                        static_cast<double>(surface->m_alpha->value()),
                        0.0,
                        1.0),
                    .mapped = surface->m_mapped,
                    .fadingOut = surface->m_fadingOut,
                    .readyToDelete = surface->m_readyToDelete,
                });
                if (result.size() > Limits::MAX_PRESENTATIONS_PER_TARGET)
                    return unavailable(
                        ErrorCode::ResourceLimited,
                        "layers",
                        "matching layer surface count exceeds the supported limit");
            }
        }
    }
    return Result<std::vector<LayerSurfaceSnapshot>>::success(
        std::move(result));
}

void HyprlandLayerCatalog::clear() noexcept {
    m_tokens.clear();
}

} // namespace hfg::v2
