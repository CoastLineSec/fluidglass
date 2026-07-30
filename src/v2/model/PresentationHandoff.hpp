#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Readiness.hpp"
#include "v2/model/Session.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hfg::v2 {

enum class PresentationHandoffState {
    Retained,
    Completed,
    Failed,
};

[[nodiscard]] constexpr std::string_view presentationHandoffStateName(
    PresentationHandoffState state) noexcept {
    switch (state) {
        case PresentationHandoffState::Retained:  return "retained";
        case PresentationHandoffState::Completed: return "completed";
        case PresentationHandoffState::Failed:    return "failed";
    }
    return "failed";
}

struct PreparedPresentationHandoff {
    TargetIdentity               identity;
    std::uint64_t                sourceGeneration = 0;
    std::uint64_t                timeoutMs = 0;
    std::vector<PresentationKey> presentations;
};

struct PresentationHandoffPresentation {
    PresentationKey          key;
    PresentationHandoffState state = PresentationHandoffState::Retained;
    std::string              detail;

    friend bool operator==(const PresentationHandoffPresentation&,
                           const PresentationHandoffPresentation&) = default;
};

struct PresentationHandoffRecord {
    TargetIdentity                             identity;
    std::uint64_t                              sourceGeneration = 0;
    std::uint64_t                              successorGeneration = 0;
    std::uint64_t                              expiresAtMs = 0;
    std::vector<PresentationHandoffPresentation> presentations;

    friend bool operator==(const PresentationHandoffRecord&,
                           const PresentationHandoffRecord&) = default;
};

class PresentationHandoffTracker {
  public:
    [[nodiscard]] Result<std::vector<PreparedPresentationHandoff>> prepare(
        const SessionSnapshot& current,
        const SessionReplacement& replacement,
        const ReadinessTracker& readiness) const;

    void commit(
        std::string_view owner,
        std::uint64_t successorGeneration,
        std::span<const PreparedPresentationHandoff> prepared,
        std::uint64_t nowMs);

    void complete(const PresentationKey& key);
    void fail(const PresentationKey& key, std::string detail);
    void fail(const TargetIdentity& identity, std::string detail);
    void expire(std::uint64_t nowMs);
    void eraseOwner(std::string_view owner);
    void clear() noexcept;

    [[nodiscard]] std::optional<PresentationHandoffRecord> target(
        const TargetIdentity& identity) const;
    [[nodiscard]] std::vector<PresentationHandoffRecord> active() const;

  private:
    std::map<TargetIdentity, PresentationHandoffRecord> m_records;
};

} // namespace hfg::v2
