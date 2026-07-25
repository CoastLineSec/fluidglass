#include "TestHarness.hpp"

#include "v2/render/RenderStageScheduler.hpp"

#include <array>
#include <vector>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

OutputGeneration output(
    std::string name = "DP-1",
    std::uint64_t generation = 1) {
    return {
        .snapshot = {
            .name = std::move(name),
            .objectToken = 1,
            .modeToken = 1,
            .bufferWidth = 1920,
            .bufferHeight = 1080,
            .logicalX = 0.0,
            .logicalY = 0.0,
            .logicalWidth = 1920.0,
            .logicalHeight = 1080.0,
            .scale = 1.0,
            .transform = OutputTransform::Normal,
            .renderFormat = 0x34325241U,
            .colorStateToken = 7,
        },
        .generation = generation,
    };
}

CaptureResource resource(
    std::uint64_t token,
    RenderStage stage,
    std::uint64_t stageObjectToken = 0,
    std::string outputName = "DP-1",
    std::uint64_t generation = 1) {
    const PixelRect region{10, 20, 100, 80};
    const auto pixels =
        static_cast<std::uint64_t>(region.width) *
        region.height;
    return {
        .token = token,
        .plan = {
            .key = {
                .output = std::move(outputName),
                .outputGeneration = generation,
                .stage = stage,
                .renderFormat = 0x34325241U,
                .colorStateToken = 7,
                .stageObjectToken = stageObjectToken,
            },
            .region = region,
            .bytesPerPixel = 4,
            .pixelCount = pixels,
            .byteCount = pixels * 4U,
        },
    };
}

