#pragma once

#include "v2/render/HyprlandCaptureEnvironment.hpp"
#include "v2/render/HyprlandDirectScanoutInhibitor.hpp"
#include "v2/render/HyprlandGlassPassCoordinator.hpp"
#include "v2/render/HyprlandOutputCatalog.hpp"
#include "v2/render/HyprlandRenderStageAdapter.hpp"
#include "v2/runtime/Runtime.hpp"
#include "v2/targets/HyprlandLayerCatalog.hpp"
#include "v2/targets/HyprlandWindowAttachmentManager.hpp"
#include "v2/targets/HyprlandWindowCatalog.hpp"

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace hfg::v2 {

class HyprlandGlassSceneController final
    : public GlassPassObserver,
      public std::enable_shared_from_this<HyprlandGlassSceneController> {
  public:
    static Result<std::shared_ptr<HyprlandGlassSceneController>>
    create(HANDLE pluginHandle, RuntimeService& runtime,
           CaptureBudget captureBudget);

    ~HyprlandGlassSceneController() override;

    HyprlandGlassSceneController(const HyprlandGlassSceneController&) = delete;
    HyprlandGlassSceneController&
    operator=(const HyprlandGlassSceneController&) = delete;

    [[nodiscard]] Result<void> refresh(std::uint64_t nowMs);
    [[nodiscard]] Result<void> onPreChecks(PHLMONITOR monitor,
                                            std::uint64_t nowMs);
    [[nodiscard]] Result<void> onRenderStage(eRenderStage stage,
                                              std::uint64_t nowMs);

    void onCaptureResult(std::uint64_t resourceToken,
                         std::uint64_t frameToken,
                         const std::optional<Error>& error) noexcept override;
    void onDrawResult(const PresentationKey& key,
                      std::uint64_t frameToken,
                      const std::optional<Error>& error) noexcept override;

    [[nodiscard]] bool renderingReady() const noexcept;
    [[nodiscard]] const std::optional<Error>& lastError() const noexcept;
    void clear() noexcept;

  private:
    HyprlandGlassSceneController(HANDLE pluginHandle,
                                 RuntimeService& runtime,
                                 CaptureBudget captureBudget);

    [[nodiscard]] Result<void> initialize();
    [[nodiscard]] Result<void> refreshResolvedScene(std::uint64_t nowMs);
    [[nodiscard]] Result<void> prepareRenderScene();
    [[nodiscard]] Result<void>
    drawWindowDecoration(const WindowDecorationDrawContext& context);
    void recordDecorationFailure(const TargetIdentity& identity,
                                 const Error& error) noexcept;
    void reconcileReadiness();
    void recordFailure(Error error) noexcept;
    void clearLiveState() noexcept;

    HANDLE m_pluginHandle = nullptr;
    RuntimeService& m_runtime;
    CaptureBudget m_captureBudget;
    HyprlandOutputCatalog m_outputs;
    HyprlandWindowCatalog m_windows;
    HyprlandLayerCatalog m_layers;
    HyprlandRenderStageAdapter m_stages;
    HyprlandDirectScanoutInhibitor m_scanout;
    HyprlandGlassPassCoordinator m_passes;
    std::unique_ptr<HyprlandWindowAttachmentManager> m_attachments;
    std::vector<OutputGeneration> m_currentOutputs;
    TargetScene m_targets;
    PresentationScene m_presentations;
    std::map<std::string, RenderHookEvent, std::less<>> m_pendingWindows;
    std::map<std::uint64_t, std::vector<PresentationKey>> m_capturePresentations;
    std::optional<Error> m_lastError;
    bool m_initialized = false;
    bool m_renderingReady = false;
};

} // namespace hfg::v2
