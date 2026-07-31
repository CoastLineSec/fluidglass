#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Readiness.hpp"
#include "v2/model/Session.hpp"
#include "v2/render/Geometry.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hfg::v2 {

enum class VisibilityTransitionState {
    Armed,
    Active,
    Completed,
    Failed,
    Superseded,
};

struct VisibilityTransitionSample {
    Point  offset;
    double opacity = 1.0;
    double progress = 0.0;
    bool   active = false;
};

struct VisibilityTransitionRecord {
    TargetIdentity                identity;
    std::string                   transitionId;
    std::uint64_t                 sourceGeneration = 0;
    std::uint64_t                 successorGeneration = 0;
    VisibilityTransitionDirection direction =
        VisibilityTransitionDirection::Hide;
    VisibilityTransitionState     state =
        VisibilityTransitionState::Armed;
    Rect                          sourceRect;
    double                        sourceRadius = 0.0;
    Point                         sourceOffset;
    Point                         destinationOffset;
    double                        sourceOpacity = 1.0;
    double                        destinationOpacity = 0.0;
    double                        startingProgress = 0.0;
    std::uint64_t                 anchorMs = 0;
    std::uint64_t                 durationMs = 0;
    std::uint64_t                 expiresAtMs = 0;
    std::string                   output;
    std::string                   namespaceName;
    std::optional<std::uint64_t>  objectToken = std::nullopt;
    std::optional<std::uint64_t>  outputGeneration = std::nullopt;
    std::string                   detail;
};

struct PreparedVisibilityTransition {
    VisibilityTransitionRecord record;
};

class VisibilityTransitionTracker {
  public:
    [[nodiscard]] Result<std::vector<PreparedVisibilityTransition>> prepare(
        const std::optional<SessionSnapshot>& previous,
        const SessionReplacement& replacement,
        std::string_view owner,
        std::uint64_t nowMs) const;

    void commit(std::vector<PreparedVisibilityTransition> prepared);
    [[nodiscard]] std::optional<VisibilityTransitionRecord> target(
        const TargetIdentity& identity) const;
    [[nodiscard]] std::vector<VisibilityTransitionRecord> records() const;

    [[nodiscard]] Result<VisibilityTransitionSample> sample(
        const TargetIdentity& identity,
        std::uint64_t nowMs) const;

    bool bind(
        const TargetIdentity& identity,
        std::string_view output,
        std::uint64_t outputGeneration,
        std::uint64_t objectToken);
    bool activate(const PresentationKey& key, std::uint64_t nowMs);
    void fail(const TargetIdentity& identity, std::string detail);
    void eraseOwner(std::string_view owner);
    void erase(const TargetIdentity& identity);
    void expire(std::uint64_t nowMs);
    void clear();

  private:
    [[nodiscard]] static VisibilityTransitionSample sampleRecord(
        const VisibilityTransitionRecord& record,
        std::uint64_t nowMs);

    std::map<TargetIdentity, VisibilityTransitionRecord> m_records;
};

[[nodiscard]] std::string_view visibilityTransitionStateName(
    VisibilityTransitionState state) noexcept;
[[nodiscard]] std::string_view visibilityTransitionDirectionName(
    VisibilityTransitionDirection direction) noexcept;

} // namespace hfg::v2
