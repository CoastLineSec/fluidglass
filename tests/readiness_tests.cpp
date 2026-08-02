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
        Case{"a drawn presentation can fail out of drawn", [] {
            ReadinessTracker tracker;
            require(tracker.accept(identity()).hasValue(), "accept failed");
            const auto key = presentation();
            require(tracker.resolvePresentation(key).hasValue() &&
                        tracker.transition(key, ReadinessState::Attached)
                            .hasValue() &&
                        tracker.transition(key, ReadinessState::CaptureReady)
                            .hasValue() &&
                        tracker.transition(key, ReadinessState::Drawn)
                            .hasValue(),
                    "march to drawn failed");
            // A decoration that dies after its presentation drew must be able
            // to take the record with it; a rejected transition would leave
            // the record claiming drawn forever.
            const auto failed = tracker.transition(
                key, ReadinessState::Unresolved, "decoration detached");
            require(failed.hasValue() &&
                        failed.value().state == ReadinessState::Unresolved,
                    "drawn presentation could not fail out of drawn");
            const auto recovered = tracker.transition(
                key, ReadinessState::Resolved, "");
            require(recovered.hasValue(),
                    "failed presentation could not re-resolve");
        }},
        Case{"known outputs keep liveness rows without presentations", [] {
            ReadinessTracker tracker;
            const std::vector<KnownOutput> known{
                {.name = "DP-1", .generation = 4},
                {.name = "DP-2", .generation = 7},
            };
            // Nothing planned anywhere: both rows still exist, all zero, so a
            // client can tell "output present, no glass expected" apart from
            // "output vanished".
            auto rows = outputGlassLiveness(tracker, known);
            require(rows.size() == 2U, "known outputs did not seed rows");
            require(rows[0].output == "DP-1" && !rows[0].drawing &&
                        rows[0].drawn == 0 && rows[0].awaiting == 0 &&
                        rows[0].failed == 0 && rows[0].inactive == 0 &&
                        rows[0].outputGeneration == 4,
                    "empty known output row is wrong");

            // A drawn presentation on one output must not disturb the other
            // row, and an output the catalog has not reported yet still
            // surfaces through its presentation.
            require(tracker.accept(identity()).hasValue(), "accept failed");
            const auto key = presentation("DP-2");
            require(tracker.resolvePresentation(key).hasValue() &&
                        tracker.transition(key, ReadinessState::Attached)
                            .hasValue() &&
                        tracker.transition(key, ReadinessState::CaptureReady)
                            .hasValue() &&
                        tracker.transition(key, ReadinessState::Drawn)
                            .hasValue(),
                    "presentation did not reach drawn");
            rows = outputGlassLiveness(tracker, known);
            require(rows.size() == 2U && rows[1].output == "DP-2" &&
                        rows[1].drawing && rows[1].drawn == 1 &&
                        !rows[0].drawing,
                    "seeded rows did not aggregate presentations");
        }},
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
        Case{"inactivity is a target state a client can observe", [] {
            ReadinessTracker tracker;
            require(tracker.accept(identity()).hasValue(), "target was not accepted");
            const auto inactive = tracker.failTarget(
                identity(),
                ReadinessState::Inactive,
                std::string(targetInactiveReasonDetail(
                    TargetInactiveReason::Offscreen)));
            require(inactive.hasValue(), "inactive was rejected as a target state");
            require(inactive.value().state == ReadinessState::Inactive,
                    "inactive state was not stored");
            require(inactive.value().detail == "target intersects no current output",
                    "inactive target did not carry a reason");
            require(readinessStateName(ReadinessState::Inactive) == "inactive",
                    "inactive state has no wire name");
        }},
        Case{"an inactive target recovers when it becomes drawable", [] {
            ReadinessTracker tracker;
            require(tracker.accept(identity()).hasValue(), "target was not accepted");
            require(tracker.failTarget(identity(), ReadinessState::Inactive, "offscreen").hasValue(),
                    "inactive was rejected as a target state");
            require(tracker.accept(identity()).hasValue(), "inactive target could not be re-accepted");
            require(tracker.resolvePresentation(presentation()).hasValue(),
                    "presentation could not resolve after inactivity");
            require(tracker.transition(presentation(), ReadinessState::Attached).hasValue(),
                    "presentation could not attach after inactivity");
        }},

        // Per-output liveness. This is what replaces per-target readiness as a
        // client contract: a client that derives no geometry needs only to know
        // whether glass is drawing on the output its surface sits on.
        Case{"an output with a drawn presentation is drawing", [] {
            ReadinessTracker readiness;
            static_cast<void>(readiness.accept(identity()));
            static_cast<void>(readiness.resolvePresentation(presentation()));
            static_cast<void>(readiness.transition(presentation(), ReadinessState::Attached));
            static_cast<void>(readiness.transition(presentation(), ReadinessState::CaptureReady));
            static_cast<void>(readiness.transition(presentation(), ReadinessState::Drawn));

            const auto liveness = outputGlassLiveness(readiness);
            require(liveness.size() == 1, "expected one output");
            require(liveness[0].output == "DP-1", "wrong output name");
            require(liveness[0].drawing, "a drawn presentation means drawing");
            require(liveness[0].drawn == 1, "wrong drawn count");
        }},
        Case{"an output that has resolved but not drawn is NOT drawing", [] {
            // The safety property: a client must stay on its neutral material
            // until glass is confirmed, or it goes transparent over nothing.
            ReadinessTracker readiness;
            static_cast<void>(readiness.accept(identity()));
            static_cast<void>(readiness.resolvePresentation(presentation()));

            const auto liveness = outputGlassLiveness(readiness);
            require(liveness.size() == 1, "expected one output");
            require(!liveness[0].drawing, "resolved is not drawn");
            require(liveness[0].awaiting == 1, "should be awaiting");
        }},
        Case{"a capture failure on one output leaves the others drawing", [] {
            // Capture resources are owned by an output generation, so this is
            // exactly the failure boundary per-output liveness exists for.
            ReadinessTracker readiness;
            static_cast<void>(readiness.accept(identity()));
            const auto good = presentation("DP-1");
            const auto bad = presentation("DP-2");
            static_cast<void>(readiness.resolvePresentation(good));
            static_cast<void>(readiness.transition(good, ReadinessState::Attached));
            static_cast<void>(readiness.transition(good, ReadinessState::CaptureReady));
            static_cast<void>(readiness.transition(good, ReadinessState::Drawn));
            static_cast<void>(readiness.resolvePresentation(bad));
            static_cast<void>(readiness.transition(bad, ReadinessState::Attached));
            static_cast<void>(readiness.transition(bad, ReadinessState::CaptureFailed));

            const auto liveness = outputGlassLiveness(readiness);
            require(liveness.size() == 2, "expected two outputs");
            require(liveness[0].output == "DP-1" && liveness[0].drawing,
                    "the healthy output should still be drawing");
            require(liveness[1].output == "DP-2" && !liveness[1].drawing,
                    "the failed output should not be drawing");
            require(liveness[1].failed == 1, "the failure should be counted");
        }},
        Case{"an inactive presentation is neither drawing nor failing", [] {
            // Resolved, and will never draw as published. Counting it as a
            // failure would report a fault that does not exist; counting it as
            // awaiting is what made this state invisible before.
            ReadinessTracker readiness;
            static_cast<void>(readiness.accept(identity()));
            static_cast<void>(readiness.resolvePresentation(presentation()));
            static_cast<void>(readiness.failTarget(
                identity(), ReadinessState::Inactive, "target is disabled"));

            const auto liveness = outputGlassLiveness(readiness);
            require(liveness.size() == 1, "expected one output");
            require(!liveness[0].drawing, "inactive is not drawing");
            require(liveness[0].inactive == 1, "should be counted inactive");
            require(liveness[0].failed == 0, "inactive is not a failure");
        }},
        Case{"an output with no presentations at all is not reported", [] {
            ReadinessTracker readiness;
            static_cast<void>(readiness.accept(identity()));
            require(outputGlassLiveness(readiness).empty(),
                    "a target with no presentations names no output");
        }},
        Case{"the newest output generation wins the label", [] {
            ReadinessTracker readiness;
            static_cast<void>(readiness.accept(identity()));
            static_cast<void>(readiness.resolvePresentation(presentation("DP-1", 1)));
            static_cast<void>(readiness.resolvePresentation(presentation("DP-1", 4)));

            const auto liveness = outputGlassLiveness(readiness);
            require(liveness.size() == 1, "same output, one entry");
            require(liveness[0].outputGeneration == 4,
                    "a rebuilt output should report its newest generation");
        }},
    });
}
