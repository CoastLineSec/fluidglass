#include "v2/targets/HyprlandWindowAttachmentManager.hpp"

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>
#include <utility>

namespace hfg::v2 {
namespace {

Result<void> failure(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<void>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

} // namespace

HyprlandWindowAttachmentManager::
HyprlandWindowAttachmentManager(
    HANDLE pluginHandle,
    HyprlandWindowCatalog& catalog,
    HyprlandWindowDecoration::DrawCallback drawCallback,
    HyprlandWindowDecoration::FailureCallback failureCallback)
    : m_pluginHandle(pluginHandle),
      m_catalog(catalog),
      m_drawCallback(std::move(drawCallback)),
      m_failureCallback(std::move(failureCallback)) {}

Result<void> HyprlandWindowAttachmentManager::reconcile(
    std::span<const WindowAttachmentState> desired) {
    if (!m_pluginHandle)
        return failure(
            ErrorCode::UnsupportedOperation,
            "plugin",
            "Hyprland plugin handle is unavailable");

    pruneExpired();
    const auto current = attached();
    auto plan = planWindowAttachments(current, desired);
    if (!plan)
        return Result<void>::failure(plan.error());

    std::vector<Entry> provisional;
    provisional.reserve(plan.value().add.size());
    for (const auto& state : plan.value().add) {
        auto window = m_catalog.windowFor(state.objectToken);
        if (!window) {
            rollback(provisional);
            return Result<void>::failure(window.error());
        }
        if (!Desktop::View::validMapped(window.value())) {
            rollback(provisional);
            return failure(
                ErrorCode::UnresolvedTarget,
                "window",
                "window became unavailable before decoration attachment");
        }

        auto decoration = makeUnique<HyprlandWindowDecoration>(
            window.value(),
            state.identity,
            state.objectToken,
            m_drawCallback,
            m_failureCallback);
        auto* rawDecoration = decoration.get();
        if (!HyprlandAPI::addWindowDecoration(
                m_pluginHandle,
                window.value(),
                std::move(decoration))) {
            rollback(provisional);
            return failure(
                ErrorCode::InternalError,
                "window-decoration",
                "Hyprland rejected the window decoration");
        }
        provisional.push_back({
            .state = state,
            .window = window.value(),
            .decoration = rawDecoration,
        });
    }

    for (const auto& state : plan.value().remove) {
        const auto existing = std::ranges::find_if(
            m_entries,
            [&](const Entry& entry) {
                return entry.state == state;
            });
        if (existing == m_entries.end())
            continue;
        const auto detached = detach(*existing);
        if (!detached) {
            rollback(provisional);
            return detached;
        }
        m_entries.erase(existing);
    }
    for (auto& entry : provisional)
        m_entries.push_back(std::move(entry));
    provisional.clear();

    reconcileBlurSuppression();
    return Result<void>::success();
}

Result<void> HyprlandWindowAttachmentManager::clear() {
    std::optional<Error> firstError;
    for (auto entry = m_entries.begin();
         entry != m_entries.end();) {
        const auto detached = detach(*entry);
        if (detached) {
            entry = m_entries.erase(entry);
            continue;
        }
        if (!firstError)
            firstError = detached.error();
        ++entry;
    }
    reconcileBlurSuppression();
    if (firstError)
        return Result<void>::failure(std::move(*firstError));
    return Result<void>::success();
}

std::vector<WindowAttachmentState>
HyprlandWindowAttachmentManager::attached() const {
    std::vector<WindowAttachmentState> result;
    result.reserve(m_entries.size());
    for (const auto& entry : m_entries) {
        if (!entry.window.expired())
            result.push_back(entry.state);
    }
    return result;
}

Result<void> HyprlandWindowAttachmentManager::detach(
    const Entry& entry) {
    if (!entry.decoration ||
        entry.window.expired())
        return Result<void>::success();
    if (HyprlandAPI::removeWindowDecoration(
            m_pluginHandle,
            entry.decoration))
        return Result<void>::success();
    if (!decorationExists(entry))
        return Result<void>::success();
    return failure(
        ErrorCode::InternalError,
        "window-decoration",
        "Hyprland did not remove the window decoration");
}

bool HyprlandWindowAttachmentManager::decorationExists(
    const Entry& entry) const {
    const auto window = entry.window.lock();
    if (!window || !entry.decoration)
        return false;
    return std::ranges::any_of(
        window->m_windowDecorations,
        [&](const auto& decoration) {
            return decoration.get() == entry.decoration;
        });
}

void HyprlandWindowAttachmentManager::rollback(
    std::vector<Entry>& provisional) {
    for (auto& entry : provisional) {
        const auto detached = detach(entry);
        if (!detached)
            m_entries.push_back(std::move(entry));
    }
    provisional.clear();
    reconcileBlurSuppression();
}

void HyprlandWindowAttachmentManager::pruneExpired() {
    std::erase_if(m_entries, [](const auto& entry) {
        return entry.window.expired();
    });
}

void HyprlandWindowAttachmentManager::reconcileBlurSuppression() {
    const auto plan =
        planWindowBlurSuppression(m_blurSuppressed, attached());

    for (const auto token : plan.release) {
        // A window that has already gone needs no release, and locking one
        // mid-destruction is undefined. Dropping the token is the cleanup.
        const auto window = m_catalog.windowFor(token);
        if (window && window.value()) {
            window.value()->m_ruleApplicator->noBlur().unset(
                Desktop::Types::PRIORITY_SET_PROP);
            if (g_pHyprRenderer)
                g_pHyprRenderer->damageWindow(window.value());
        }
        std::erase(m_blurSuppressed, token);
    }

    for (const auto token : plan.claim) {
        const auto window = m_catalog.windowFor(token);
        if (!window || !window.value())
            continue;
        // Highest priority so a user rule cannot defeat it, and reversible so
        // releasing restores whatever the user configured.
        window.value()->m_ruleApplicator->noBlur().set(
            true, Desktop::Types::PRIORITY_SET_PROP);
        if (g_pHyprRenderer)
            g_pHyprRenderer->damageWindow(window.value());
        m_blurSuppressed.push_back(token);
    }
}

} // namespace hfg::v2
