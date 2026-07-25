#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Readiness.hpp"

#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>

#include <cstdint>
#include <functional>
#include <string_view>

namespace hfg::v2 {

struct WindowDecorationDrawContext {
    TargetIdentity identity;
    std::uint64_t  objectToken = 0;
    PHLWINDOW      window;
    PHLMONITOR     monitor;
    double         opacity = 1.0;
};

class HyprlandWindowDecoration final : public IHyprWindowDecoration {
  public:
    using DrawCallback =
        std::function<Result<void>(const WindowDecorationDrawContext&)>;
    using FailureCallback =
        std::function<void(const TargetIdentity&, const Error&)>;

    HyprlandWindowDecoration(
        PHLWINDOW window,
        TargetIdentity identity,
        std::uint64_t objectToken,
        DrawCallback drawCallback,
        FailureCallback failureCallback = {});

    [[nodiscard]] const TargetIdentity& identity() const noexcept;
    [[nodiscard]] std::uint64_t objectToken() const noexcept;
    [[nodiscard]] PHLWINDOW window() const;

    SDecorationPositioningInfo getPositioningInfo() override;
    void onPositioningReply(
        const SDecorationPositioningReply& reply) override;
    void draw(PHLMONITOR monitor, float const& opacity) override;
    eDecorationType getDecorationType() override;
    void updateWindow(PHLWINDOW window) override;
    void damageEntire() override;
    eDecorationLayer getDecorationLayer() override;
    std::uint64_t getDecorationFlags() override;
    std::string getDisplayName() override;

  private:
    void reportFailure(Error error) noexcept;

    PHLWINDOWREF     m_window;
    TargetIdentity   m_identity;
    std::uint64_t    m_objectToken = 0;
    DrawCallback     m_drawCallback;
    FailureCallback  m_failureCallback;
};

} // namespace hfg::v2
