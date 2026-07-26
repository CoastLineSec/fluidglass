#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/HyprlandOutputCatalog.hpp"
#include "v2/render/RenderStageScheduler.hpp"
#include "v2/targets/HyprlandWindowCatalog.hpp"

#include <hyprland/src/SharedDefs.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace hfg::v2 {

class HyprlandRenderStageAdapter {
  public:
    HyprlandRenderStageAdapter(
        HyprlandOutputCatalog& outputs,
        HyprlandWindowCatalog& windows);

    [[nodiscard]] Result<std::optional<RenderHookEvent>>
    observe(eRenderStage stage);

    void clear() noexcept;

  private:
    struct ActiveFrame {
        std::uint64_t outputGeneration = 0;
        std::uint64_t frameToken = 0;
    };

    [[nodiscard]] Result<OutputGeneration>
    currentOutput() const;

    HyprlandOutputCatalog&                    m_outputs;
    HyprlandWindowCatalog&                    m_windows;
    std::map<std::string, ActiveFrame, std::less<>> m_activeFrames;
    std::map<std::string, std::uint64_t, std::less<>> m_lastFrameTokens;
};

} // namespace hfg::v2
