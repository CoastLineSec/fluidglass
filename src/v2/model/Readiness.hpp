#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Target.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
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
    Inactive,
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
        case ReadinessState::Inactive:        return "inactive";
    }
    return "invalid";
}

struct TargetIdentity {
    std::string owner;
    std::string targetId;

    friend bool operator==(const TargetIdentity&, const TargetIdentity&) = default;
    friend auto operator<=>(const TargetIdentity&, const TargetIdentity&) = default;
};

// A target can resolve cleanly and still contribute nothing to draw. That is
// not a failure and will never turn into one, so without its own state it is
// indistinguishable from a target that is merely still being resolved — and a
// client waiting for a drawn presentation would wait forever with nothing to
// observe. The reason says which of the four ways it happened.
enum class TargetInactiveReason {
    Disabled,
    EmptyGeometry,
    Offscreen,
    Suppressed,
};

[[nodiscard]] constexpr std::string_view targetInactiveReasonName(
    TargetInactiveReason reason) noexcept {
    switch (reason) {
        case TargetInactiveReason::Disabled:      return "disabled";
        case TargetInactiveReason::EmptyGeometry: return "empty-geometry";
        case TargetInactiveReason::Offscreen:     return "offscreen";
        case TargetInactiveReason::Suppressed:    return "suppressed";
    }
    return "disabled";
}

[[nodiscard]] constexpr std::string_view targetInactiveReasonDetail(
    TargetInactiveReason reason) noexcept {
    switch (reason) {
        case TargetInactiveReason::Disabled:
            return "target is disabled";
        case TargetInactiveReason::EmptyGeometry:
            return "target geometry clips to nothing inside its attachment";
        case TargetInactiveReason::Offscreen:
            return "target intersects no current output";
        case TargetInactiveReason::Suppressed:
            return "a higher-precedence target holds the same attachment";
    }
    return "target is inactive";
}

struct InactiveTarget {
    TargetIdentity       identity;
    TargetInactiveReason reason = TargetInactiveReason::Disabled;

    friend bool operator==(const InactiveTarget&, const InactiveTarget&) = default;
    friend auto operator<=>(const InactiveTarget&, const InactiveTarget&) = default;
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

/**
 * Whether glass is drawing on one output.
 *
 * This is the whole of what a client needs in order to decide between glass and
 * its own neutral material. Per-target readiness stays inside the plugin — the
 * renderer needs it — but it stops being a client contract, because a client
 * that derives nothing from geometry has nothing to reconcile it against.
 *
 * Per output rather than per target because that is where failure actually
 * lives: backdrop capture resources are owned by an output generation, so a
 * capture failure takes out every target on that output and nothing elsewhere.
 */
/** An output the renderer currently serves, whether or not glass is on it. */
struct KnownOutput {
    std::string   name;
    std::uint64_t generation = 0;

    friend bool operator==(const KnownOutput&, const KnownOutput&) = default;
};

struct OutputGlassLiveness {
    std::string   output;
    std::uint64_t outputGeneration = 0;
    std::size_t   drawn = 0;
    std::size_t   awaiting = 0;
    std::size_t   failed = 0;
    std::size_t   inactive = 0;
    /** At least one presentation on this output is confirmed drawn. */
    bool          drawing = false;

    friend bool operator==(const OutputGlassLiveness&, const OutputGlassLiveness&) = default;
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

    /** Every presentation the tracker holds, for aggregation. */
    [[nodiscard]] std::vector<std::pair<PresentationKey, ReadinessRecord>>
    allPresentations() const;

    /** Every accepted target identity, for scene-driven cleanup. */
    [[nodiscard]] std::vector<TargetIdentity> targetIdentities() const;

  private:
    [[nodiscard]] ReadinessRecord nextRecord(ReadinessState state, std::string detail);
    [[nodiscard]] static bool validPresentationTransition(ReadinessState from, ReadinessState to);
    [[nodiscard]] static bool validTargetFailure(ReadinessState state);

    std::map<TargetIdentity, ReadinessRecord>   m_targets;
    std::map<PresentationKey, ReadinessRecord>  m_presentations;
    std::uint64_t                               m_sequence = 0;
};

/**
 * Rolls per-presentation readiness up to one verdict per output.
 *
 * Outputs are reported in name order. An output with presentations but none
 * drawn reports `drawing == false`, which is what keeps a client on its neutral
 * material rather than going transparent over nothing.
 */
/**
 * Rolls per-presentation readiness up to one verdict per output.
 *
 * Every known output gets a row, presentations or not: a client polling
 * between a publish and its first resolution must see "this output exists and
 * nothing is drawn there yet", never a vanished row it cannot distinguish from
 * an unplugged monitor. An all-zero row means nothing is planned for that
 * output; `awaiting > 0` means glass is expected and not yet confirmed.
 */
[[nodiscard]] std::vector<OutputGlassLiveness> outputGlassLiveness(
    const ReadinessTracker& readiness,
    std::span<const KnownOutput> knownOutputs = {});

} // namespace hfg::v2
