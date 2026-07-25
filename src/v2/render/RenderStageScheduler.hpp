#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/CaptureCache.hpp"
#include "v2/render/OutputGeneration.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hfg::v2 {

enum class RenderHookStage {
    PostWallpaper,
    PreWindow,
    PostWindows,
    LastMoment,
};

struct RenderHookEvent {
    OutputGeneration output;
    RenderHookStage   hook = RenderHookStage::PostWindows;
    std::uint64_t     frameToken = 0;
    std::uint64_t     stageObjectToken = 0;

    friend bool operator==(
        const RenderHookEvent&,
        const RenderHookEvent&) = default;
};

class RenderStageScheduler {
  public:
    [[nodiscard]] Result<std::vector<CaptureResource>>
    schedule(
        std::span<const CaptureResource> resources,
        const RenderHookEvent& event);

    void clearOutput(std::string_view output);
    void clear() noexcept;

  private:
    struct FrameState {
        std::uint64_t generation = 0;
        std::uint64_t frameToken = 0;
        std::set<std::uint64_t> capturedTokens;
    };

    std::map<std::string, FrameState, std::less<>> m_frames;
};

} // namespace hfg::v2
