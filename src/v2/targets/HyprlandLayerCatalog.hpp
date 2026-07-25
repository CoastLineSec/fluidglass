#pragma once

#include "v2/core/Result.hpp"
#include "v2/targets/LayerAdapter.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

namespace hfg::v2 {

class HyprlandLayerCatalog {
  public:
    [[nodiscard]] Result<std::vector<LayerSurfaceSnapshot>> snapshots(
        std::string_view namespaceName);

    void clear() noexcept;

  private:
    [[nodiscard]] Result<std::uint64_t> tokenFor(PHLLSREF surface);

    std::map<PHLLSREF, std::uint64_t> m_tokens;
    std::uint64_t                     m_lastToken = 0;
};

} // namespace hfg::v2
