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

enum class PresentationMorphState {
    Active,
    Settling,
    Completed,
    Failed,
};

[[nodiscard]] constexpr std::string_view presentationMorphStateName(
    PresentationMorphState state) noexcept {
    switch (state) {
        case PresentationMorphState::Active:    return "active";
        case PresentationMorphState::Settling:  return "settling";
        case PresentationMorphState::Completed: return "completed";
        case PresentationMorphState::Failed:    return "failed";
    }
    return "failed";
}

struct PresentationMorphEndpoint {
    Rect   rect;
    double radius = 0.0;

    friend bool operator==(const PresentationMorphEndpoint&,
                           const PresentationMorphEndpoint&) = default;
};

struct PreparedPresentationMorph {
    std::string                                      transitionId;
    PresentationHandoffRequest::MorphCoordinateSpace coordinateSpace =
        PresentationHandoffRequest::MorphCoordinateSpace::SurfaceLocal;
    PresentationMorphEndpoint                        source;
    PresentationMorphEndpoint                        destination;
    std::uint64_t                                    durationMs = 0;
};

struct PresentationMorphRecord {
    std::string                                      transitionId;
    PresentationHandoffRequest::MorphCoordinateSpace coordinateSpace =
        PresentationHandoffRequest::MorphCoordinateSpace::SurfaceLocal;
    PresentationMorphEndpoint                        source;
    PresentationMorphEndpoint                        destination;
    Rect                                             envelope;
    std::uint64_t                                    anchorMs = 0;
    std::uint64_t                                    durationMs = 0;
    PresentationMorphState                           state =
        PresentationMorphState::Active;
    std::string                                      detail;

    friend bool operator==(const PresentationMorphRecord&,
                           const PresentationMorphRecord&) = default;
};

struct ResolvedPresentationMorph {
    PresentationMorphEndpoint current;
    Rect                      envelope;
    double                    progress = 0.0;
    bool                      active = false;

    friend bool operator==(const ResolvedPresentationMorph&,
                           const ResolvedPresentationMorph&) = default;
};

[[nodiscard]] Result<ResolvedPresentationMorph> resolvePresentationMorph(
    const PresentationMorphRecord& morph,
    std::uint64_t nowMs);

struct PreparedPresentationHandoff {
    TargetIdentity               identity;
    std::uint64_t                sourceGeneration = 0;
    std::uint64_t                timeoutMs = 0;
    std::vector<PresentationKey> presentations;
    std::optional<PreparedPresentationMorph> morph;
    bool                         preserveActiveMorph = false;
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
    std::optional<PresentationMorphRecord>     morph;

    friend bool operator==(const PresentationHandoffRecord&,
                           const PresentationHandoffRecord&) = default;
};

class PresentationHandoffTracker {
  public:
    [[nodiscard]] Result<std::vector<PreparedPresentationHandoff>> prepare(
        const SessionSnapshot& current,
        const SessionReplacement& replacement,
        const ReadinessTracker& readiness,
        std::uint64_t nowMs = 0) const;

    void commit(
        std::string_view owner,
        std::uint64_t successorGeneration,
        std::span<const PreparedPresentationHandoff> prepared,
        std::uint64_t nowMs);

    void complete(const PresentationKey& key);
    void fail(const PresentationKey& key, std::string detail);
    void fail(const TargetIdentity& identity, std::string detail);
    void settleMorph(const TargetIdentity& identity);
    void expire(std::uint64_t nowMs);
    void eraseOwner(std::string_view owner);
    void clear() noexcept;

    [[nodiscard]] std::optional<PresentationHandoffRecord> target(
        const TargetIdentity& identity) const;
    [[nodiscard]] std::vector<PresentationHandoffRecord> active() const;
    [[nodiscard]] std::vector<PresentationHandoffRecord> morphing() const;

  private:
    std::map<TargetIdentity, PresentationHandoffRecord> m_records;
};

} // namespace hfg::v2
