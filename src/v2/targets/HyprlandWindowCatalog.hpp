#pragma once

#include "v2/core/Result.hpp"
#include "v2/targets/WindowAdapter.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

namespace hfg::v2 {

class HyprlandWindowCatalog {
  public:
    [[nodiscard]] Result<std::vector<WindowSnapshot>>
    allSnapshots();
    [[nodiscard]] Result<std::vector<WindowSnapshot>> snapshots(
        std::string_view address);
    [[nodiscard]] Result<PHLWINDOW> windowFor(
        std::uint64_t objectToken);
    [[nodiscard]] Result<std::uint64_t> objectTokenFor(
        PHLWINDOWREF window);

    void clear() noexcept;

  private:
    std::map<PHLWINDOWREF, std::uint64_t> m_tokens;
    std::uint64_t                         m_lastToken = 0;
};

} // namespace hfg::v2
