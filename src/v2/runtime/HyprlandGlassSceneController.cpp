#include "v2/runtime/HyprlandGlassSceneController.hpp"

#include "v2/render/CaptureScene.hpp"
#include "v2/runtime/LiveScenePlan.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

Result<void> failure(ErrorCode code, std::string path, std::string message) {
    return Result<void>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

ReadinessState readinessFailureState(const Error& error,
                                     bool captureBoundary = false) {
    if (error.code == ErrorCode::ResourceLimited)
        return ReadinessState::ResourceLimited;
    if (captureBoundary)
        return ReadinessState::CaptureFailed;
    if (error.code == ErrorCode::UnsupportedOperation ||
        error.code == ErrorCode::UnsupportedTarget)
        return ReadinessState::Unsupported;
    return ReadinessState::Unresolved;
}

using PresentationMembership =
    std::set<std::pair<PresentationKey, std::uint64_t>>;

PresentationMembership membershipOf(const PresentationScene& scene) {
    PresentationMembership result;
    for (const auto& planned : scene.presentations)
        result.emplace(planned.presentation.key,
                       planned.presentation.attachmentToken);
    return result;
}

} // namespace

Result<std::shared_ptr<HyprlandGlassSceneController>>
HyprlandGlassSceneController::create(HANDLE pluginHandle,
                                     RuntimeService& runtime,
                                     CaptureBudget captureBudget) {
    if (!pluginHandle)
        return Result<std::shared_ptr<HyprlandGlassSceneController>>::failure({
            .code = ErrorCode::UnsupportedOperation,
            .path = "plugin",
            .message = "Hyprland plugin handle is unavailable",
        });
    auto controller = std::shared_ptr<HyprlandGlassSceneController>(
        new HyprlandGlassSceneController(pluginHandle, runtime,
                                         captureBudget));
    if (auto initialized = controller->initialize(); !initialized)
        return Result<std::shared_ptr<HyprlandGlassSceneController>>::failure(
            initialized.error());
    return Result<std::shared_ptr<HyprlandGlassSceneController>>::success(
        std::move(controller));
}

HyprlandGlassSceneController::HyprlandGlassSceneController(
    HANDLE pluginHandle, RuntimeService& runtime, CaptureBudget captureBudget)
    : m_pluginHandle(pluginHandle), m_runtime(runtime),
      m_captureBudget(captureBudget), m_stages(m_outputs, m_windows),
      m_scanout(m_outputs) {}

HyprlandGlassSceneController::~HyprlandGlassSceneController() {
    clear();
}

Result<void> HyprlandGlassSceneController::initialize() {
    const std::weak_ptr<HyprlandGlassSceneController> weak =
        shared_from_this();
    m_attachments = std::make_unique<HyprlandWindowAttachmentManager>(
        m_pluginHandle, m_windows,
        [weak](const WindowDecorationDrawContext& context) -> Result<void> {
            const auto controller = weak.lock();
            if (!controller)
                return failure(ErrorCode::UnsupportedOperation,
                               "window-decoration",
                               "v2 scene controller is unavailable");
            return controller->drawWindowDecoration(context);
        },
        [weak](const TargetIdentity& identity, const Error& error) {
            if (const auto controller = weak.lock())
                controller->recordDecorationFailure(identity, error);
        });
    m_passes.setObserver(weak);
    m_initialized = true;
    return Result<void>::success();
}

Result<void> HyprlandGlassSceneController::refresh(std::uint64_t nowMs) {
    auto refreshed = refreshResolvedScene(nowMs);
    if (!refreshed) {
        recordFailure(refreshed.error());
        return refreshed;
    }
    m_lastError.reset();
    return Result<void>::success();
}

Result<void>
HyprlandGlassSceneController::onPreChecks(PHLMONITOR monitor,
                                          std::uint64_t nowMs) {
    if (!monitor)
        return failure(ErrorCode::UnresolvedTarget, "monitor",
                       "render pre-check has no output");
    return refresh(nowMs);
}

Result<void>
HyprlandGlassSceneController::onRenderStage(eRenderStage stage,
                                             std::uint64_t nowMs) {
    if (!m_initialized)
        return failure(ErrorCode::UnsupportedOperation, "controller",
                       "v2 scene controller is not initialized");

    if (stage == RENDER_BEGIN) {
        if (auto refreshed = refreshResolvedScene(nowMs); !refreshed) {
            recordFailure(refreshed.error());
            return refreshed;
        }
        if (auto prepared = prepareRenderScene(); !prepared) {
            recordFailure(prepared.error());
            return prepared;
        }
    }

    auto observed = m_stages.observe(stage);
    if (!observed) {
        recordFailure(observed.error());
        return Result<void>::failure(observed.error());
    }
    if (!observed.value())
        return Result<void>::success();

    const auto& event = *observed.value();
    auto enqueued = m_passes.enqueue(event);
    if (!enqueued) {
        recordFailure(enqueued.error());
        return Result<void>::failure(enqueued.error());
    }
    if (event.hook == RenderHookStage::PreWindow)
        m_pendingWindows.insert_or_assign(event.output.snapshot.name, event);
    else
        m_pendingWindows.erase(event.output.snapshot.name);
    return Result<void>::success();
}

