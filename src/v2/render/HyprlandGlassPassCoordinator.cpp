#include "v2/render/HyprlandGlassPassCoordinator.hpp"

#include "v2/render/GlassFramePlan.hpp"
#include "v2/render/CaptureExecutionTracker.hpp"
#include "v2/render/HyprlandCaptureExecutor.hpp"
#include "v2/render/HyprlandGlassDrawExecutor.hpp"
#include "v2/render/HyprlandGlassShader.hpp"
#include "v2/render/PresentationResourceCache.hpp"

#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace hfg::v2 {

struct HyprlandGlassPassExecutionState {
  HyprlandCaptureResourceManager resources;
  RenderStageScheduler scheduler;
  CaptureExecutionTracker captures;
  HyprlandGlassShader shader;
  PresentationResourceCache<HyprlandGlassBlur> blurs;
  std::weak_ptr<GlassPassObserver> observer;
};

namespace {

template <typename T>
Result<T> failure(ErrorCode code, std::string path, std::string message) {
  return Result<T>::failure({
      .code = code,
      .path = std::move(path),
      .message = std::move(message),
  });
}

void logPassFailure(std::string_view pass, const Error &error) noexcept {
  try {
    if (Log::logger)
      Log::logger->log(Log::ERR, "[hyprfluidglass] {} failed ({} at {}): {}",
                       pass, errorCodeName(error.code), error.path,
                       error.message);
  } catch (...) {
  }
}

void logUnexpectedPassFailure(std::string_view pass,
                              std::string_view message) noexcept {
  try {
    if (Log::logger)
      Log::logger->log(Log::ERR, "[hyprfluidglass] {} failed unexpectedly: {}",
                       pass, message);
  } catch (...) {
  }
}

void reportCapture(const std::shared_ptr<HyprlandGlassPassExecutionState> &execution,
                   std::uint64_t resourceToken, std::uint64_t frameToken,
                   std::optional<Error> error) noexcept {
  try {
    if (execution)
      if (const auto observer = execution->observer.lock())
        observer->onCaptureResult(resourceToken, frameToken, error);
  } catch (...) {
  }
}

void reportDraw(const std::shared_ptr<HyprlandGlassPassExecutionState> &execution,
                const PresentationKey &key, std::uint64_t frameToken,
                std::optional<Error> error) noexcept {
  try {
    if (execution)
      if (const auto observer = execution->observer.lock())
        observer->onDrawResult(key, frameToken, error);
  } catch (...) {
  }
}

std::shared_ptr<HyprlandGlassBlur> blurFor(
    const std::shared_ptr<HyprlandGlassPassExecutionState> &execution,
    const PresentationKey &key) {
  return execution->blurs.resourceFor(key);
}

class V2CapturePass final : public IPassElement {
public:
  V2CapturePass(std::shared_ptr<HyprlandGlassPassExecutionState> execution,
                std::uint64_t resourceToken, CapturePlan expected,
                OutputGeneration output,
                std::vector<PixelRect> outputDamage,
                std::uint64_t frameToken)
      : m_execution(std::move(execution)), m_resourceToken(resourceToken),
        m_expected(std::move(expected)), m_output(std::move(output)),
        m_outputDamage(std::move(outputDamage)),
        m_frameToken(frameToken) {}

  bool needsLiveBlur() override { return false; }
  bool needsPrecomputeBlur() override { return false; }
  const char *passName() override { return "HyprFluidGlassV2Capture"; }
  ePassElementType type() override { return EK_CUSTOM; }
  std::optional<CBox> boundingBox() override { return std::nullopt; }
  CRegion opaqueRegion() override { return {}; }
  bool undiscardable() override { return true; }

