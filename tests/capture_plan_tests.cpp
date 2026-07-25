#include "TestHarness.hpp"

#include "v2/render/CapturePlan.hpp"

#include <array>
#include <limits>
#include <vector>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

constexpr std::uint32_t AR24 = 0x34325241U;
constexpr std::uint32_t AB30 = 0x30334241U;

CaptureLimits limits(
    std::uint32_t maxWidth = 4096,
    std::uint32_t maxHeight = 4096,
    std::uint64_t maxPixels = 4096U * 4096U,
    std::uint64_t maxBytes = 4096U * 4096U * 16U) {
    return {
        .maxWidth = maxWidth,
        .maxHeight = maxHeight,
        .maxApronPixels = 512,
        .maxBytesPerPixel = 16,
        .maxPixels = maxPixels,
        .maxBytes = maxBytes,
    };
}

OutputGeneration output(
    std::uint64_t generation = 1,
    std::uint32_t format = AR24,
    std::uint64_t colorState = 7) {
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
            .renderFormat = format,
            .colorStateToken = colorState,
        },
        .generation = generation,
    };
}

CaptureRequest request(
    PixelRect coverage,
    std::uint32_t apron = 0,
    std::uint32_t bytesPerPixel = 4) {
    return {
        .output = output(),
        .stage = RenderStage::PostWindows,
        .coverage = coverage,
        .apronPixels = apron,
        .bytesPerPixel = bytesPerPixel,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"capture expansion clips to output bounds", [] {
            const std::array requests{
                request(PixelRect{.x = 5, .y = 10, .width = 100, .height = 50}, 20),
            };
            const auto result = planCaptures(requests, limits());
            require(result.hasValue() && result.value().size() == 1U, "capture was not planned");
            const auto& plan = result.value().front();
            require(
                plan.region == PixelRect{.x = 0, .y = 0, .width = 125, .height = 80},
                "capture apron or clipping changed");
            require(plan.pixelCount == 10'000U, "capture pixel count changed");
            require(plan.byteCount == 40'000U, "capture byte count changed");
        }},
        Case{"compatible requests share their bounded union", [] {
            const std::array requests{
                request(PixelRect{.x = 100, .y = 100, .width = 100, .height = 100}, 10),
                request(PixelRect{.x = 180, .y = 120, .width = 100, .height = 60}, 10),
            };
            const auto result = planCaptures(requests, limits());
            require(result.hasValue() && result.value().size() == 1U, "compatible captures did not merge");
            require(
                result.value().front().region ==
                    PixelRect{.x = 90, .y = 90, .width = 200, .height = 120},
                "compatible capture union changed");
        }},
        Case{"capture identity preserves generation stage format and color", [] {
            std::vector<CaptureRequest> requests;
            requests.push_back(request(PixelRect{10, 10, 20, 20}));
            auto differentGeneration = requests.front();
            differentGeneration.output = output(2);
            requests.push_back(differentGeneration);
            auto differentStage = requests.front();
            differentStage.stage = RenderStage::PreWindow;
            differentStage.stageObjectToken = 42;
            requests.push_back(differentStage);
            auto differentFormat = requests.front();
            differentFormat.output = output(1, AB30);
            differentFormat.bytesPerPixel = 8;
            requests.push_back(differentFormat);
            auto differentColor = requests.front();
            differentColor.output = output(1, AR24, 8);
            requests.push_back(differentColor);

            const auto result = planCaptures(requests, limits());
            require(result.hasValue() && result.value().size() == 5U, "incompatible captures were reused");
            require(result.value()[0].key.renderFormat == AR24, "source format was replaced");
            require(result.value()[3].key.renderFormat == AB30, "wide format was replaced");
            require(result.value()[4].key.colorStateToken == 8U, "color state was discarded");
        }},
        Case{"pre-window captures are scoped to an exact window", [] {
            auto first = request(PixelRect{10, 10, 20, 20});
            first.stage = RenderStage::PreWindow;
            first.stageObjectToken = 41;
            auto second = first;
            second.coverage = PixelRect{15, 15, 20, 20};
            second.stageObjectToken = 42;

            const auto result = planCaptures(
                std::array{first, second},
                limits());
            require(result.hasValue() &&
                        result.value().size() == 2U,
                    "different pre-window objects shared a capture");
            require(result.value()[0].key.stageObjectToken == 41U &&
                        result.value()[1].key.stageObjectToken == 42U,
                    "pre-window object identity was lost");
        }},
        Case{"bounded planner partitions distant compatible requests", [] {
            const std::array requests{
                request(PixelRect{.x = 0, .y = 0, .width = 100, .height = 100}),
                request(PixelRect{.x = 500, .y = 0, .width = 100, .height = 100}),
            };
            const auto result = planCaptures(
                requests,
                limits(200, 200, 40'000, 160'000));
            require(result.hasValue() && result.value().size() == 2U, "distant captures were rejected or over-merged");
        }},
        Case{"planner chooses the smallest compatible union", [] {
            const std::array requests{
                request(PixelRect{.x = 0, .y = 0, .width = 100, .height = 100}),
                request(PixelRect{.x = 500, .y = 0, .width = 100, .height = 100}),
                request(PixelRect{.x = 90, .y = 0, .width = 20, .height = 100}),
            };
            const auto result = planCaptures(
                requests,
                limits(200, 200, 40'000, 160'000));
            require(result.hasValue() && result.value().size() == 2U, "capture bins changed");
            require(
                result.value()[0].region == PixelRect{.x = 0, .y = 0, .width = 110, .height = 100},
                "request did not join the smallest compatible union");
        }},
        Case{"individual over-limit capture fails before allocation", [] {
            const std::array requests{
                request(PixelRect{.x = 0, .y = 0, .width = 300, .height = 100}),
            };
            const auto result = planCaptures(
                requests,
                limits(200, 200, 40'000, 160'000));
            require(!result, "over-limit capture was accepted");
            require(result.error().code == ErrorCode::ResourceLimited, "wrong resource failure code");
            require(result.error().path == "requests[0].coverage", "wrong resource failure path");
        }},
        Case{"byte limits use checked adapter format size", [] {
            const std::array requests{
                request(PixelRect{.x = 0, .y = 0, .width = 100, .height = 100}, 0, 16),
            };
            auto restricted = limits();
            restricted.maxBytes = 159'999;
            const auto result = planCaptures(requests, restricted);
            require(!result, "capture exceeding byte limit was accepted");
            require(result.error().code == ErrorCode::ResourceLimited, "wrong byte-limit error");
        }},
        Case{"coverage must be contained by its output generation", [] {
            const std::array requests{
                request(PixelRect{.x = 1900, .y = 1000, .width = 30, .height = 90}),
            };
            const auto result = planCaptures(requests, limits());
            require(!result, "out-of-buffer coverage was accepted");
            require(result.error().path == "requests[0].coverage", "wrong coverage failure path");
        }},
        Case{"malformed output stage and format size fail closed", [] {
            auto malformedOutput = request(PixelRect{0, 0, 10, 10});
            malformedOutput.output.generation = 0;
            require(
                !planCaptures(std::array{malformedOutput}, limits()),
                "zero output generation was accepted");

            auto malformedStage = request(PixelRect{0, 0, 10, 10});
            malformedStage.stage = static_cast<RenderStage>(99);
            require(
                !planCaptures(std::array{malformedStage}, limits()),
                "unknown render stage was accepted");

            auto unscopedPreWindow =
                request(PixelRect{0, 0, 10, 10});
            unscopedPreWindow.stage =
                RenderStage::PreWindow;
            require(
                !planCaptures(
                    std::array{unscopedPreWindow},
                    limits()),
                "unscoped pre-window capture was accepted");

            auto scopedGlobal =
                request(PixelRect{0, 0, 10, 10});
            scopedGlobal.stageObjectToken = 9;
            require(
                !planCaptures(
                    std::array{scopedGlobal},
                    limits()),
                "global stage accepted an object token");

            auto malformedFormatSize = request(PixelRect{0, 0, 10, 10});
            malformedFormatSize.bytesPerPixel = 0;
            require(
                !planCaptures(std::array{malformedFormatSize}, limits()),
                "zero format size was accepted");
        }},
        Case{"limits are explicit and validated", [] {
            const std::array requests{
                request(PixelRect{0, 0, 10, 10}),
            };
            auto invalidLimits = limits();
            invalidLimits.maxBytes = 0;
            const auto result = planCaptures(requests, invalidLimits);
            require(!result, "zero allocation limit was accepted");
            require(result.error().path == "limits.max_bytes", "wrong limit failure path");
        }},
        Case{"empty request set allocates nothing", [] {
            const std::array<CaptureRequest, 0> requests{};
            const auto result = planCaptures(requests, limits());
            require(result.hasValue() && result.value().empty(), "empty request set created captures");
        }},
        Case{"coverage helper requires exact compatibility and containment", [] {
            const CapturePlan available{
                .key = CaptureKey{"DP-1", 1, RenderStage::PostWindows, AR24, 7},
                .region = PixelRect{.x = 10, .y = 10, .width = 100, .height = 100},
                .bytesPerPixel = 4,
                .pixelCount = 10'000,
                .byteCount = 40'000,
            };
            auto required = available;
            required.region = PixelRect{.x = 20, .y = 20, .width = 10, .height = 10};
            require(capturePlanCovers(available, required), "contained compatible capture was not reusable");
            required.key.outputGeneration = 2;
            require(!capturePlanCovers(available, required), "stale output generation was reusable");
            required = available;
            required.region = PixelRect{.x = 0, .y = 20, .width = 20, .height = 20};
            require(!capturePlanCovers(available, required), "uncontained capture was reusable");
        }},
        Case{"standalone plan validation rejects inconsistent accounting", [] {
            CapturePlan capture{
                .key = CaptureKey{"DP-1", 1, RenderStage::PostWindows, AR24, 7},
                .region = PixelRect{.x = 0, .y = 0, .width = 10, .height = 10},
                .bytesPerPixel = 4,
                .pixelCount = 100,
                .byteCount = 400,
            };
            require(validateCapturePlan(capture).hasValue(), "valid capture plan was rejected");
            capture.pixelCount = 99;
            const auto result = validateCapturePlan(capture);
            require(!result, "inconsistent pixel count was accepted");
            require(result.error().path == "plan.pixel_count", "wrong plan-validation path");
        }},
    });
}
