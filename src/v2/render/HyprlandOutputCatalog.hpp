#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/OutputGeneration.hpp"

#include <aquamarine/output/Output.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace hfg::v2 {

struct OutputCatalogRefresh {
    std::vector<OutputGeneration> current;
    std::vector<OutputGeneration> retired;
};

class HyprlandOutputCatalog {
  public:
    [[nodiscard]] Result<OutputCatalogRefresh> refresh();
    [[nodiscard]] std::optional<OutputGeneration> current(
        std::string_view name) const;
    [[nodiscard]] Result<PHLMONITOR> monitorFor(
        std::uint64_t objectToken);

    void clear() noexcept;

  private:
    struct ModeState {
        SP<Aquamarine::SOutputMode> mode;
        std::uint32_t                bufferWidth = 0;
        std::uint32_t                bufferHeight = 0;
        std::uint32_t                refreshMilliHz = 0;
        std::uint64_t                token = 0;
    };

    [[nodiscard]] Result<std::uint64_t> objectTokenFor(
        PHLMONITORREF monitor);
    [[nodiscard]] Result<std::uint64_t> modeTokenFor(
        std::uint64_t objectToken,
        const SP<Aquamarine::SOutputMode>& mode,
        std::uint32_t bufferWidth,
        std::uint32_t bufferHeight,
        std::uint32_t refreshMilliHz);
    [[nodiscard]] Result<OutputSnapshot> snapshotFor(
        const PHLMONITOR& monitor);
    void pruneExpiredObjects() noexcept;

    std::map<PHLMONITORREF, std::uint64_t> m_objectTokens;
    std::map<std::uint64_t, ModeState>      m_modes;
    OutputGenerationTracker                 m_generations;
    std::uint64_t                           m_lastObjectToken = 0;
    std::uint64_t                           m_lastModeToken = 0;
};

} // namespace hfg::v2
