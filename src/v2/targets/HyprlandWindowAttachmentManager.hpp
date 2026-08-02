#pragma once

#include "v2/core/Result.hpp"
#include "v2/targets/HyprlandWindowCatalog.hpp"
#include "v2/targets/HyprlandWindowDecoration.hpp"
#include "v2/targets/WindowAttachmentPlan.hpp"
#include "v2/targets/WindowBlurSuppressionPlan.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hfg::v2 {

class HyprlandWindowAttachmentManager {
  public:
    HyprlandWindowAttachmentManager(
        HANDLE pluginHandle,
        HyprlandWindowCatalog& catalog,
        HyprlandWindowDecoration::DrawCallback drawCallback,
        HyprlandWindowDecoration::FailureCallback failureCallback = {});

    [[nodiscard]] Result<void> reconcile(
        std::span<const WindowAttachmentState> desired);
    [[nodiscard]] Result<void> clear();
    [[nodiscard]] std::vector<WindowAttachmentState>
    attached() const;

  private:
    struct Entry {
        WindowAttachmentState    state;
        PHLWINDOWREF             window;
        IHyprWindowDecoration*   decoration = nullptr;
    };

    [[nodiscard]] Result<void> detach(const Entry& entry);
    [[nodiscard]] bool decorationExists(
        const Entry& entry) const;
    void rollback(std::vector<Entry>& provisional);
    void pruneExpired();

    /**
     * Brings Hyprland's own blur suppression in line with the attachments.
     *
     * Called after every change to `m_entries`. Suppression is derived from the
     * attachments rather than tracked beside them, so a missed release is not
     * possible: a window is claimed exactly while a decoration is attached.
     */
    void reconcileBlurSuppression();

    HANDLE                                      m_pluginHandle = nullptr;
    HyprlandWindowCatalog&                      m_catalog;
    HyprlandWindowDecoration::DrawCallback      m_drawCallback;
    HyprlandWindowDecoration::FailureCallback   m_failureCallback;
    std::vector<Entry>                           m_entries;
    /** Object tokens whose windows currently carry our blur suppression. */
    std::vector<std::uint64_t>                   m_blurSuppressed;
};

} // namespace hfg::v2
