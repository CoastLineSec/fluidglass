#include "TestHarness.hpp"

#include "v2/model/VisibilityTransition.hpp"

#include <optional>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

Target layer() {
    return {
        .id = "bar",
        .kind = TargetKind::Layer,
        .material = {.source = MaterialSource::Session, .name = "glass"},
        .shape = RoundedRectShape{18.0},
        .selector = LayerSelector{.namespaceName = "hgs:dash1:DP-2"},
        .geometry = Rect{12.0, 8.0, 776.0, 44.0},
        .stage = std::nullopt,
        .transition = std::nullopt,
        .enabled = true,
    };
}

struct Fixture {
    SessionSnapshot previous{
        .owner = "client:shell:s1",
        .clientId = "shell",
        .mode = SessionMode::Client,
        .generation = 4,
        .expiresAtMs = 10'000,
        .transitionAnchorMs = 0,
        .materials = {{"glass", Material{}}},
        .targets = {layer()},
    };
    SessionReplacement replacement{
        .generation = 5,
        .materials = {{"glass", Material{}}},
        .targets = {layer()},
        .handoffs = {},
        .visibilityTransitions = {{
            .targetId = "bar",
            .transitionId = "hide-1",
            .sourceGeneration = 4,
            .direction = VisibilityTransitionDirection::Hide,
            .edge = TransitionEdge::Top,
            .sourceRect = {12.0, 8.0, 776.0, 44.0},
            .sourceRadius = 18.0,
            .travel = 52.0,
            .durationMs = 200,
            .timeoutMs = 750,
            .output = "DP-2",
            .namespaceName = "hgs:dash1:DP-2",
        }},
    };
    TargetIdentity identity{
        .owner = previous.owner,
        .targetId = "bar",
    };
    PresentationKey key{
        .identity = identity,
        .output = "DP-2",
        .outputGeneration = 7,
        .stage = RenderStage::PostLayer,
    };
};

void prepareAndCommit(
    Fixture& fixture,
    VisibilityTransitionTracker& tracker,
    std::uint64_t nowMs = 100) {
    auto prepared = tracker.prepare(
        fixture.previous,
        fixture.replacement,
        fixture.previous.owner,
        nowMs);
    require(prepared.hasValue() && prepared.value().size() == 1,
            "valid transition was not prepared");
    tracker.commit(std::move(prepared.value()));
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"successor is armed without changing readiness", [] {
            Fixture fixture;
            VisibilityTransitionTracker tracker;
            prepareAndCommit(fixture, tracker);
            const auto record = tracker.target(fixture.identity);
            require(record &&
                        record->state == VisibilityTransitionState::Armed &&
                        record->anchorMs == 0,
                    "transition was not armed before draw");
        }},
        Case{"first successful draw supplies the authoritative anchor", [] {
            Fixture fixture;
            VisibilityTransitionTracker tracker;
            prepareAndCommit(fixture, tracker);
            require(tracker.bind(fixture.identity, "DP-2", 7, 99),
                    "live presentation did not bind");
            require(tracker.activate(fixture.key, 140),
                    "first draw did not activate transition");
            const auto record = tracker.target(fixture.identity);
            require(record &&
                        record->state == VisibilityTransitionState::Active &&
                        record->anchorMs == 140,
                    "draw anchor was not retained");
        }},
        Case{"hide travels outward and reaches transparent completion", [] {
            Fixture fixture;
            VisibilityTransitionTracker tracker;
            prepareAndCommit(fixture, tracker);
            tracker.bind(fixture.identity, "DP-2", 7, 99);
            tracker.activate(fixture.key, 100);
            const auto start = tracker.sample(fixture.identity, 100);
            const auto end = tracker.sample(fixture.identity, 300);
            require(start && end &&
                        start.value().offset.y == 0.0 &&
                        start.value().opacity == 1.0 &&
                        end.value().offset.y == -52.0 &&
                        end.value().opacity == 0.0 &&
                        !end.value().active,
                    "hide endpoints do not express outward visibility");
        }},
        Case{"reversal snapshots current compositor-visible state", [] {
            Fixture fixture;
            VisibilityTransitionTracker tracker;
            prepareAndCommit(fixture, tracker);
            tracker.bind(fixture.identity, "DP-2", 7, 99);
            tracker.activate(fixture.key, 100);
            const auto current = tracker.sample(fixture.identity, 150);
            fixture.previous.generation = 5;
            fixture.replacement.generation = 6;
            fixture.replacement.visibilityTransitions.front()
                .sourceGeneration = 5;
            fixture.replacement.visibilityTransitions.front()
                .transitionId = "reveal-2";
            fixture.replacement.visibilityTransitions.front().direction =
                VisibilityTransitionDirection::Reveal;
            auto prepared = tracker.prepare(
                fixture.previous,
                fixture.replacement,
                fixture.previous.owner,
                150);
            require(prepared.hasValue(), "reversal was rejected");
            tracker.commit(std::move(prepared.value()));
            const auto reversed = tracker.sample(fixture.identity, 150);
            const auto record = tracker.target(fixture.identity);
            require(current && reversed &&
                        record &&
                        current.value().offset == reversed.value().offset &&
                        current.value().opacity == reversed.value().opacity &&
                        record->startingProgress > 0.0 &&
                        record->startingProgress < 1.0,
                    "reversal jumped to an endpoint");
        }},
        Case{"surface or output lifetime changes fail closed", [] {
            Fixture fixture;
            VisibilityTransitionTracker tracker;
            prepareAndCommit(fixture, tracker);
            require(tracker.bind(fixture.identity, "DP-2", 7, 99),
                    "initial bind failed");
            require(!tracker.bind(fixture.identity, "DP-2", 8, 99),
                    "new output generation was accepted");
            require(tracker.target(fixture.identity)->state ==
                        VisibilityTransitionState::Failed,
                    "lifetime mismatch did not fail transition");
        }},
        Case{"timeout and owner cleanup release records", [] {
            Fixture fixture;
            VisibilityTransitionTracker tracker;
            prepareAndCommit(fixture, tracker);
            tracker.expire(851);
            require(tracker.target(fixture.identity)->state ==
                        VisibilityTransitionState::Failed,
                    "armed transition did not time out");
            tracker.eraseOwner(fixture.previous.owner);
            require(!tracker.target(fixture.identity),
                    "owner cleanup retained transition");
        }},
        Case{"invalid source generation is rejected", [] {
            Fixture fixture;
            VisibilityTransitionTracker tracker;
            fixture.replacement.visibilityTransitions.front()
                .sourceGeneration = 3;
            const auto result = tracker.prepare(
                fixture.previous,
                fixture.replacement,
                fixture.previous.owner,
                100);
            require(!result.hasValue() &&
                        result.error().code == ErrorCode::StaleGeneration,
                    "stale source generation was accepted");
        }},
        Case{"invalid target identity is rejected", [] {
            Fixture fixture;
            VisibilityTransitionTracker tracker;
            fixture.replacement.visibilityTransitions.front().targetId =
                "missing";
            const auto result = tracker.prepare(
                fixture.previous,
                fixture.replacement,
                fixture.previous.owner,
                100);
            require(!result.hasValue() &&
                        result.error().code == ErrorCode::InvalidTarget,
                    "missing successor target was accepted");
        }},
    });
}
