#pragma once

#include "v2/core/Result.hpp"

#include <cstdint>
#include <map>
#include <utility>

namespace hfg::v2 {

class CaptureExecutionTracker {
  public:
    [[nodiscard]] Result<void> schedule(std::uint64_t resourceToken,
                                        std::uint64_t frameToken);
    [[nodiscard]] Result<void> complete(std::uint64_t resourceToken,
                                        std::uint64_t frameToken);
    void fail(std::uint64_t resourceToken, std::uint64_t frameToken) noexcept;
    [[nodiscard]] bool ready(std::uint64_t resourceToken,
                             std::uint64_t frameToken) const noexcept;
    void retire(std::uint64_t resourceToken) noexcept;
    void clear() noexcept;

  private:
    std::map<std::uint64_t, std::uint64_t> m_scheduled;
    std::map<std::uint64_t, std::uint64_t> m_completed;
};

} // namespace hfg::v2
