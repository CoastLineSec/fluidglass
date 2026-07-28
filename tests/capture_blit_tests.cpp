#include "TestHarness.hpp"

#include "v2/render/CaptureBlit.hpp"

#include <array>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

OutputGeneration output() {
    return {
        .snapshot = OutputSnapshot{
            .name = "DP-1",
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
        .generation = 4,
    };
}

CapturePlan plan(PixelRect region) {
    const auto pixels = static_cast<std::uint64_t>(region.width) *
        static_cast<std::uint64_t>(region.height);
    return {
        .key = CaptureKey{
            .output = "DP-1",
            .outputGeneration = 4,
            .stage = RenderStage::PostWindows,
            .renderFormat = 0x34325241U,
            .colorStateToken = 7,
        },
        .region = region,
        .bytesPerPixel = 4,
        .pixelCount = pixels,
        .byteCount = pixels * 4U,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"top-left coverage maps to bottom-left blit coordinates", [] {
            const auto result = captureBlitFor(
                plan(PixelRect{.x = 100, .y = 200, .width = 300, .height = 150}),
                output());
            require(result.hasValue(), "capture blit mapping failed");
            require(
                result.value().source == BlitRect{100, 730, 400, 880},
                "source coordinate conversion changed");
            require(
                result.value().destination == BlitRect{0, 0, 300, 150},
                "destination rectangle changed");
        }},
        Case{"top and bottom output edges map exactly", [] {
            const auto top = captureBlitFor(
                plan(PixelRect{.x = 0, .y = 0, .width = 10, .height = 20}),
                output());
            require(top.hasValue(), "top-edge mapping failed");
            require(top.value().source == BlitRect{0, 1060, 10, 1080}, "top edge moved");

            const auto bottom = captureBlitFor(
                plan(PixelRect{.x = 0, .y = 1060, .width = 10, .height = 20}),
                output());
            require(bottom.hasValue(), "bottom-edge mapping failed");
            require(bottom.value().source == BlitRect{0, 0, 10, 20}, "bottom edge moved");
        }},
        Case{"rotated output uses already-mapped framebuffer coverage", [] {
            auto rotated = output();
            rotated.snapshot.bufferWidth = 1080;
            rotated.snapshot.bufferHeight = 1920;
            rotated.snapshot.logicalWidth = 1920.0;
            rotated.snapshot.logicalHeight = 1080.0;
            rotated.snapshot.transform = OutputTransform::Rotate90;
            const auto result = captureBlitFor(
                plan(PixelRect{.x = 20, .y = 100, .width = 40, .height = 200}),
                rotated);
            require(result.hasValue(), "rotated-output blit mapping failed");
            require(
                result.value().source == BlitRect{20, 1620, 60, 1820},
                "rotated framebuffer coverage was transformed twice");
        }},
        Case{"uninitialized captures receive one complete baseline", [] {
            const auto result = captureUpdateBlits(
                plan(PixelRect{
                    .x = 100,
                    .y = 200,
                    .width = 300,
                    .height = 150,
                }),
                output(),
                std::array{
                    PixelRect{.x = 120, .y = 220, .width = 10, .height = 20},
                },
                false);
            require(
                result.hasValue() &&
                    result.value() ==
                        std::vector{
                            CaptureBlit{
                                .source = {100, 730, 400, 880},
                                .destination = {0, 0, 300, 150},
                            },
                        },
                "new capture did not receive a complete baseline");
        }},
        Case{"initialized captures update only fresh damaged pixels", [] {
            const auto result = captureUpdateBlits(
                plan(PixelRect{
                    .x = 100,
                    .y = 200,
                    .width = 300,
                    .height = 150,
                }),
                output(),
                std::array{
                    PixelRect{.x = 120, .y = 220, .width = 10, .height = 20},
                    PixelRect{.x = 700, .y = 500, .width = 20, .height = 20},
                },
                true);
            require(
                result.hasValue() &&
                    result.value() ==
                        std::vector{
                            CaptureBlit{
                                .source = {120, 840, 130, 860},
                                .destination = {20, 110, 30, 130},
                            },
                        },
                "partial capture overwrote retained undamaged pixels");
        }},
        Case{"transform three damage maps into its bounded capture", [] {
            auto rotated = output();
            rotated.snapshot.bufferWidth = 1920;
            rotated.snapshot.bufferHeight = 1080;
            rotated.snapshot.logicalWidth = 1080.0;
            rotated.snapshot.logicalHeight = 1920.0;
            rotated.snapshot.transform = OutputTransform::Rotate270;
            auto rotatedPlan = plan(PixelRect{
                .x = 1800,
                .y = 0,
                .width = 120,
                .height = 1080,
            });
            const auto result = captureUpdateBlits(
                rotatedPlan,
                rotated,
                std::array{
                    PixelRect{.x = 100, .y = 10, .width = 200, .height = 20},
                },
                true);
            require(
                result.hasValue() &&
                    result.value() ==
                        std::vector{
                            CaptureBlit{
                                .source = {1890, 780, 1910, 980},
                                .destination = {90, 780, 110, 980},
                            },
                        },
                "transform-three damage used the wrong coordinate space");
        }},
        Case{"initialized captures preserve clean pixels without damage", [] {
            const auto result = captureUpdateBlits(
                plan(PixelRect{100, 200, 300, 150}),
                output(),
                {},
                true);
            require(
                result.hasValue() && result.value().empty(),
                "damage-free frame recopied stale compositor contents");
        }},
        Case{"stale output identity fails closed", [] {
            auto stale = output();
            stale.generation = 5;
            const auto result = captureBlitFor(
                plan(PixelRect{0, 0, 10, 10}),
                stale);
            require(!result, "stale output generation was accepted");
            require(result.error().code == ErrorCode::StaleGeneration, "wrong stale-output error");
        }},
        Case{"out-of-buffer region fails before blit", [] {
            const auto result = captureBlitFor(
                plan(PixelRect{.x = 1900, .y = 1000, .width = 30, .height = 90}),
                output());
            require(!result, "out-of-buffer blit was accepted");
            require(result.error().path == "plan.region", "wrong region failure path");
        }},
        Case{"malformed capture plan is rejected", [] {
            auto malformed = plan(PixelRect{0, 0, 10, 10});
            malformed.byteCount = 1;
            require(!captureBlitFor(malformed, output()), "malformed plan was accepted");
        }},
        Case{"malformed output damage fails closed", [] {
            const auto result = captureUpdateBlits(
                plan(PixelRect{100, 200, 300, 150}),
                output(),
                std::array{
                    PixelRect{-1, 0, 10, 10},
                },
                true);
            require(
                !result &&
                    result.error().path ==
                        "output_damage[0].rect",
                "invalid output damage reached a capture update");
        }},
    });
}
