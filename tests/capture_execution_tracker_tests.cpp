#include "TestHarness.hpp"

#include "v2/render/CaptureExecutionTracker.hpp"

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

int main() {
    return hfg::test::run({
        Case{"capture is not ready until the scheduled frame completes", [] {
            CaptureExecutionTracker tracker;
            require(tracker.schedule(4, 8).hasValue(), "schedule failed");
            require(!tracker.ready(4, 8), "scheduled capture was reported ready");
            require(tracker.complete(4, 8).hasValue(), "complete failed");
            require(tracker.ready(4, 8), "completed capture was not ready");
        }},
        Case{"a new frame invalidates an older successful capture", [] {
            CaptureExecutionTracker tracker;
            require(tracker.schedule(4, 8).hasValue(), "first schedule failed");
            require(tracker.complete(4, 8).hasValue(), "first complete failed");
            require(tracker.schedule(4, 9).hasValue(), "second schedule failed");
            require(!tracker.ready(4, 8), "old frame remained ready");
            require(!tracker.ready(4, 9), "new frame was ready before capture");
        }},
        Case{"capture failure prevents drawing", [] {
            CaptureExecutionTracker tracker;
            require(tracker.schedule(5, 2).hasValue(), "schedule failed");
            require(tracker.complete(5, 2).hasValue(), "complete failed");
            tracker.fail(5, 2);
            require(!tracker.ready(5, 2), "failed capture remained ready");
        }},
        Case{"stale completion and zero tokens fail closed", [] {
            CaptureExecutionTracker tracker;
            require(!tracker.schedule(0, 1), "zero resource token was accepted");
            require(!tracker.schedule(1, 0), "zero frame token was accepted");
            require(tracker.schedule(1, 2).hasValue(), "valid schedule failed");
            const auto stale = tracker.complete(1, 3);
            require(!stale && stale.error().code == ErrorCode::StaleGeneration,
                    "stale completion was accepted");
        }},
        Case{"retire and clear remove capture readiness", [] {
            CaptureExecutionTracker tracker;
            require(tracker.schedule(1, 2).hasValue(), "first schedule failed");
            require(tracker.complete(1, 2).hasValue(), "first complete failed");
            tracker.retire(1);
            require(!tracker.ready(1, 2), "retired capture remained ready");
            require(tracker.schedule(2, 3).hasValue(), "second schedule failed");
            require(tracker.complete(2, 3).hasValue(), "second complete failed");
            tracker.clear();
            require(!tracker.ready(2, 3), "cleared capture remained ready");
        }},
    });
}