  std::vector<UP<IPassElement>> draw() override {
    try {
      if (!m_execution) {
        logUnexpectedPassFailure(passName(), "execution state is unavailable");
        return {};
      }
      auto *resource =
          m_execution->resources.resourceFor(m_resourceToken);
      if (!resource || !(resource->plan() == m_expected)) {
        const Error error{
            .code = ErrorCode::StaleGeneration,
            .path = "resource",
            .message = "capture allocation changed before pass execution",
        };
        m_execution->captures.fail(m_resourceToken, m_frameToken);
        logPassFailure(passName(), error);
        reportCapture(m_execution, m_resourceToken, m_frameToken, error);
        return {};
      }
      if (auto captured = captureCurrentFramebuffer(
              *resource,
              m_output,
              m_outputDamage);
          !captured) {
        m_execution->captures.fail(m_resourceToken, m_frameToken);
        logPassFailure(passName(), captured.error());
        reportCapture(m_execution, m_resourceToken, m_frameToken,
                      captured.error());
        return {};
      }
      if (auto completed =
              m_execution->captures.complete(m_resourceToken, m_frameToken);
          !completed) {
        logPassFailure(passName(), completed.error());
        reportCapture(m_execution, m_resourceToken, m_frameToken,
                      completed.error());
        return {};
      }
      reportCapture(m_execution, m_resourceToken, m_frameToken, std::nullopt);
    } catch (const std::exception &error) {
      logUnexpectedPassFailure(passName(), error.what());
      const Error failure{
          .code = ErrorCode::InternalError,
          .path = "capture",
          .message = "capture pass raised an unexpected exception",
      };
      if (m_execution)
        m_execution->captures.fail(m_resourceToken, m_frameToken);
      reportCapture(m_execution, m_resourceToken, m_frameToken, failure);
    } catch (...) {
      logUnexpectedPassFailure(passName(), "non-standard exception");
      const Error failure{
          .code = ErrorCode::InternalError,
          .path = "capture",
          .message = "capture pass raised an unexpected exception",
      };
      if (m_execution)
        m_execution->captures.fail(m_resourceToken, m_frameToken);
      reportCapture(m_execution, m_resourceToken, m_frameToken, failure);
    }
    return {};
  }

private:
  std::shared_ptr<HyprlandGlassPassExecutionState> m_execution;
  std::uint64_t m_resourceToken = 0;
  CapturePlan m_expected;
  OutputGeneration m_output;
  std::vector<PixelRect> m_outputDamage;
  std::uint64_t m_frameToken = 0;
};

class V2GlassPass final : public IPassElement {
public:
  V2GlassPass(std::shared_ptr<HyprlandGlassPassExecutionState> execution,
              GlassDrawPlan plan, OutputGeneration output,
              std::uint64_t frameToken)
      : m_execution(std::move(execution)), m_plan(std::move(plan)),
        m_output(std::move(output)), m_frameToken(frameToken),
        m_blur(blurFor(m_execution, m_plan.key)) {}

  bool needsLiveBlur() override { return false; }
  bool needsPrecomputeBlur() override { return false; }
  const char *passName() override { return "HyprFluidGlassV2Draw"; }
  ePassElementType type() override { return EK_CUSTOM; }
  std::optional<CBox> boundingBox() override {
    return CBox{m_plan.destination.x, m_plan.destination.y,
                m_plan.destination.width, m_plan.destination.height};
  }
  CRegion opaqueRegion() override { return {}; }
  bool undiscardable() override { return true; }

