#include "TestHarness.hpp"

#include "v2/model/PresentationHandoff.hpp"

#include <array>
#include <string>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

Target layer(std::string namespaceName = "hgs:bar:DP-1") {
    return {
        .id = "bar",
        .kind = TargetKind::Layer,
        .material = {.source = MaterialSource::Session, .name = "glass"},
        .shape = RoundedRectShape{18.0},
        .selector = LayerSelector{.namespaceName = std::move(namespaceName)},
        .geometry = Rect{0.0, 0.0, 800.0, 44.0},
        .stage = std::nullopt,
        .transition = std::nullopt,
        .enabled = true,
    };
}

struct Fixture {
    TargetIdentity identity{.owner = "client:shell:s1", .targetId = "bar"};
    PresentationKey key{
        .identity = identity,
        .output = "DP-1",
        .outputGeneration = 7,
        .stage = RenderStage::PostLayer,
    };
    SessionSnapshot current{
        .owner = identity.owner,
        .clientId = "shell",
        .mode = SessionMode::Client,
        .generation = 3,
        .expiresAtMs = 10'000,
        .transitionAnchorMs = 0,
        .materials = {{"glass", Material{}}},
        .targets = {layer()},
    };
    SessionReplacement replacement{
        .generation = 4,
        .materials = {{"glass", Material{}}},
        .targets = {layer()},
        .handoffs = {{
            .targetId = "bar",
            .sourceGeneration = 3,
            .timeoutMs = 500,
        }},
    };
    ReadinessTracker readiness;

    Fixture() {
        require(readiness.accept(identity).hasValue(), "target accept failed");
        require(readiness.resolvePresentation(key).hasValue(),
                "presentation resolve failed");
        require(readiness.transition(key, ReadinessState::Attached).hasValue(),
                "presentation attach failed");
        require(readiness.transition(key, ReadinessState::CaptureReady).hasValue(),
                "presentation capture failed");
        require(readiness.transition(key, ReadinessState::Drawn).hasValue(),
                "presentation draw failed");
    }
};

PreparedPresentationHandoff prepare(Fixture& fixture,
                                    PresentationHandoffTracker& tracker) {
    auto prepared = tracker.prepare(fixture.current, fixture.replacement,
                                    fixture.readiness);
    require(prepared.hasValue() && prepared.value().size() == 1U,
            "valid handoff was not prepared");
    return prepared.value().front();
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"exact drawn layer presentation is retained", [] {
            Fixture fixture;
            PresentationHandoffTracker tracker;
            const auto prepared = prepare(fixture, tracker);
            tracker.commit(fixture.current.owner, 4, std::array{prepared}, 100);
            const auto record = tracker.target(fixture.identity);
            require(record && record->sourceGeneration == 3 &&
                        record->successorGeneration == 4 &&
                        record->expiresAtMs == 600 &&
                        record->presentations.size() == 1U &&
                        record->presentations.front().state ==
                            PresentationHandoffState::Retained,
                    "retained handoff record is incomplete");
            require(fixture.readiness.presentation(fixture.key)->state ==
                        ReadinessState::Drawn,
                    "handoff mutated authoritative readiness");
        }},
        Case{"successor draw retires only its fallback", [] {
            Fixture fixture;
            PresentationHandoffTracker tracker;
            const auto prepared = prepare(fixture, tracker);
            tracker.commit(fixture.current.owner, 4, std::array{prepared}, 100);
            tracker.complete(fixture.key);
            const auto record = tracker.target(fixture.identity);
            require(record && record->presentations.front().state ==
                                  PresentationHandoffState::Completed &&
                        tracker.active().empty(),
                    "successful successor draw left a retained fallback active");
        }},
        Case{"timeout fails retained presentation", [] {
            Fixture fixture;
            PresentationHandoffTracker tracker;
            const auto prepared = prepare(fixture, tracker);
            tracker.commit(fixture.current.owner, 4, std::array{prepared}, 100);
            tracker.expire(599);
            require(tracker.active().size() == 1U,
                    "handoff expired before its deadline");
            tracker.expire(600);
            const auto record = tracker.target(fixture.identity);
            require(record && record->presentations.front().state ==
                                  PresentationHandoffState::Failed &&
                        tracker.active().empty(),
                    "handoff timeout did not retire retained state");
        }},
        Case{"output or surface invalidation fails retained presentation", [] {
            Fixture fixture;
            PresentationHandoffTracker tracker;
            const auto prepared = prepare(fixture, tracker);
            tracker.commit(fixture.current.owner, 4, std::array{prepared}, 100);
            tracker.fail(fixture.key, "output invalidated");
            require(tracker.active().empty(),
                    "output invalidation left a retained fallback active");
        }},
        Case{"session and renderer cleanup release retained state", [] {
            Fixture fixture;
            PresentationHandoffTracker tracker;
            const auto prepared = prepare(fixture, tracker);
            tracker.commit(fixture.current.owner, 4, std::array{prepared}, 100);
            tracker.eraseOwner(fixture.current.owner);
            require(!tracker.target(fixture.identity),
                    "session cleanup left a handoff record");
            tracker.commit(fixture.current.owner, 4, std::array{prepared}, 100);
            tracker.clear();
            require(tracker.active().empty() && !tracker.target(fixture.identity),
                    "renderer cleanup left retained resources");
        }},
        Case{"stale source generation is rejected", [] {
            Fixture fixture;
            fixture.replacement.handoffs.front().sourceGeneration = 2;
            PresentationHandoffTracker tracker;
            const auto result = tracker.prepare(fixture.current,
                                                fixture.replacement,
                                                fixture.readiness);
            require(!result && result.error().code == ErrorCode::StaleGeneration,
                    "stale source generation was accepted");
        }},
        Case{"non-drawn predecessor is rejected", [] {
            Fixture fixture;
            require(fixture.readiness.transition(
                        fixture.key, ReadinessState::Attached).hasValue(),
                    "failed to reset predecessor state");
            PresentationHandoffTracker tracker;
            const auto result = tracker.prepare(fixture.current,
                                                fixture.replacement,
                                                fixture.readiness);
            require(!result && result.error().code == ErrorCode::UnresolvedTarget,
                    "non-drawn predecessor was retained");
        }},
        Case{"changed surface identity and unsupported timeout are rejected", [] {
            Fixture fixture;
            PresentationHandoffTracker tracker;
            fixture.replacement.targets.front() = layer("hgs:bar:DP-2");
            require(!tracker.prepare(fixture.current, fixture.replacement,
                                     fixture.readiness),
                    "different layer namespace was retained");
            fixture.replacement.targets.front() = layer();
            fixture.replacement.handoffs.front().timeoutMs = 0;
            require(!tracker.prepare(fixture.current, fixture.replacement,
                                     fixture.readiness),
                    "zero handoff timeout was accepted");
        }},
    });
}
