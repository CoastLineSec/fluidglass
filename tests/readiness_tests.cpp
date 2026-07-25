#include "TestHarness.hpp"

#include "v2/model/Readiness.hpp"

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

TargetIdentity identity() {
    return {.owner = "client:shell:one", .targetId = "bar"};
}

PresentationKey presentation(std::string output = "DP-1", std::uint64_t generation = 1) {
    return {
        .identity = identity(),
        .output = std::move(output),
        .outputGeneration = generation,
        .stage = RenderStage::PostLayer,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"canonical readiness sequence", [] {
            ReadinessTracker tracker;
            require(tracker.accept(identity()).hasValue(), "target was not accepted");
            const auto key = presentation();
            require(tracker.resolvePresentation(key).hasValue(), "presentation was not resolved");
            require(tracker.transition(key, ReadinessState::Attached).hasValue(), "attach failed");
            require(tracker.transition(key, ReadinessState::CaptureReady).hasValue(), "capture-ready failed");
            const auto drawn = tracker.transition(key, ReadinessState::Drawn);
            require(drawn.hasValue(), "drawn transition failed");
            require(drawn.value().state == ReadinessState::Drawn, "drawn state changed");
        }},
        Case{"drawn requires capture-ready", [] {
            ReadinessTracker tracker;
            require(tracker.accept(identity()).hasValue(), "target was not accepted");
            const auto key = presentation();
            require(tracker.resolvePresentation(key).hasValue(), "presentation was not resolved");
            require(tracker.transition(key, ReadinessState::Attached).hasValue(), "attach failed");
            const auto result = tracker.transition(key, ReadinessState::Drawn);
            require(!result, "attached presentation claimed drawn");
        }},
        Case{"presentations are output-generation scoped", [] {
            ReadinessTracker tracker;
            require(tracker.accept(identity()).hasValue(), "target was not accepted");
            const auto first = presentation("DP-1", 1);
            const auto second = presentation("DP-1", 2);
            require(tracker.resolvePresentation(first).hasValue(), "first presentation was not resolved");
            require(tracker.transition(first, ReadinessState::Attached).hasValue(), "first attach failed");
            require(tracker.transition(first, ReadinessState::CaptureReady).hasValue(), "first capture failed");
            require(tracker.transition(first, ReadinessState::Drawn).hasValue(), "first draw failed");
            require(tracker.resolvePresentation(second).hasValue(), "second presentation was not resolved");
            require(tracker.presentation(first)->state == ReadinessState::Drawn, "old generation state changed");
            require(tracker.presentation(second)->state == ReadinessState::Resolved, "new generation inherited readiness");
        }},
        Case{"multi-output presentations are independent", [] {
            ReadinessTracker tracker;
            require(tracker.accept(identity()).hasValue(), "target was not accepted");
            const auto left = presentation("DP-1");
            const auto right = presentation("HDMI-A-1");
            require(tracker.resolvePresentation(left).hasValue(), "left presentation was not resolved");
            require(tracker.resolvePresentation(right).hasValue(), "right presentation was not resolved");
            require(tracker.transition(left, ReadinessState::Attached).hasValue(), "left attach failed");
            require(tracker.transition(left, ReadinessState::CaptureReady).hasValue(), "left capture failed");
            require(tracker.transition(left, ReadinessState::Drawn).hasValue(), "left draw failed");
            require(tracker.presentation(left)->state == ReadinessState::Drawn, "left output did not draw");
            require(tracker.presentation(right)->state == ReadinessState::Resolved, "right output inherited left readiness");
        }},
        Case{"capture failure can recover", [] {
            ReadinessTracker tracker;
            require(tracker.accept(identity()).hasValue(), "target was not accepted");
            const auto key = presentation();
            require(tracker.resolvePresentation(key).hasValue(), "presentation was not resolved");
            require(tracker.transition(key, ReadinessState::Attached).hasValue(), "attach failed");
            require(tracker.transition(key, ReadinessState::CaptureFailed, "allocation failed").hasValue(),
                    "capture failure was not recorded");
            require(tracker.transition(key, ReadinessState::CaptureReady).hasValue(), "capture failure did not recover");
            require(tracker.transition(key, ReadinessState::Drawn).hasValue(), "recovered presentation did not draw");
        }},
        Case{"target failure updates all presentations", [] {
            ReadinessTracker tracker;
            require(tracker.accept(identity()).hasValue(), "target was not accepted");
            const auto left = presentation("DP-1");
            const auto right = presentation("HDMI-A-1");
            require(tracker.resolvePresentation(left).hasValue(), "left presentation was not resolved");
            require(tracker.resolvePresentation(right).hasValue(), "right presentation was not resolved");
            require(tracker.failTarget(identity(), ReadinessState::Expired, "lease expired").hasValue(),
                    "target expiry was not recorded");
            require(tracker.target(identity())->state == ReadinessState::Expired, "target did not expire");
            require(tracker.presentation(left)->state == ReadinessState::Expired, "left presentation did not expire");
            require(tracker.presentation(right)->state == ReadinessState::Expired, "right presentation did not expire");
        }},
        Case{"accepting a replacement clears old presentations", [] {
            ReadinessTracker tracker;
            require(tracker.accept(identity()).hasValue(), "target was not accepted");
            require(tracker.resolvePresentation(presentation()).hasValue(), "presentation was not resolved");
            require(tracker.presentations(identity()).size() == 1, "presentation missing");
            require(tracker.accept(identity()).hasValue(), "replacement was not accepted");
            require(tracker.presentations(identity()).empty(), "replacement kept stale presentation");
        }},
        Case{"sequence increases on observable change", [] {
            ReadinessTracker tracker;
            const auto accepted = tracker.accept(identity()).value();
            const auto resolved = tracker.resolvePresentation(presentation()).value();
            const auto attached = tracker.transition(presentation(), ReadinessState::Attached).value();
            require(accepted.sequence < resolved.sequence && resolved.sequence < attached.sequence,
                    "readiness sequence did not increase");
        }},
        Case{"unaccepted target cannot resolve", [] {
            ReadinessTracker tracker;
            require(!tracker.resolvePresentation(presentation()), "unaccepted target resolved");
            require(!tracker.failTarget(identity(), ReadinessState::Invalid), "unaccepted target failed");
        }},
    });
}
