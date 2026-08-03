#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/CaptureScene.hpp"
#include "v2/render/GlassFramePlan.hpp"
#include "v2/render/GlassRenderScene.hpp"
#include "v2/render/HyprlandCaptureResourceManager.hpp"
#include "v2/render/RenderStageScheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace hfg::v2 {

struct GlassSceneReconcileResult {
  GlassRenderScene scene;
  std::vector<CaptureAllocationFailure> allocationFailures;
  std::vector<std::uint64_t> retiredTokens;
};

struct GlassPassEnqueueResult {
  std::size_t capturePasses = 0;
  std::size_t drawPasses = 0;
  std::vector<PixelRect> renderDamage;
  std::vector<Rect> continuationDamage;
  bool directScanoutBlockRequired = false;
};

struct HyprlandGlassPassExecutionState;

class GlassPassObserver {
public:
  virtual ~GlassPassObserver() = default;
  virtual void onCaptureResult(std::uint64_t resourceToken,
                               std::uint64_t frameToken,
                               const std::optional<Error> &error) noexcept = 0;
  virtual void onDrawResult(const PresentationKey &key,
                            std::uint64_t frameToken,
                            const std::optional<Error> &error) noexcept = 0;
};

class HyprlandGlassPassCoordinator {
public:
  HyprlandGlassPassCoordinator();
  ~HyprlandGlassPassCoordinator();

  HyprlandGlassPassCoordinator(const HyprlandGlassPassCoordinator &) = delete;
  HyprlandGlassPassCoordinator &
  operator=(const HyprlandGlassPassCoordinator &) = delete;
  HyprlandGlassPassCoordinator(HyprlandGlassPassCoordinator &&) = delete;
  HyprlandGlassPassCoordinator &
  operator=(HyprlandGlassPassCoordinator &&) = delete;

  [[nodiscard]] Result<GlassSceneReconcileResult>
  reconcile(const CaptureScene &captures, std::uint64_t maxTotalBytes);

  [[nodiscard]] Result<GlassPassEnqueueResult>
  enqueue(const RenderHookEvent &event);

  [[nodiscard]] Result<GlassPassEnqueueResult>
  enqueueWindowDecoration(const RenderHookEvent &event,
                          const TargetIdentity &identity,
                          std::uint64_t objectToken, double opacity);

  [[nodiscard]] const GlassRenderScene &scene() const noexcept;

  void setObserver(std::weak_ptr<GlassPassObserver> observer) noexcept;
  void clear() noexcept;

private:
  std::shared_ptr<HyprlandGlassPassExecutionState> m_execution;
  GlassRenderScene m_scene;
  // One-slot memo: at pre-window the decoration path replans the exact frame
  // the plain enqueue just planned, so the plan is kept for that frame.
  struct FramePlanMemo {
    std::uint64_t frameToken = 0;
    std::string output;
    std::vector<PixelRect> damage;
    GlassFramePlan plan;
  };
  std::optional<FramePlanMemo> m_framePlanMemo;
};

} // namespace hfg::v2
