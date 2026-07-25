#pragma once

#include "v2/model/Config.hpp"
#include "v2/model/Readiness.hpp"
#include "v2/model/Session.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace hfg::v2 {

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

  private:
    void expireSessions(std::uint64_t nowMs);

    ConfigStore      m_config;
    SessionManager   m_sessions;
    ReadinessTracker m_readiness;
};

} // namespace hfg::v2
