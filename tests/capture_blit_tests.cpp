#include "TestHarness.hpp"

#include "v2/render/CaptureBlit.hpp"

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
    });
}