  std::vector<UP<IPassElement>> draw() override {
    try {
      if (!m_execution) {
        logUnexpectedPassFailure(passName(), "execution state is unavailable");
        return {};
      }
      auto drawn = draw(m_plan, m_blur);
      if (drawn && drawn.value()) {
        reportDraw(m_execution, m_plan.key, m_frameToken, std::nullopt);
        return {};
      }

      const auto primaryError = drawn
                                    ? Error{
                                          .code = ErrorCode::StaleGeneration,
                                          .path = "draw",
                                          .message = "glass draw did not produce a presentation",
                                      }
                                    : drawn.error();
      logPassFailure(passName(), primaryError);
      reportDraw(m_execution, m_plan.key, m_frameToken, primaryError);
    } catch (const std::exception &error) {
      logUnexpectedPassFailure(passName(), error.what());
      reportDraw(m_execution, m_plan.key, m_frameToken,
                 Error{
                     .code = ErrorCode::InternalError,
                     .path = "draw",
                     .message = "glass draw raised an unexpected exception",
                 });
    } catch (...) {
      logUnexpectedPassFailure(passName(), "non-standard exception");
      reportDraw(m_execution, m_plan.key, m_frameToken,
                 Error{
                     .code = ErrorCode::InternalError,
                     .path = "draw",
                     .message = "glass draw raised an unexpected exception",
                 });
    }
    return {};
  }

private:
  Result<bool> draw(const GlassDrawPlan &plan,
                    const std::shared_ptr<HyprlandGlassBlur> &blur) {
    if (!m_execution->captures.ready(plan.resourceToken, m_frameToken))
      return Result<bool>::failure({
          .code = ErrorCode::StaleGeneration,
          .path = "capture",
          .message =
              "glass draw was skipped because its capture did not succeed in this frame",
      });
    const auto *resource =
        m_execution->resources.resourceFor(plan.resourceToken);
    if (!resource || !(resource->plan() == plan.capture))
      return Result<bool>::failure({
          .code = ErrorCode::StaleGeneration,
          .path = "resource",
          .message = "draw allocation changed before pass execution",
      });
    if (!blur)
      return Result<bool>::failure({
          .code = ErrorCode::InternalError,
          .path = "blur",
          .message = "glass blur resources are unavailable",
      });
    return drawGlass(plan, plan.resourceToken, *resource, m_output,
                     m_execution->shader, *blur);
  }

  std::shared_ptr<HyprlandGlassPassExecutionState> m_execution;
  GlassDrawPlan m_plan;
  OutputGeneration m_output;
  std::uint64_t m_frameToken = 0;
  std::shared_ptr<HyprlandGlassBlur> m_blur;
};

Result<std::vector<PixelRect>>
currentFrameDamage(const OutputSnapshot &output) {
  if (!g_pHyprRenderer)
    return failure<std::vector<PixelRect>>(ErrorCode::UnsupportedOperation,
                                           "renderer",
                                           "Hyprland renderer is unavailable");
  const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
  if (!monitor || monitor->m_name != output.name)
    return failure<std::vector<PixelRect>>(
        ErrorCode::StaleGeneration, "renderer.output",
        "current render output differs from the frame event");

  CRegion clipped = g_pHyprRenderer->m_renderData.damage.copy();
  auto oriented = outputOrientedPixelSize(output);
  if (!oriented)
    return Result<std::vector<PixelRect>>::failure(
        oriented.error());
  clipped.intersect(
      0,
      0,
      oriented.value().width,
      oriented.value().height);
  std::vector<PixelRect> result;
  clipped.forEachRect([&result](const pixman_box32 &rect) {
    const auto width = rect.x2 - rect.x1;
    const auto height = rect.y2 - rect.y1;
    if (width > 0 && height > 0)
      result.push_back({
          .x = rect.x1,
          .y = rect.y1,
          .width = width,
          .height = height,
      });
  });
  return Result<std::vector<PixelRect>>::success(std::move(result));
}

} // namespace

HyprlandGlassPassCoordinator::HyprlandGlassPassCoordinator()
    : m_execution(std::make_shared<HyprlandGlassPassExecutionState>()) {}

HyprlandGlassPassCoordinator::~HyprlandGlassPassCoordinator() = default;

