#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/CaptureCache.hpp"
#include "v2/render/CaptureScene.hpp"
#include "v2/render/GlassDrawPlan.hpp"

#include <span>
#include <vector>

namespace hfg::v2 {

struct GlassDrawFailure {
  PresentationKey key;
  Error error;

  friend bool operator==(const GlassDrawFailure &,
                         const GlassDrawFailure &) = default;
};

struct GlassRenderScene {
  std::vector<CaptureResource> resources;
  std::vector<GlassDrawPlan> draws;
  std::vector<PresentationHandoffPair> handoffs = {};
  std::vector<InactiveTarget> inactive;
  std::vector<TargetIdentity> suppressed;
  std::vector<TargetResolutionFailure> targetFailures;
  std::vector<PresentationCaptureFailure> captureFailures;
  std::vector<GlassDrawFailure> drawFailures;

  friend bool operator==(const GlassRenderScene &,
                         const GlassRenderScene &) = default;
};

[[nodiscard]] Result<GlassRenderScene>
buildGlassRenderScene(const CaptureScene &captures,
                      std::span<const CaptureResource> resources);

} // namespace hfg::v2
