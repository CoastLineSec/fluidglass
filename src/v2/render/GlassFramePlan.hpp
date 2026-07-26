#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/GlassRenderScene.hpp"
#include "v2/render/RenderStageScheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hfg::v2 {

struct GlassFramePlan {
  std::vector<std::uint64_t> captureTokens;
  std::vector<std::size_t> drawIndices;
  std::vector<PixelRect> renderDamage;
  std::vector<Rect> continuationDamage;
  bool blockDirectScanout = false;

  friend bool operator==(const GlassFramePlan &,
                         const GlassFramePlan &) = default;
};

[[nodiscard]] Result<GlassFramePlan>
planGlassFrame(const GlassRenderScene &scene, const RenderHookEvent &event,
               std::span<const PixelRect> frameDamage);

} // namespace hfg::v2