Result<void> HyprlandGlassSceneController::refreshResolvedScene(
    std::uint64_t nowMs) {
    m_runtime.tick(nowMs);
    auto outputs = m_outputs.refresh();
    if (!outputs)
        return Result<void>::failure(outputs.error());
    auto windows = m_windows.allSnapshots();
    if (!windows)
        return Result<void>::failure(windows.error());
    auto layers = m_layers.allSnapshots();
    if (!layers)
        return Result<void>::failure(layers.error());
    const auto sessions = m_runtime.sessionManager().snapshots();
    auto targets = buildTargetScene(m_runtime.configStore().active(), sessions,
                                    windows.value(), layers.value(),
                                    outputs.value().current);
    if (!targets)
        return Result<void>::failure(targets.error());
    auto presentations =
        buildPresentationScene(targets.value(),
                               m_runtime.configStore().active(), sessions,
                               outputs.value().current, nowMs);
    if (!presentations)
        return Result<void>::failure(presentations.error());
    auto bindings =
        planLiveSceneBindings(presentations.value().presentations);
    if (!bindings)
        return Result<void>::failure(bindings.error());

    const auto previousLeases = m_scanout.leases();
    if (auto scanout =
            m_scanout.reconcile(bindings.value().directScanoutLeases);
        !scanout)
        return scanout;
    if (!m_attachments) {
        static_cast<void>(m_scanout.reconcile(previousLeases));
        return failure(ErrorCode::InternalError, "window-attachments",
                       "window attachment manager is unavailable");
    }
    if (auto attached =
            m_attachments->reconcile(bindings.value().windowAttachments);
        !attached) {
        static_cast<void>(m_scanout.reconcile(previousLeases));
        return attached;
    }

    const auto previousMembership = membershipOf(m_presentations);
    const auto membershipChanged =
        previousMembership != membershipOf(presentations.value());
    m_currentOutputs = std::move(outputs.value().current);
    m_targets = std::move(targets.value());
    m_presentations = std::move(presentations.value());
    reconcileReadiness(previousMembership);
    if (membershipChanged && g_pHyprRenderer && g_pCompositor)
        for (const auto& monitor : g_pCompositor->m_monitors)
            if (monitor)
                g_pHyprRenderer->damageMonitor(monitor);
    return Result<void>::success();
}

Result<void> HyprlandGlassSceneController::prepareRenderScene() {
    auto environment =
        inspectHyprlandCaptureEnvironment(m_currentOutputs, m_captureBudget);
    if (!environment)
        return Result<void>::failure(environment.error());
    auto captures =
        buildCaptureScene(m_presentations, environment.value().formats,
                          environment.value().limits);
    if (!captures)
        return Result<void>::failure(captures.error());
    auto reconciled =
        m_passes.reconcile(captures.value(),
                           environment.value().maxTotalBytes);
    if (!reconciled)
        return Result<void>::failure(reconciled.error());

    m_capturePresentations.clear();
    for (const auto& draw : reconciled.value().scene.draws)
        m_capturePresentations[draw.resourceToken].push_back(draw.key);

    auto& readiness = m_runtime.readinessTracker();
    for (const auto& captureFailure :
         reconciled.value().scene.captureFailures)
        if (readiness.presentation(captureFailure.key))
            static_cast<void>(readiness.transition(
                captureFailure.key,
                readinessFailureState(captureFailure.error, true),
                captureFailure.error.message));
    for (const auto& drawFailure : reconciled.value().scene.drawFailures)
        if (readiness.presentation(drawFailure.key))
            static_cast<void>(readiness.transition(
                drawFailure.key,
                readinessFailureState(drawFailure.error, true),
                drawFailure.error.message));

    m_renderingReady = true;
    m_lastError.reset();
    publishStatus();
    return Result<void>::success();
}

Result<void> HyprlandGlassSceneController::drawWindowDecoration(
    const WindowDecorationDrawContext& context) {
    if (!context.monitor)
        return failure(ErrorCode::UnresolvedTarget,
                       "window-decoration.monitor",
                       "window decoration has no output");
    const auto pending = m_pendingWindows.find(context.monitor->m_name);
    if (pending == m_pendingWindows.end())
        return failure(ErrorCode::StaleGeneration,
                       "window-decoration.frame",
                       "window decoration has no matching pre-window frame");
    if (pending->second.stageObjectToken != context.objectToken)
        return failure(ErrorCode::StaleGeneration,
                       "window-decoration.window",
                       "window decoration does not match the current pre-window target");
    auto enqueued = m_passes.enqueueWindowDecoration(
        pending->second, context.identity, context.objectToken,
        context.opacity);
    if (!enqueued)
        return Result<void>::failure(enqueued.error());
    m_pendingWindows.erase(pending);
    return Result<void>::success();
}

