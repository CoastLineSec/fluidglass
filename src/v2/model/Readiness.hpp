#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Target.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hfg::v2 {

enum class ReadinessState {
    Accepted,
    Resolved,
    Attached,
    CaptureReady,
    Drawn,
    Invalid,
    Unresolved,
    Unsupported,
    CaptureFailed,
    ShaderFailed,
    ResourceLimited,
    Expired,
    Detached,
};

[[nodiscard]] constexpr std::string_view readinessStateName(ReadinessState state) noexcept {
    switch (state) {
        case ReadinessState::Accepted:        return "accepted";
        case ReadinessState::Resolved:        return "resolved";
        case ReadinessState::Attached:        return "attached";
        case ReadinessState::CaptureReady:    return "capture-ready";
        case ReadinessState::Drawn:           return "drawn";
        case ReadinessState::Invalid:         return "invalid";
        case ReadinessState::Unresolved:      return "unresolved";
        case ReadinessState::Unsupported:     return "unsupported";
        case ReadinessState::CaptureFailed:   return "capture-failed";
        case ReadinessState::ShaderFailed:    return "shader-failed";
        case ReadinessState::ResourceLimited: return "resource-limited";
        case ReadinessState::Expired:         return "expired";
        case ReadinessState::Detached:        return "detached";
    }
    return "invalid";
}

struct TargetIdentity {
    std::string owner;
    std::string targetId;

    friend bool operator==(const TargetIdentity&, const TargetIdentity&) = default;
    friend auto operator<=>(const TargetIdentity&, const TargetIdentity&) = default;
};

struct PresentationKey {
    TargetIdentity identity;
    std::string    output;
    std::uint64_t  outputGeneration = 0;
    RenderStage    stage = RenderStage::PostWindows;

    friend bool operator==(const PresentationKey&, const PresentationKey&) = default;
    friend auto operator<=>(const PresentationKey&, const PresentationKey&) = default;
};

struct ReadinessRecord {
    ReadinessState state = ReadinessState::Accepted;
    std::uint64_t  sequence = 0;
    std::string    detail;

    friend bool operator==(const ReadinessRecord&, const ReadinessRecord&) = default;
};

class ReadinessTracker {
  public:
    [[nodiscard]] Result<ReadinessRecord> accept(TargetIdentity identity);
    [[nodiscard]] Result<ReadinessRecord> failTarget(
        const TargetIdentity& identity,
        ReadinessState state,
        std::string detail = {});

    [[nodiscard]] Result<ReadinessRecord> resolvePresentation(PresentationKey key);
    [[nodiscard]] Result<ReadinessRecord> transition(
        const PresentationKey& key,
        ReadinessState state,
        std::string detail = {});

    [[nodiscard]] std::optional<ReadinessRecord> target(const TargetIdentity& identity) const;
    [[nodiscard]] std::optional<ReadinessRecord> presentation(const PresentationKey& key) const;
    [[nodiscard]] std::vector<std::pair<PresentationKey, ReadinessRecord>> presentations(
        const TargetIdentity& identity) const;

    void erase(const TargetIdentity& identity);
    void erasePresentation(const PresentationKey& key);

  private:
    [[nodiscard]] ReadinessRecord nextRecord(ReadinessState state, std::string detail);
    [[nodiscard]] static bool validPresentationTransition(ReadinessState from, ReadinessState to);
    [[nodiscard]] static bool validTargetFailure(ReadinessState state);

    std::map<TargetIdentity, ReadinessRecord>   m_targets;
    std::map<PresentationKey, ReadinessRecord>  m_presentations;
    std::uint64_t                               m_sequence = 0;
};

} // namespace hfg::v2
