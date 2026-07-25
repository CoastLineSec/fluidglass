#include "TestHarness.hpp"

#include "v2/core/Limits.hpp"
#include "v2/render/OutputGeneration.hpp"

#include <array>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

OutputSnapshot output(std::string name = "DP-1") {
    return {
        .name = std::move(name),
        .objectToken = 1,
        .modeToken = 1,
        .bufferWidth = 2400,
        .bufferHeight = 1350,
        .logicalX = 0.0,
        .logicalY = 0.0,
        .logicalWidth = 1920.0,
        .logicalHeight = 1080.0,
        .scale = 1.25,
        .transform = OutputTransform::Normal,
        .renderFormat = 0x34325241U,
        .colorStateToken = 1,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"first snapshot creates a generation", [] {
            OutputGenerationTracker tracker;
            const auto result = tracker.update(output());
            require(result.hasValue(), "valid output snapshot was rejected");
            require(result.value().changed, "first output snapshot was not a change");
            require(result.value().current.generation == 1, "first output generation changed");
            require(!result.value().retired, "first output update retired a generation");
            require(tracker.activeCount() == 1, "active output count changed");
        }},
        Case{"identical snapshot reuses its generation", [] {
            OutputGenerationTracker tracker;
            const auto first = tracker.update(output()).value();
            const auto second = tracker.update(output()).value();
            require(!second.changed, "identical output snapshot created a generation");
            require(second.current == first.current, "identical snapshot changed current generation");
            require(!second.retired, "identical snapshot retired the current generation");
        }},
        Case{"render-relevant changes retire generations", [] {
            OutputGenerationTracker tracker;
            auto snapshot = output();
            require(tracker.update(snapshot).value().current.generation == 1, "initial generation changed");

            snapshot.objectToken = 2;
            auto changed = tracker.update(snapshot).value();
            require(changed.changed && changed.retired->generation == 1, "object replacement did not retire");

            snapshot.modeToken = 2;
            changed = tracker.update(snapshot).value();
            require(changed.current.generation == 3, "mode change did not advance generation");

            snapshot.bufferWidth = 2560;
            changed = tracker.update(snapshot).value();
            require(changed.current.generation == 4, "buffer size change did not advance generation");

            snapshot.logicalX = -1920.0;
            changed = tracker.update(snapshot).value();
            require(changed.current.generation == 5, "layout change did not advance generation");

            snapshot.logicalWidth = 2048.0;
            changed = tracker.update(snapshot).value();
            require(changed.current.generation == 6, "logical size change did not advance generation");

            snapshot.scale = 1.5;
            changed = tracker.update(snapshot).value();
            require(changed.current.generation == 7, "scale change did not advance generation");

            snapshot.transform = OutputTransform::Rotate90;
            changed = tracker.update(snapshot).value();
            require(changed.current.generation == 8, "transform change did not advance generation");

            snapshot.renderFormat = 0x30335241U;
            changed = tracker.update(snapshot).value();
            require(changed.current.generation == 9, "format change did not advance generation");

            snapshot.colorStateToken = 2;
            changed = tracker.update(snapshot).value();
            require(changed.current.generation == 10, "color-state change did not advance generation");
        }},
        Case{"removal retires without reusing identity", [] {
            OutputGenerationTracker tracker;
            const auto first = tracker.update(output()).value().current;
            const auto retired = tracker.remove("DP-1");
            require(retired && *retired == first, "output removal did not return the retired generation");
            require(!tracker.current("DP-1"), "removed output remained current");
            require(tracker.activeCount() == 0, "removed output remained active");

            auto replacement = output();
            replacement.objectToken = 9;
            const auto readded = tracker.update(replacement).value().current;
            require(readded.generation == first.generation + 1, "re-added output reused a retired generation");
        }},
        Case{"clearing active outputs preserves generation history", [] {
            OutputGenerationTracker tracker;
            const auto first = tracker.update(output()).value().current;
            tracker.clearCurrent();
            require(tracker.currents().empty(),
                    "cleared output remained current");
            require(tracker.activeCount() == 0U,
                    "cleared output remained active");

            auto replacement = output();
            replacement.objectToken = 2;
            const auto readded = tracker.update(replacement).value().current;
            require(readded.generation == first.generation + 1,
                    "clear reused a retired generation number");
        }},
        Case{"outputs have independent generation sequences", [] {
            OutputGenerationTracker tracker;
            require(tracker.update(output("DP-1")).value().current.generation == 1,
                    "first output generation changed");
            require(tracker.update(output("HDMI-A-1")).value().current.generation == 1,
                    "second output did not start at generation one");
            auto changed = output("DP-1");
            changed.scale = 2.0;
            require(tracker.update(changed).value().current.generation == 2,
                    "first output generation did not advance independently");
            require(tracker.current("HDMI-A-1")->generation == 1,
                    "second output inherited another output's generation");
        }},
        Case{"current generations are enumerated in name order", [] {
            OutputGenerationTracker tracker;
            require(tracker.update(output("HDMI-A-1")).hasValue(),
                    "second output update failed");
            require(tracker.update(output("DP-1")).hasValue(),
                    "first output update failed");

            const auto currents = tracker.currents();
            require(currents.size() == 2U,
                    "current generation enumeration changed size");
            require(currents[0].snapshot.name == "DP-1",
                    "current generations are not name ordered");
            require(currents[1].snapshot.name == "HDMI-A-1",
                    "current generations are not name ordered");

            require(tracker.remove("DP-1").has_value(),
                    "current output removal failed");
            const auto remaining = tracker.currents();
            require(remaining.size() == 1U &&
                        remaining[0].snapshot.name == "HDMI-A-1",
                    "retired output remained in current enumeration");
        }},
        Case{"all Wayland transforms are accepted", [] {
            OutputGenerationTracker tracker;
            constexpr std::array transforms{
                OutputTransform::Normal,
                OutputTransform::Rotate90,
                OutputTransform::Rotate180,
                OutputTransform::Rotate270,
                OutputTransform::Flipped,
                OutputTransform::Flipped90,
                OutputTransform::Flipped180,
                OutputTransform::Flipped270,
            };
            auto snapshot = output();
            for (const auto transform : transforms) {
                snapshot.transform = transform;
                require(tracker.update(snapshot).hasValue(), "supported output transform was rejected");
            }
            require(tracker.current("DP-1")->generation == transforms.size(),
                    "transform changes did not produce distinct generations");
        }},
        Case{"invalid snapshots fail without mutation", [] {
            OutputGenerationTracker tracker;
            auto snapshot = output();
            snapshot.scale = std::numeric_limits<double>::quiet_NaN();
            require(!tracker.update(snapshot), "NaN scale was accepted");
            require(tracker.activeCount() == 0, "failed first update mutated the tracker");

            require(tracker.update(output()).hasValue(), "valid baseline update failed");
            snapshot = output();
            snapshot.bufferWidth = Limits::MAX_OUTPUT_BUFFER_DIMENSION + 1U;
            const auto result = tracker.update(snapshot);
            require(!result, "over-limit buffer was accepted");
            require(result.error().code == ErrorCode::ResourceLimited,
                    "buffer limit did not report resource-limited");
            require(tracker.current("DP-1")->generation == 1,
                    "failed replacement changed the active generation");
        }},
        Case{"snapshot identity fields are closed", [] {
            OutputGenerationTracker tracker;
            auto snapshot = output();
            snapshot.name = "";
            require(!tracker.update(snapshot), "empty output name was accepted");
            snapshot = output();
            snapshot.objectToken = 0;
            require(!tracker.update(snapshot), "zero object token was accepted");
            snapshot = output();
            snapshot.modeToken = 0;
            require(!tracker.update(snapshot), "zero mode token was accepted");
            snapshot = output();
            snapshot.transform = static_cast<OutputTransform>(99);
            require(!tracker.update(snapshot), "unknown output transform was accepted");
            snapshot = output();
            snapshot.renderFormat = 0;
            require(!tracker.update(snapshot), "zero render format was accepted");
        }},
    });
}
