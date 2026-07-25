#pragma once

#include "v2/core/Result.hpp"

#include <hyprland/src/render/Shader.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace hfg::v2 {

class HyprlandStateGuard {
  public:
    [[nodiscard]] static Result<std::unique_ptr<HyprlandStateGuard>> capture(
        std::span<const SP<CShader>> additionalTrackedShaders,
        std::span<const std::uint32_t> textureUnits);

    ~HyprlandStateGuard();

    HyprlandStateGuard(const HyprlandStateGuard&) = delete;
    HyprlandStateGuard& operator=(const HyprlandStateGuard&) = delete;
    HyprlandStateGuard(HyprlandStateGuard&&) = delete;
    HyprlandStateGuard& operator=(HyprlandStateGuard&&) = delete;

    [[nodiscard]] Result<void> restore();

  private:
    struct Snapshot;

    explicit HyprlandStateGuard(std::unique_ptr<Snapshot> snapshot);

    std::unique_ptr<Snapshot> m_snapshot;
    bool                      m_restored = false;
};

} // namespace hfg::v2
