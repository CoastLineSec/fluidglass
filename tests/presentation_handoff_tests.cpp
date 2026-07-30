#include "TestHarness.hpp"

#include "v2/model/PresentationHandoff.hpp"

#include <array>
#include <string>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

Target layer(
    std::string namespaceName = "hgs:bar:DP-1",
    Rect geometry = {0.0, 0.0, 800.0, 44.0},
    double radius = 18.0) {
    return {
        .id = "bar",
        .kind = TargetKind::Layer,
        .material = {.source = MaterialSource::Session, .name = "glass"},
        .shape = RoundedRectShape{radius},
        .selector = LayerSelector{.namespaceName = std::move(namespaceName)},
        .geometry = geometry,
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
        Case{"morph accepts authoritative endpoints and compositor anchor", [] {
            Fixture fixture;
            fixture.current.targets.front() = layer(
                "hgs:bar:DP-1", {12.0, 8.0, 776.0, 44.0}, 18.0);
            fixture.replacement.targets.front() = layer(
                "hgs:bar:DP-1", {0.0, 0.0, 800.0, 44.0}, 0.0);
            fixture.replacement.handoffs.front().morph =
                PresentationHandoffRequest::Morph{
                    .transitionId = "attach-1",
                    .durationMs = 240,
                };
            PresentationHandoffTracker tracker;
            auto prepared = tracker.prepare(
                fixture.current, fixture.replacement, fixture.readiness, 1'000);
            require(prepared && prepared.value().front().morph,
                    "valid geometry morph was rejected");
            tracker.commit(
                fixture.current.owner, 4, prepared.value(), 1'000);
            const auto record = tracker.target(fixture.identity);
            require(record && record->morph &&
                        record->morph->anchorMs == 1'000 &&
                        record->morph->source.radius == 18.0 &&
                        record->morph->destination.radius == 0.0 &&
                        record->morph->envelope ==
                            Rect{0.0, 0.0, 800.0, 52.0},
                    "accepted morph did not preserve authoritative geometry");
            const auto start =
                resolvePresentationMorph(*record->morph, 1'000);
            const auto middle =
                resolvePresentationMorph(*record->morph, 1'120);
            const auto end =
                resolvePresentationMorph(*record->morph, 1'240);
            require(start && middle && end &&
                        start.value().current == record->morph->source &&
                        middle.value().current.rect.x > 0.0 &&
                        middle.value().current.rect.x < 12.0 &&
                        middle.value().current.radius > 0.0 &&
                        middle.value().current.radius < 18.0 &&
                        end.value().current == record->morph->destination &&
                        !end.value().active,
                    "morph endpoints or monotonic midpoint are incorrect");
        }},
        Case{"reversal starts from compositor-visible geometry", [] {
            Fixture fixture;
            fixture.current.targets.front() = layer(
                "hgs:bar:DP-1", {12.0, 8.0, 776.0, 44.0}, 18.0);
            fixture.replacement.targets.front() = layer(
                "hgs:bar:DP-1", {0.0, 0.0, 800.0, 44.0}, 0.0);
            fixture.replacement.handoffs.front().morph =
                PresentationHandoffRequest::Morph{
                    .transitionId = "attach-1",
                    .durationMs = 240,
                };
            PresentationHandoffTracker tracker;
            auto first = tracker.prepare(
                fixture.current, fixture.replacement, fixture.readiness, 1'000);
            require(first.hasValue(), "first morph preparation failed");
            tracker.commit(fixture.current.owner, 4, first.value(), 1'000);
            const auto active = tracker.target(fixture.identity);
            require(active && active->morph, "active morph was not recorded");
            const auto visible =
                resolvePresentationMorph(*active->morph, 1'080);
            require(visible.hasValue(), "active morph could not be inspected");

            auto current = fixture.current;
            current.generation = 4;
            current.targets = fixture.replacement.targets;
            auto reverse = fixture.replacement;
            reverse.generation = 5;
            reverse.targets = {layer(
                "hgs:bar:DP-1", {12.0, 8.0, 776.0, 44.0}, 18.0)};
            reverse.handoffs.front().sourceGeneration = 4;
            reverse.handoffs.front().morph =
                PresentationHandoffRequest::Morph{
                    .transitionId = "float-2",
                    .durationMs = 240,
                };
            auto prepared = tracker.prepare(
                current, reverse, fixture.readiness, 1'080);
            require(prepared && prepared.value().front().morph &&
                        prepared.value().front().morph->source ==
                            visible.value().current &&
                        prepared.value().front().morph->durationMs > 0U &&
                        prepared.value().front().morph->durationMs < 240U,
                    "reversal jumped back to a stale endpoint");
            tracker.commit(current.owner, 5, prepared.value(), 1'080);
            require(tracker.morphing().size() == 1U &&
                        tracker.morphing().front().morph->transitionId ==
                            "float-2",
                    "superseded morph record was not bounded to one target");
        }},
        Case{"an unchanged target keeps its active morph across a generation", [] {
            Fixture fixture;
            fixture.current.targets.front() = layer(
                "hgs:bar:DP-1", {12.0, 8.0, 776.0, 44.0}, 18.0);
            fixture.replacement.targets.front() = layer(
                "hgs:bar:DP-1", {0.0, 0.0, 800.0, 44.0}, 0.0);
            fixture.replacement.handoffs.front().morph =
                PresentationHandoffRequest::Morph{
                    .transitionId = "attach-1",
                    .durationMs = 240,
                };
            PresentationHandoffTracker tracker;
            auto first = tracker.prepare(
                fixture.current, fixture.replacement, fixture.readiness, 1'000);
            require(first.hasValue(), "first morph preparation failed");
            tracker.commit(fixture.current.owner, 4, first.value(), 1'000);

            auto current = fixture.current;
            current.generation = 4;
            current.targets = fixture.replacement.targets;
            auto unrelated = fixture.replacement;
            unrelated.generation = 5;
            unrelated.handoffs.front().sourceGeneration = 4;
            unrelated.handoffs.front().morph.reset();
            auto prepared = tracker.prepare(
                current, unrelated, fixture.readiness, 1'080);
            require(prepared &&
                        prepared.value().front().preserveActiveMorph,
                    "unchanged target did not preserve its active morph");
            tracker.commit(current.owner, 5, prepared.value(), 1'080);
            const auto continued = tracker.target(fixture.identity);
            require(continued && continued->morph &&
                        continued->successorGeneration == 5 &&
                        continued->morph->transitionId == "attach-1" &&
                        continued->morph->anchorMs == 1'000,
                    "unrelated generation restarted or canceled active motion");
        }},
        Case{"output-local morph accepts explicit monitor endpoints", [] {
            Fixture fixture;
            fixture.current.targets.front() = layer(
                "hgs:bar:DP-1", {12.0, 8.0, 776.0, 44.0}, 18.0);
            fixture.replacement.targets.front() = layer(
                "hgs:bar:DP-1", {0.0, 0.0, 800.0, 44.0}, 0.0);
            fixture.replacement.handoffs.front().morph =
                PresentationHandoffRequest::Morph{
                    .transitionId = "bottom-attach",
                    .durationMs = 240,
                    .coordinateSpace =
                        PresentationHandoffRequest::MorphCoordinateSpace::
                            OutputLocal,
                    .source = PresentationHandoffRequest::MorphEndpoint{
                        .rect = {12.0, 48.0, 776.0, 44.0},
                        .radius = 18.0,
                    },
                    .destination =
                        PresentationHandoffRequest::MorphEndpoint{
                            .rect = {0.0, 56.0, 800.0, 44.0},
                            .radius = 0.0,
                        },
                };
            PresentationHandoffTracker tracker;
            auto prepared = tracker.prepare(
                fixture.current, fixture.replacement,
                fixture.readiness, 1'000);
            require(prepared && prepared.value().front().morph &&
                        prepared.value().front().morph->coordinateSpace ==
                            PresentationHandoffRequest::
                                MorphCoordinateSpace::OutputLocal &&
                        prepared.value().front().morph->source.rect ==
                            Rect{12.0, 48.0, 776.0, 44.0} &&
                        prepared.value().front().morph->destination.rect ==
                            Rect{0.0, 56.0, 800.0, 44.0},
                    "output-local endpoints were not preserved");
            tracker.commit(
                fixture.current.owner, 4, prepared.value(), 1'000);
            tracker.expire(1'240);
            const auto settling = tracker.target(fixture.identity);
            require(settling && settling->morph &&
                        settling->morph->state ==
                            PresentationMorphState::Settling &&
                        tracker.morphing().size() == 1U,
                    "output-local endpoint override retired before settlement");
            auto current = fixture.current;
            current.generation = 4;
            current.targets = fixture.replacement.targets;
            auto unrelated = fixture.replacement;
            unrelated.generation = 5;
            unrelated.handoffs.front().sourceGeneration = 4;
            unrelated.handoffs.front().morph.reset();
            auto preserved = tracker.prepare(
                current, unrelated, fixture.readiness, 1'250);
            require(preserved &&
                        preserved.value().front().preserveActiveMorph,
                    "settling output-local override was not preserved");
            tracker.commit(
                current.owner, 5, preserved.value(), 1'250);
            const auto continued = tracker.target(fixture.identity);
            require(continued && continued->morph &&
                        continued->successorGeneration == 5 &&
                        continued->morph->state ==
                            PresentationMorphState::Settling &&
                        continued->morph->transitionId == "bottom-attach",
                    "unrelated generation retired the settling override");
            tracker.settleMorph(fixture.identity);
            require(tracker.target(fixture.identity)->morph->state ==
                        PresentationMorphState::Completed &&
                        tracker.morphing().empty(),
                    "settled output-local override was not retired");
        }},
        Case{"output-local morph endpoints are strict and target-sized", [] {
            Fixture fixture;
            fixture.current.targets.front() = layer(
                "hgs:bar:DP-1", {12.0, 8.0, 776.0, 44.0}, 18.0);
            fixture.replacement.targets.front() = layer(
                "hgs:bar:DP-1", {0.0, 0.0, 800.0, 44.0}, 0.0);
            fixture.replacement.handoffs.front().morph =
                PresentationHandoffRequest::Morph{
                    .transitionId = "bottom-attach",
                    .durationMs = 240,
                    .coordinateSpace =
                        PresentationHandoffRequest::MorphCoordinateSpace::
                            OutputLocal,
                };
            PresentationHandoffTracker tracker;
            require(!tracker.prepare(
                        fixture.current, fixture.replacement,
                        fixture.readiness, 1'000),
                    "missing output-local endpoints were accepted");
            fixture.replacement.handoffs.front().morph->source =
                PresentationHandoffRequest::MorphEndpoint{
                    .rect = {12.0, 48.0, 775.0, 44.0},
                    .radius = 18.0,
                };
            fixture.replacement.handoffs.front().morph->destination =
                PresentationHandoffRequest::MorphEndpoint{
                    .rect = {0.0, 56.0, 800.0, 44.0},
                    .radius = 0.0,
                };
            require(!tracker.prepare(
                        fixture.current, fixture.replacement,
                        fixture.readiness, 1'000),
                    "endpoint size inconsistent with its target was accepted");
        }},
        Case{"output-local settlement remains bounded by handoff timeout", [] {
            Fixture fixture;
            fixture.replacement.handoffs.front().morph =
                PresentationHandoffRequest::Morph{
                    .transitionId = "right-attach",
                    .durationMs = 200,
                    .coordinateSpace =
                        PresentationHandoffRequest::MorphCoordinateSpace::
                            OutputLocal,
                    .source = PresentationHandoffRequest::MorphEndpoint{
                        .rect = {48.0, 0.0, 800.0, 44.0},
                        .radius = 18.0,
                    },
                    .destination =
                        PresentationHandoffRequest::MorphEndpoint{
                            .rect = {56.0, 0.0, 800.0, 44.0},
                            .radius = 18.0,
                        },
                };
            PresentationHandoffTracker tracker;
            auto prepared = tracker.prepare(
                fixture.current, fixture.replacement,
                fixture.readiness, 100);
            require(prepared.hasValue(), "bounded output-local morph failed");
            tracker.commit(
                fixture.current.owner, 4, prepared.value(), 100);
            tracker.expire(300);
            require(tracker.morphing().size() == 1U,
                    "settlement override disappeared at animation completion");
            tracker.expire(600);
            require(tracker.morphing().empty() &&
                        tracker.target(fixture.identity)->morph->state ==
                            PresentationMorphState::Failed,
                    "unsettled output-local override survived its lease");
        }},
        Case{"morph rejects unsupported shape and excessive duration", [] {
            Fixture fixture;
            fixture.replacement.handoffs.front().morph =
                PresentationHandoffRequest::Morph{
                    .transitionId = "attach-1",
                    .durationMs = 1'001,
                };
            PresentationHandoffTracker tracker;
            require(!tracker.prepare(
                        fixture.current, fixture.replacement,
                        fixture.readiness, 100),
                    "excessive morph duration was accepted");
            fixture.replacement.handoffs.front().morph->durationMs = 200;
            fixture.replacement.targets.front().shape = RingShape{
                .outerRadius = 18.0,
                .thickness = 2.0,
            };
            require(!tracker.prepare(
                        fixture.current, fixture.replacement,
                        fixture.readiness, 100),
                    "non-rounded shape morph was accepted");
        }},
    });
}