Result<GlassSceneReconcileResult>
HyprlandGlassPassCoordinator::reconcile(const CaptureScene &captures,
                                        std::uint64_t maxTotalBytes) {
  if (!m_execution)
    return failure<GlassSceneReconcileResult>(
        ErrorCode::InternalError, "execution",
        "glass pass execution state is unavailable");

  auto resources = [&]() -> Result<CaptureResourceReconcileResult> {
    auto reconciled = m_execution->resources.reconcile(
        captures.captures, maxTotalBytes);
    if (!reconciled)
      return reconciled;

    auto scene = buildGlassRenderScene(
        captures, reconciled.value().resources);
    if (!scene)
      return Result<CaptureResourceReconcileResult>::failure(scene.error());
    m_scene = std::move(scene.value());
    m_framePlanMemo.reset();
    return reconciled;
  }();
  if (!resources)
    return Result<GlassSceneReconcileResult>::failure(resources.error());
  std::vector<PresentationKey> activeBlurResources;
  activeBlurResources.reserve(m_scene.draws.size());
  for (const auto &draw : m_scene.draws)
    activeBlurResources.push_back(draw.key);
  m_execution->blurs.retain(activeBlurResources);
  for (const auto token : resources.value().retiredTokens)
    m_execution->captures.retire(token);
  return Result<GlassSceneReconcileResult>::success({
      .scene = m_scene,
      .allocationFailures = std::move(resources.value().failures),
      .retiredTokens = std::move(resources.value().retiredTokens),
  });
}

Result<GlassPassEnqueueResult>
HyprlandGlassPassCoordinator::enqueue(const RenderHookEvent &event) {
  if (!m_execution)
    return failure<GlassPassEnqueueResult>(
        ErrorCode::InternalError, "execution",
        "glass pass execution state is unavailable");
  if (!g_pHyprRenderer)
    return failure<GlassPassEnqueueResult>(ErrorCode::UnsupportedOperation,
                                           "renderer",
                                           "Hyprland renderer is unavailable");

  auto damage = currentFrameDamage(event.output.snapshot);
  if (!damage)
    return Result<GlassPassEnqueueResult>::failure(damage.error());
  auto frame = planGlassFrame(m_scene, event, damage.value());
  if (frame && event.hook == RenderHookStage::PreWindow)
    m_framePlanMemo = FramePlanMemo{
        .frameToken = event.frameToken,
        .output = event.output.snapshot.name,
        .damage = damage.value(),
        .plan = frame.value(),
    };
  if (!frame)
    return Result<GlassPassEnqueueResult>::failure(frame.error());

  std::vector<CaptureResource> selectedResources;
  selectedResources.reserve(frame.value().captureTokens.size());
  for (const auto token : frame.value().captureTokens) {
    const auto selected =
        std::ranges::find_if(m_scene.resources, [token](const auto &resource) {
          return resource.token == token;
        });
    if (selected == m_scene.resources.end())
      return failure<GlassPassEnqueueResult>(
          ErrorCode::StaleGeneration, "frame.capture_tokens",
          "frame references a capture resource absent from the scene");
    selectedResources.push_back(*selected);
  }
  auto scheduled = m_execution->scheduler.schedule(selectedResources, event);
  if (!scheduled)
    return Result<GlassPassEnqueueResult>::failure(scheduled.error());
  for (const auto &resource : scheduled.value()) {
    auto tracked = m_execution->captures.schedule(resource.token,
                                                  event.frameToken);
    if (!tracked)
      return Result<GlassPassEnqueueResult>::failure(tracked.error());
  }

  for (const auto index : frame.value().drawIndices)
    if (index >= m_scene.draws.size())
      return failure<GlassPassEnqueueResult>(
          ErrorCode::InternalError, "frame.draw_indices",
          "frame references a draw absent from the scene");

  for (const auto &rect : frame.value().renderDamage)
    g_pHyprRenderer->m_renderData.damage.add(rect.x, rect.y, rect.width,
                                             rect.height);
  for (const auto &rect : frame.value().continuationDamage)
    g_pHyprRenderer->damageBox(CBox{rect.x, rect.y, rect.width, rect.height});

  for (const auto &resource : scheduled.value())
    g_pHyprRenderer->m_renderPass.add(makeUnique<V2CapturePass>(
        m_execution, resource.token, resource.plan, event.output,
        damage.value(),
        event.frameToken));
  const auto decorationOwned = event.hook == RenderHookStage::PreWindow;
  if (!decorationOwned)
    for (const auto index : frame.value().drawIndices)
      g_pHyprRenderer->m_renderPass.add(makeUnique<V2GlassPass>(
          m_execution, m_scene.draws[index], event.output,
          event.frameToken));

  return Result<GlassPassEnqueueResult>::success({
      .capturePasses = scheduled.value().size(),
      .drawPasses = decorationOwned ? 0U : frame.value().drawIndices.size(),
      .renderDamage = std::move(frame.value().renderDamage),
      .continuationDamage = std::move(frame.value().continuationDamage),
      .directScanoutBlockRequired = frame.value().blockDirectScanout,
  });
}