RenderHookEvent event(
    RenderHookStage hook,
    std::uint64_t frameToken = 1,
    std::uint64_t stageObjectToken = 0,
    OutputGeneration currentOutput = output()) {
    return {
        .output = std::move(currentOutput),
        .hook = hook,
        .frameToken = frameToken,
        .stageObjectToken = stageObjectToken,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"global hooks select only their exact render stage", [] {
            RenderStageScheduler scheduler;
            const std::array resources{
                resource(1, RenderStage::PostWallpaper),
                resource(2, RenderStage::PostWindows),
                resource(3, RenderStage::PostLayer),
            };
            const auto wallpaper = scheduler.schedule(
                resources,
                event(RenderHookStage::PostWallpaper));
            const auto windows = scheduler.schedule(
                resources,
                event(RenderHookStage::PostWindows));
            const auto layers = scheduler.schedule(
                resources,
                event(RenderHookStage::LastMoment));
            require(wallpaper.hasValue() &&
                        wallpaper.value() ==
                            std::vector{resources[0]},
                    "post-wallpaper selection changed");
            require(windows.hasValue() &&
                        windows.value() ==
                            std::vector{resources[1]},
                    "post-windows selection changed");
            require(layers.hasValue() &&
                        layers.value() ==
                            std::vector{resources[2]},
                    "post-layer selection changed");
        }},
        Case{"pre-window hook selects only its exact window", [] {
            RenderStageScheduler scheduler;
            const std::array resources{
                resource(
                    1,
                    RenderStage::PreWindow,
                    41),
                resource(
                    2,
                    RenderStage::PreWindow,
                    42),
            };
            const auto first = scheduler.schedule(
                resources,
                event(
                    RenderHookStage::PreWindow,
                    1,
                    41));
            const auto second = scheduler.schedule(
                resources,
                event(
                    RenderHookStage::PreWindow,
                    1,
                    42));
            require(first.hasValue() &&
                        first.value() ==
                            std::vector{resources[0]},
                    "first window selected the wrong capture");
            require(second.hasValue() &&
                        second.value() ==
                            std::vector{resources[1]},
                    "second window selected the wrong capture");
        }},
        Case{"duplicate hooks cannot capture twice in one frame", [] {
            RenderStageScheduler scheduler;
            const std::array resources{
                resource(8, RenderStage::PostWindows),
            };
            const auto first = scheduler.schedule(
                resources,
                event(RenderHookStage::PostWindows, 5));
            const auto duplicate = scheduler.schedule(
                resources,
                event(RenderHookStage::PostWindows, 5));
            require(first.hasValue() &&
                        first.value().size() == 1U,
                    "first hook did not schedule capture");
            require(duplicate.hasValue() &&
                        duplicate.value().empty(),
                    "duplicate hook repeated capture");
        }},
        Case{"new frames and generations schedule again", [] {
            RenderStageScheduler scheduler;
            const std::array firstGeneration{
                resource(8, RenderStage::PostWindows),
            };
            require(scheduler.schedule(
                        firstGeneration,
                        event(RenderHookStage::PostWindows, 5))
                        .hasValue(),
                    "first frame failed");
            const auto nextFrame = scheduler.schedule(
                firstGeneration,
                event(RenderHookStage::PostWindows, 6));
            require(nextFrame.hasValue() &&
                        nextFrame.value().size() == 1U,
                    "new frame did not reschedule capture");

            const std::array nextGeneration{
                resource(
                    9,
                    RenderStage::PostWindows,
                    0,
                    "DP-1",
                    2),
            };
            const auto replaced = scheduler.schedule(
                nextGeneration,
                event(
                    RenderHookStage::PostWindows,
                    1,
                    0,
                    output("DP-1", 2)));
            require(replaced.hasValue() &&
                        replaced.value().size() == 1U,
                    "new output generation did not reset scheduling");
        }},
        Case{"stale frame and generation events fail closed", [] {
            RenderStageScheduler scheduler;
            const std::array current{
                resource(
                    1,
                    RenderStage::PostWindows,
                    0,
                    "DP-1",
                    2),
            };
            require(scheduler.schedule(
                        current,
                        event(
                            RenderHookStage::PostWindows,
                            7,
                            0,
                            output("DP-1", 2)))
                        .hasValue(),
                    "current event failed");
            const auto oldFrame = scheduler.schedule(
                current,
                event(
                    RenderHookStage::PostWindows,
                    6,
                    0,
                    output("DP-1", 2)));
            require(!oldFrame &&
                        oldFrame.error().code ==
                            ErrorCode::StaleGeneration,
                    "stale frame was accepted");
            const auto oldGeneration = scheduler.schedule(
                current,
                event(
                    RenderHookStage::PostWindows,
                    8,
                    0,
                    output("DP-1", 1)));
            require(!oldGeneration,
                    "retired output generation was accepted");
        }},
        Case{"outputs track frames independently", [] {
            RenderStageScheduler scheduler;
            auto other = resource(
                2,
                RenderStage::PostWindows,
                0,
                "HDMI-A-1");
            other.plan.key.colorStateToken = 7;
            const std::array resources{
                resource(1, RenderStage::PostWindows),
                other,
            };
            const auto first = scheduler.schedule(
                resources,
                event(RenderHookStage::PostWindows, 9));
            const auto second = scheduler.schedule(
                resources,
                event(
                    RenderHookStage::PostWindows,
                    2,
                    0,
                    output("HDMI-A-1")));
            require(first.hasValue() &&
                        first.value().size() == 1U &&
                        first.value().front().token == 1U,
                    "first output selection changed");
            require(second.hasValue() &&
                        second.value().size() == 1U &&
                        second.value().front().token == 2U,
                    "second output selection changed");
        }},
        Case{"hook object-token rules fail closed", [] {
            RenderStageScheduler scheduler;
            require(!scheduler.schedule(
                        {},
                        event(RenderHookStage::PreWindow)),
                    "unscoped pre-window hook was accepted");
            require(!scheduler.schedule(
                        {},
                        event(
                            RenderHookStage::PostWindows,
                            1,
                            9)),
                    "global hook accepted a window token");
        }},
        Case{"malformed and duplicate resources fail closed", [] {
            RenderStageScheduler scheduler;
            const auto valid =
                resource(1, RenderStage::PostWindows);
            const std::array duplicates{valid, valid};
            require(!scheduler.schedule(
                        duplicates,
                        event(RenderHookStage::PostWindows)),
                    "duplicate capture token was accepted");

            auto malformed = valid;
            malformed.plan.byteCount = 1;
            require(!scheduler.schedule(
                        std::array{malformed},
                        event(RenderHookStage::PostWindows)),
                    "malformed capture plan was accepted");
        }},
        Case{"clearing output state permits a fresh frame sequence", [] {
            RenderStageScheduler scheduler;
            const std::array resources{
                resource(1, RenderStage::PostWindows),
            };
            require(scheduler.schedule(
                        resources,
                        event(RenderHookStage::PostWindows, 20))
                        .hasValue(),
                    "initial frame failed");
            scheduler.clearOutput("DP-1");
            const auto reset = scheduler.schedule(
                resources,
                event(RenderHookStage::PostWindows, 1));
            require(reset.hasValue() &&
                        reset.value().size() == 1U,
                    "cleared output retained stale frame state");
        }},
    });
}