void HyprlandGlassSceneController::onCaptureResult(
    std::uint64_t resourceToken, std::uint64_t,
    const std::optional<Error>& error) noexcept {
    try {
        const auto found = m_capturePresentations.find(resourceToken);
        if (found == m_capturePresentations.end())
            return;
        auto& readiness = m_runtime.readinessTracker();
        for (const auto& key : found->second) {
            if (!readiness.presentation(key))
                continue;
            if (error)
                static_cast<void>(readiness.transition(
                    key, readinessFailureState(*error, true),
                    error->message));
            else
                static_cast<void>(readiness.transition(
                    key, ReadinessState::CaptureReady));
        }
    } catch (...) {
    }
}

void HyprlandGlassSceneController::onDrawResult(
    const PresentationKey& key, std::uint64_t,
    const std::optional<Error>& error) noexcept {
    try {
        auto& readiness = m_runtime.readinessTracker();
        if (!readiness.presentation(key))
            return;
        if (error)
            static_cast<void>(readiness.transition(
                key,
                error->path == "capture" ? ReadinessState::CaptureFailed
                                         : ReadinessState::ShaderFailed,
                error->message));
        else
            static_cast<void>(
                readiness.transition(key, ReadinessState::Drawn));
    } catch (...) {
    }
}

void HyprlandGlassSceneController::recordDecorationFailure(
    const TargetIdentity& identity, const Error& error) noexcept {
    try {
        auto& readiness = m_runtime.readinessTracker();
        for (const auto& [key, record] : readiness.presentations(identity)) {
            static_cast<void>(record);
            static_cast<void>(readiness.transition(
                key, readinessFailureState(error), error.message));
        }
    } catch (...) {
    }
}

void HyprlandGlassSceneController::reconcileReadiness(
    const std::set<std::pair<PresentationKey, std::uint64_t>>&
        previousMembership) {
    auto& readiness = m_runtime.readinessTracker();
    std::set<PresentationKey> currentKeys;
    for (const auto& planned : m_presentations.presentations) {
        const auto& key = planned.presentation.key;
        const auto targetRecord = readiness.target(key.identity);
        if (!targetRecord)
            continue;
        if (targetRecord->state != ReadinessState::Accepted)
            static_cast<void>(readiness.accept(key.identity));
        currentKeys.insert(key);
        const auto unchanged = previousMembership.contains(
            {key, planned.presentation.attachmentToken});
        if (!unchanged && readiness.presentation(key))
            readiness.erasePresentation(key);
        if (!readiness.presentation(key)) {
            static_cast<void>(readiness.resolvePresentation(key));
            static_cast<void>(
                readiness.transition(key, ReadinessState::Attached));
        }
    }

    for (const auto& session : m_runtime.sessionManager().snapshots())
        for (const auto& target : session.targets) {
            const TargetIdentity identity{
                .owner = session.owner,
                .targetId = target.id,
            };
            for (const auto& [key, record] :
                 readiness.presentations(identity)) {
                static_cast<void>(record);
                if (!currentKeys.contains(key))
                    readiness.erasePresentation(key);
            }
        }

    for (const auto& failed : m_presentations.failures)
        if (readiness.target(failed.identity))
            static_cast<void>(readiness.failTarget(
                failed.identity, readinessFailureState(failed.error),
                failed.error.message));
}

void HyprlandGlassSceneController::recordFailure(Error error) noexcept {
    m_lastError = std::move(error);
    m_renderingReady = false;
    clearLiveState();
    publishStatus();
}

bool HyprlandGlassSceneController::renderingReady() const noexcept {
    return m_renderingReady;
}

const std::optional<Error>&
HyprlandGlassSceneController::lastError() const noexcept {
    return m_lastError;
}

void HyprlandGlassSceneController::clearLiveState() noexcept {
    m_passes.clear();
    m_scanout.clear();
    if (m_attachments)
        static_cast<void>(m_attachments->clear());
    m_pendingWindows.clear();
    m_capturePresentations.clear();
}

void HyprlandGlassSceneController::publishStatus() noexcept {
    try {
        const auto& scene = m_passes.scene();
        m_runtime.setRendererStatus({
            .renderingReady = m_renderingReady,
            .renderer = m_renderingReady ? "active" :
                        (m_lastError ? "failed" : "inactive"),
            .presentations = m_presentations.presentations.size(),
            .captureResources = scene.resources.size(),
            .draws = scene.draws.size(),
            .windowAttachments =
                m_attachments ? m_attachments->attached().size() : 0U,
            .directScanoutLeases = m_scanout.leases().size(),
            .lastError = m_lastError,
        });
    } catch (...) {
    }
}

void HyprlandGlassSceneController::clear() noexcept {
    m_passes.setObserver({});
    clearLiveState();
    m_stages.clear();
    m_layers.clear();
    m_windows.clear();
    m_outputs.clear();
    m_currentOutputs.clear();
    m_targets = {};
    m_presentations = {};
    m_initialized = false;
    m_renderingReady = false;
    m_lastError.reset();
    publishStatus();
}

} // namespace hfg::v2