Result<GlassPassEnqueueResult>
HyprlandGlassPassCoordinator::enqueueWindowDecoration(
    const RenderHookEvent &event, const TargetIdentity &identity,
    std::uint64_t objectToken, double opacity) {
  if (!m_execution)
    return failure<GlassPassEnqueueResult>(
        ErrorCode::InternalError, "execution",
        "glass pass execution state is unavailable");
  if (!g_pHyprRenderer)
    return failure<GlassPassEnqueueResult>(ErrorCode::UnsupportedOperation,
                                           "renderer",
                                           "Hyprland renderer is unavailable");
  if (!std::isfinite(opacity) || opacity < 0.0 || opacity > 1.0)
    return failure<GlassPassEnqueueResult>(
        ErrorCode::InvalidRequest, "opacity",
        "window decoration opacity must be finite from 0 through 1");

  const auto memoized =
      m_framePlanMemo &&
      m_framePlanMemo->frameToken == event.frameToken &&
      m_framePlanMemo->output == event.output.snapshot.name;
  std::vector<PixelRect> computedDamage;
  if (!memoized) {
    auto damage = currentFrameDamage(event.output.snapshot);
    if (!damage)
      return Result<GlassPassEnqueueResult>::failure(damage.error());
    computedDamage = std::move(damage.value());
  }
  const auto &frameDamage =
      memoized ? m_framePlanMemo->damage : computedDamage;
  auto selected = planWindowDecorationDraws(
      m_scene, event, identity, objectToken, frameDamage,
      memoized ? &m_framePlanMemo->plan : nullptr);
  if (!selected)
    return Result<GlassPassEnqueueResult>::failure(selected.error());

  for (const auto index : selected.value()) {
    auto draw = m_scene.draws[index];
    draw.opacity = opacity;
    g_pHyprRenderer->m_renderPass.add(
        makeUnique<V2GlassPass>(m_execution, std::move(draw), event.output,
                                event.frameToken));
  }
  return Result<GlassPassEnqueueResult>::success({
      .capturePasses = 0,
      .drawPasses = selected.value().size(),
      .renderDamage = {},
      .continuationDamage = {},
      .directScanoutBlockRequired = !selected.value().empty(),
  });
}

const GlassRenderScene &HyprlandGlassPassCoordinator::scene() const noexcept {
  return m_scene;
}

void HyprlandGlassPassCoordinator::setObserver(
    std::weak_ptr<GlassPassObserver> observer) noexcept {
  if (m_execution)
    m_execution->observer = std::move(observer);
}

void HyprlandGlassPassCoordinator::clear() noexcept {
  m_scene = {};
  m_framePlanMemo.reset();
  if (!m_execution)
    return;
  m_execution->scheduler.clear();
  m_execution->captures.clear();
  m_execution->resources.clear();
  m_execution->blurs.clear();
  m_execution->shader.reset();
}

} // namespace hfg::v2
