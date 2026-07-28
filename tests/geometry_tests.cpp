#include "TestHarness.hpp"

#include "v2/render/Geometry.hpp"

#include <array>
#include <cmath>
#include <limits>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

OutputGeneration output(OutputTransform transform = OutputTransform::Normal) {
    const bool rotated = transform == OutputTransform::Rotate90 ||
        transform == OutputTransform::Rotate270 ||
        transform == OutputTransform::Flipped90 ||
        transform == OutputTransform::Flipped270;
    return {
        .snapshot = OutputSnapshot{
            .name = "DP-1",
            .objectToken = 1,
            .modeToken = 1,
            .bufferWidth = 200,
            .bufferHeight = 100,
            .logicalX = 0.0,
            .logicalY = 0.0,
            .logicalWidth = rotated ? 100.0 : 200.0,
            .logicalHeight = rotated ? 200.0 : 100.0,
            .scale = 1.0,
            .transform = transform,
            .renderFormat = 0x34325241U,
            .colorStateToken = 1,
        },
        .generation = 1,
    };
}

void requireNear(double actual, double expected, std::string_view message) {
    require(std::abs(actual - expected) < 1e-9, message);
}

void requireRect(const Rect& actual, const Rect& expected, std::string_view message) {
    requireNear(actual.x, expected.x, message);
    requireNear(actual.y, expected.y, message);
    requireNear(actual.width, expected.width, message);
    requireNear(actual.height, expected.height, message);
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"fractional coverage preserves outside edges", [] {
            OutputGeneration generation{
                .snapshot = OutputSnapshot{
                    .name = "DP-1",
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
                },
                .generation = 1,
            };
            const auto mapped = mapGlobalLogicalRect(
                Rect{.x = 100.2, .y = 50.4, .width = 10.1, .height = 5.2},
                generation);
            require(mapped.hasValue() && mapped.value(), "fractional geometry did not map");
            requireRect(
                mapped.value()->bufferRect,
                Rect{.x = 125.25, .y = 63.0, .width = 12.625, .height = 6.5},
                "fractional buffer rectangle changed");
            require(
                mapped.value()->coverage == PixelRect{.x = 125, .y = 63, .width = 13, .height = 7},
                "outside-preserving coverage changed");
        }},
        Case{"global geometry clips before mapping", [] {
            auto generation = output();
            generation.snapshot.logicalX = -200.0;
            const auto mapped = mapGlobalLogicalRect(
                Rect{.x = -210.0, .y = 90.0, .width = 30.0, .height = 20.0},
                generation);
            require(mapped.hasValue() && mapped.value(), "partially visible geometry did not map");
            requireRect(
                mapped.value()->clippedGlobal,
                Rect{.x = -200.0, .y = 90.0, .width = 20.0, .height = 10.0},
                "global clipping changed");
            requireRect(
                mapped.value()->outputLocal,
                Rect{.x = 0.0, .y = 90.0, .width = 20.0, .height = 10.0},
                "output-local clipping changed");
            require(
                mapped.value()->coverage == PixelRect{.x = 0, .y = 90, .width = 20, .height = 10},
                "clipped coverage changed");
        }},
        Case{"fully clipped geometry has no presentation", [] {
            const auto mapped = mapGlobalLogicalRect(
                Rect{.x = 250.0, .y = 20.0, .width = 40.0, .height = 40.0},
                output());
            require(mapped.hasValue(), "fully clipped geometry returned an error");
            require(!mapped.value(), "fully clipped geometry produced coverage");
        }},
        Case{"all output transforms map to framebuffer space", [] {
            struct Expected {
                OutputTransform transform;
                Rect            bufferRect;
            };
            constexpr std::array cases{
                Expected{OutputTransform::Normal, Rect{10.0, 20.0, 30.0, 40.0}},
                Expected{OutputTransform::Rotate90, Rect{20.0, 60.0, 40.0, 30.0}},
                Expected{OutputTransform::Rotate180, Rect{160.0, 40.0, 30.0, 40.0}},
                Expected{OutputTransform::Rotate270, Rect{140.0, 10.0, 40.0, 30.0}},
                Expected{OutputTransform::Flipped, Rect{160.0, 20.0, 30.0, 40.0}},
                Expected{OutputTransform::Flipped90, Rect{20.0, 10.0, 40.0, 30.0}},
                Expected{OutputTransform::Flipped180, Rect{10.0, 40.0, 30.0, 40.0}},
                Expected{OutputTransform::Flipped270, Rect{140.0, 60.0, 40.0, 30.0}},
            };
            for (const auto& test : cases) {
                const auto mapped = mapGlobalLogicalRect(
                    Rect{.x = 10.0, .y = 20.0, .width = 30.0, .height = 40.0},
                    output(test.transform));
                require(mapped.hasValue() && mapped.value(), "supported transform did not map");
                requireRect(mapped.value()->bufferRect, test.bufferRect, "transformed rectangle changed");
                require(
                    mapped.value()->coverage == PixelRect{
                        .x = static_cast<std::int32_t>(test.bufferRect.x),
                        .y = static_cast<std::int32_t>(test.bufferRect.y),
                        .width = static_cast<std::int32_t>(test.bufferRect.width),
                        .height = static_cast<std::int32_t>(test.bufferRect.height),
                    },
                    "transformed coverage changed");
            }
        }},
        Case{"semantic corners follow the inverse output transform", [] {
            const auto mapped = mapGlobalLogicalRect(
                Rect{.x = 10.0, .y = 20.0, .width = 30.0, .height = 40.0},
                output(OutputTransform::Rotate90));
            require(mapped.hasValue() && mapped.value(), "rotated geometry did not map");
            const std::array expected{
                Point{20.0, 90.0},
                Point{20.0, 60.0},
                Point{60.0, 60.0},
                Point{60.0, 90.0},
            };
            require(mapped.value()->semanticCorners == expected, "semantic corner order changed");
        }},
        Case{"pixel rectangles round trip through every output transform", [] {
            for (const auto transform : {
                     OutputTransform::Normal,
                     OutputTransform::Rotate90,
                     OutputTransform::Rotate180,
                     OutputTransform::Rotate270,
                     OutputTransform::Flipped,
                     OutputTransform::Flipped90,
                     OutputTransform::Flipped180,
                     OutputTransform::Flipped270,
                 }) {
                const auto snapshot = output(transform).snapshot;
                const PixelRect oriented{10, 20, 30, 40};
                const auto buffer = mapOutputPixelRectToBuffer(
                    oriented,
                    snapshot);
                require(
                    buffer.hasValue(),
                    "output-oriented pixel rectangle did not map");
                const auto roundTrip = mapBufferPixelRectToOutput(
                    buffer.value(),
                    snapshot);
                require(
                    roundTrip.hasValue() &&
                        roundTrip.value() == oriented,
                    "pixel rectangle did not round trip");
            }
        }},
        Case{"transform three maps a portrait top bar to the buffer edge", [] {
            OutputGeneration generation{
                .snapshot = {
                    .name = "HDMI-A-2",
                    .objectToken = 3,
                    .modeToken = 7,
                    .bufferWidth = 1920,
                    .bufferHeight = 1080,
                    .logicalX = 0.0,
                    .logicalY = 1237.0,
                    .logicalWidth = 1080.0,
                    .logicalHeight = 1920.0,
                    .scale = 1.0,
                    .transform = OutputTransform::Rotate270,
                    .renderFormat = 0x34325241U,
                    .colorStateToken = 9,
                },
                .generation = 4,
            };
            const PixelRect outputBar{0, 0, 1080, 60};
            const auto buffer = mapOutputPixelRectToBuffer(
                outputBar,
                generation.snapshot);
            require(
                buffer.hasValue() &&
                    buffer.value() ==
                        PixelRect{1860, 0, 60, 1080},
                "portrait top bar did not map to the buffer edge");
            const auto restored = mapBufferPixelRectToOutput(
                buffer.value(),
                generation.snapshot);
            require(
                restored.hasValue() &&
                    restored.value() == outputBar,
                "portrait buffer edge did not map back to the top bar");
        }},
        Case{"fractional clipping stays inside the buffer", [] {
            OutputGeneration generation{
                .snapshot = OutputSnapshot{
                    .name = "DP-1",
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
                },
                .generation = 1,
            };
            const auto mapped = mapGlobalLogicalRect(
                Rect{.x = -5.0, .y = 1070.0, .width = 20.0, .height = 20.0},
                generation);
            require(mapped.hasValue() && mapped.value(), "edge-clipped geometry did not map");
            require(
                mapped.value()->coverage == PixelRect{.x = 0, .y = 1337, .width = 19, .height = 13},
                "fractional edge coverage changed");
        }},
        Case{"inconsistent output metrics fail closed", [] {
            auto generation = output();
            generation.snapshot.logicalWidth = 190.0;
            const auto result = mapGlobalLogicalRect(
                Rect{.x = 10.0, .y = 10.0, .width = 20.0, .height = 20.0},
                generation);
            require(!result, "inconsistent output metrics were accepted");
            require(result.error().path == "output.metrics", "metric failure path changed");
        }},
        Case{"mapping validates every boundary", [] {
            auto generation = output();
            generation.generation = 0;
            require(!mapGlobalLogicalRect(
                Rect{.x = 0.0, .y = 0.0, .width = 20.0, .height = 20.0},
                generation), "zero output generation was accepted");

            generation = output();
            generation.snapshot.scale = std::numeric_limits<double>::infinity();
            require(!mapGlobalLogicalRect(
                Rect{.x = 0.0, .y = 0.0, .width = 20.0, .height = 20.0},
                generation), "invalid output snapshot was accepted");

            generation = output();
            require(!mapGlobalLogicalRect(
                Rect{
                    .x = 0.0,
                    .y = 0.0,
                    .width = std::numeric_limits<double>::quiet_NaN(),
                    .height = 20.0,
                },
                generation), "invalid target geometry was accepted");
        }},
    });
}
