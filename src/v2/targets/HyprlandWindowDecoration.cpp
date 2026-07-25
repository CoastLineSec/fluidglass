#include "v2/targets/HyprlandWindowDecoration.hpp"

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace hfg::v2 {

HyprlandWindowDecoration::HyprlandWindowDecoration(
    PHLWINDOW window,
    TargetIdentity identity,
    std::uint64_t objectToken,
    DrawCallback drawCallback,
    FailureCallback failureCallback)
    : IHyprWindowDecoration(window),
      m_window(window),
      m_identity(std::move(identity)),
      m_objectToken(objectToken),
      m_drawCallback(std::move(drawCallback)),
      m_failureCallback(std::move(failureCallback)) {}

const TargetIdentity&
HyprlandWindowDecoration::identity() const noexcept {
    return m_identity;
}

std::uint64_t
HyprlandWindowDecoration::objectToken() const noexcept {
    return m_objectToken;
}

PHLWINDOW HyprlandWindowDecoration::window() const {
    return m_window.lock();
}

SDecorationPositioningInfo
HyprlandWindowDecoration::getPositioningInfo() {
    SDecorationPositioningInfo info;
    info.policy = DECORATION_POSITION_ABSOLUTE;
    info.edges =
        DECORATION_EDGE_TOP |
        DECORATION_EDGE_BOTTOM |
        DECORATION_EDGE_LEFT |
        DECORATION_EDGE_RIGHT;
    info.reserved = false;
    return info;
}

void HyprlandWindowDecoration::onPositioningReply(
    const SDecorationPositioningReply&) {}

void HyprlandWindowDecoration::draw(
    PHLMONITOR monitor,
    float const& opacity) {
    const auto attachedWindow = m_window.lock();
    if (!m_drawCallback ||
        !monitor ||
        !attachedWindow ||
        !attachedWindow->m_isMapped ||
        attachedWindow->m_fadingOut ||
        attachedWindow->m_readyToDelete)
        return;

    try {
        const double effectiveOpacity =
            static_cast<double>(opacity);
        if (!std::isfinite(effectiveOpacity)) {
            reportFailure({
                .code = ErrorCode::InternalError,
                .path = "window-decoration.opacity",
                .message = "Hyprland supplied a non-finite decoration opacity",
            });
            return;
        }
        const auto result = m_drawCallback({
            .identity = m_identity,
            .objectToken = m_objectToken,
            .window = attachedWindow,
            .monitor = std::move(monitor),
            .opacity = std::clamp(
                effectiveOpacity,
                0.0,
                1.0),
        });
        if (!result)
            reportFailure(result.error());
    } catch (const std::exception& error) {
        reportFailure({
            .code = ErrorCode::InternalError,
            .path = "window-decoration.draw",
            .message = error.what(),
        });
    } catch (...) {
        reportFailure({
            .code = ErrorCode::InternalError,
            .path = "window-decoration.draw",
            .message = "non-standard exception crossed the draw boundary",
        });
    }
}

eDecorationType
HyprlandWindowDecoration::getDecorationType() {
    return DECORATION_CUSTOM;
}

void HyprlandWindowDecoration::updateWindow(PHLWINDOW) {}

void HyprlandWindowDecoration::damageEntire() {
    const auto attachedWindow = m_window.lock();
    if (attachedWindow &&
        attachedWindow->m_isMapped &&
        g_pHyprRenderer)
        g_pHyprRenderer->damageWindow(attachedWindow, true);
}

eDecorationLayer
HyprlandWindowDecoration::getDecorationLayer() {
    return DECORATION_LAYER_UNDER;
}

std::uint64_t
HyprlandWindowDecoration::getDecorationFlags() {
    return DECORATION_NON_SOLID;
}

std::string HyprlandWindowDecoration::getDisplayName() {
    return "HyprFluidGlass";
}

void HyprlandWindowDecoration::reportFailure(
    Error error) noexcept {
    if (!m_failureCallback)
        return;
    try {
        m_failureCallback(m_identity, error);
    } catch (...) {
    }
}

} // namespace hfg::v2
