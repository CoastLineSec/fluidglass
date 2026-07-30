#pragma once

#include "v2/model/Config.hpp"
#include "v2/model/PresentationHandoff.hpp"
#include "v2/model/Readiness.hpp"
#include "v2/model/Session.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hfg::v2 {

struct RendererRuntimeStatus {
    bool renderingReady = false;
    std::string renderer = "inactive";
    std::size_t presentations = 0;
    std::size_t captureResources = 0;
    std::size_t draws = 0;
    std::size_t windowAttachments = 0;
    std::size_t directScanoutLeases = 0;
    std::optional<Error> lastError;

    friend bool operator==(const RendererRuntimeStatus&,
                           const RendererRuntimeStatus&) = default;
};

class RuntimeService {
  public:
    explicit RuntimeService(SessionManager::OpaqueIdFactory opaqueIdFactory);

    [[nodiscard]] std::string handle(std::string_view payload, std::uint64_t nowMs) noexcept;
    void tick(std::uint64_t nowMs) noexcept;

    [[nodiscard]] ConfigStore& configStore() noexcept;
    [[nodiscard]] const ConfigStore& configStore() const noexcept;
    [[nodiscard]] SessionManager& sessionManager() noexcept;
    [[nodiscard]] const SessionManager& sessionManager() const noexcept;
    [[nodiscard]] ReadinessTracker& readinessTracker() noexcept;
    [[nodiscard]] const ReadinessTracker& readinessTracker() const noexcept;
    [[nodiscard]] PresentationHandoffTracker& handoffTracker() noexcept;
    [[nodiscard]] const PresentationHandoffTracker& handoffTracker() const noexcept;
    void setRendererStatus(RendererRuntimeStatus status) noexcept;
    [[nodiscard]] const RendererRuntimeStatus& rendererStatus() const noexcept;

  private:
    void expireSessions(std::uint64_t nowMs);

    ConfigStore      m_config;
    SessionManager   m_sessions;
    ReadinessTracker m_readiness;
    PresentationHandoffTracker m_handoffs;
    RendererRuntimeStatus m_rendererStatus;
};

} // namespace hfg::v2
