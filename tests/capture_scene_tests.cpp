#include "TestHarness.hpp"

#include "v2/render/CaptureScene.hpp"

#include <array>
#include <utility>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

constexpr std::uint32_t AR24 = 0x34325241U;
constexpr std::uint32_t AB4H = 0x48344241U;

CaptureLimits limits(
    std::uint32_t maxWidth = 1024,
    std::uint32_t maxHeight = 1024) {
    return {
        .maxWidth = maxWidth,
        .maxHeight = maxHeight,
        .maxApronPixels = 512,
        .maxBytesPerPixel = 16,
        .maxPixels =
            static_cast<std::uint64_t>(maxWidth) *
            maxHeight,
        .maxBytes =
            static_cast<std::uint64_t>(maxWidth) *
            maxHeight * 16U,
    };
}

PlannedPresentation presentation(
    std::string id,
    PixelRect coverage,
    std::uint32_t format = AR24,
    std::uint32_t apron = 10,
    RenderStage stage = RenderStage::PostWindows,
    std::uint64_t objectToken = 1,
    TargetKind kind = TargetKind::Region) {
    const TargetIdentity identity{
        .owner = "client:demo:s1",
        .targetId = id,
    };
    OutputGeneration output{
        .snapshot = OutputSnapshot{
            .name = "DP-1",
            .objectToken = 1,
            .modeToken = 1,
            .bufferWidth = 1000,
            .bufferHeight = 1000,
            .logicalX = 0.0,
            .logicalY = 0.0,
            .logicalWidth = 1000.0,
            .logicalHeight = 1000.0,
            .scale = 1.0,
            .transform = OutputTransform::Normal,
            .renderFormat = format,
            .colorStateToken = 1,
        },
        .generation = 1,
    };
    Material selectedMaterial;
    selectedMaterial.name = "fluid";
    MaterialSamplingFootprint sampling;
    sampling.apronPixels = apron;
    return {
        .target = ResolvedTarget{
            .definition = Target{
                .id = id,
                .kind = kind,
                .material = {
                    .source = MaterialSource::Session,
                    .name = "fluid",
                },
                .shape = RoundedRectShape{},
                .selector = RegionSelector{.output = "DP-1"},
                .geometry = Rect{
                    static_cast<double>(coverage.x),
                    static_cast<double>(coverage.y),
                    static_cast<double>(coverage.width),
                    static_cast<double>(coverage.height),
                },
                .stage = stage,
                .transition = std::nullopt,
                .enabled = true,
            },
            .attachment = ResolvedAttachment{
                .identity = identity,
                .kind = kind,
                .objectToken = objectToken,
                .globalGeometry = Rect{
                    static_cast<double>(coverage.x),
                    static_cast<double>(coverage.y),
                    static_cast<double>(coverage.width),
                    static_cast<double>(coverage.height),
                },
                .stage = stage,
                .outputFilter = "DP-1",
                .opacity = 1.0,
            },
            .roundingPower = 2.0,
        },
        .material = std::move(selectedMaterial),
        .presentation = ResolvedPresentation{
            .key = {
                .identity = identity,
                .output = "DP-1",
                .outputGeneration = 1,
                .stage = stage,
            },
            .attachmentToken = objectToken,
            .geometry = MappedGeometry{
                .clippedGlobal = {},
                .outputLocal = {},
                .bufferRect = {},
                .coverage = coverage,
                .semanticCorners = {},
            },
            .opacity = 1.0,
        },
        .output = std::move(output),
        .sampling = sampling,
    };
}

PresentationScene scene(
    std::vector<PlannedPresentation> presentations) {
    return {
        .presentations = std::move(presentations),
        .inactive = {},
        .suppressed = {},
        .failures = {},
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"overlapping compatible presentations share one capture", [] {
            const auto input = scene({
                presentation(
                    "left",
                    PixelRect{100, 100, 100, 100}),
                presentation(
                    "right",
                    PixelRect{180, 120, 100, 60}),
            });
            const std::array formats{
                CaptureFormatLayout{AR24, 4},
            };
            const auto result = buildCaptureScene(
                input,
                formats,
                limits());
            require(result.hasValue(), "capture scene failed");
            require(result.value().captures.size() == 1U,
                    "compatible captures were not merged");
            require(result.value().assignments.size() == 2U,
                    "presentation assignments were lost");
            require(result.value().assignments[0].captureIndex == 0U &&
                        result.value().assignments[1].captureIndex == 0U,
                    "merged presentations did not share the capture");
        }},
        Case{"exact format layouts determine byte accounting", [] {
            const auto input = scene({
                presentation(
                    "sdr",
                    PixelRect{10, 10, 20, 20},
                    AR24),
                presentation(
                    "fp16",
                    PixelRect{40, 10, 20, 20},
                    AB4H),
            });
            const std::array formats{
                CaptureFormatLayout{AR24, 4},
                CaptureFormatLayout{AB4H, 8},
            };
            const auto result = buildCaptureScene(
                input,
                formats,
                limits());
            require(result.hasValue() &&
                        result.value().captures.size() == 2U,
                    "incompatible formats were merged or rejected");
            require(result.value().captures[0].bytesPerPixel == 4U,
                    "SDR format size changed");
            require(result.value().captures[1].bytesPerPixel == 8U,
                    "FP16 format size changed");
        }},
        Case{"different windows never share a pre-window capture", [] {
            const auto input = scene({
                presentation(
                    "first-window",
                    PixelRect{10, 10, 100, 100},
                    AR24,
                    10,
                    RenderStage::PreWindow,
                    41,
                    TargetKind::Window),
                presentation(
                    "second-window",
                    PixelRect{20, 20, 100, 100},
                    AR24,
                    10,
                    RenderStage::PreWindow,
                    42,
                    TargetKind::Window),
            });
            const std::array formats{
                CaptureFormatLayout{AR24, 4},
            };
            const auto result = buildCaptureScene(
                input,
                formats,
                limits());
            require(result.hasValue() &&
                        result.value().captures.size() == 2U,
                    "different windows shared pre-window capture state");
            require(result.value().captures[0]
                            .key.stageObjectToken == 41U &&
                        result.value().captures[1]
                            .key.stageObjectToken == 42U,
                    "window capture identity was lost");
        }},
        Case{"pre-window regions fail without poisoning siblings", [] {
            const auto input = scene({
                presentation(
                    "valid",
                    PixelRect{10, 10, 20, 20}),
                presentation(
                    "invalid-pre-window",
                    PixelRect{40, 10, 20, 20},
                    AR24,
                    10,
                    RenderStage::PreWindow),
            });
            const std::array formats{
                CaptureFormatLayout{AR24, 4},
            };
            const auto result = buildCaptureScene(
                input,
                formats,
                limits());
            require(result.hasValue(),
                    "unsupported region discarded its sibling");
            require(result.value().assignments.size() == 1U,
                    "valid sibling capture was lost");
            require(result.value().captureFailures.size() == 1U &&
                        result.value().captureFailures.front()
                                .error.code ==
                            ErrorCode::UnsupportedOperation,
                    "unsupported pre-window region was not isolated");
        }},
        Case{"unsupported format fails only its presentation", [] {
            const auto input = scene({
                presentation(
                    "supported",
                    PixelRect{10, 10, 20, 20},
                    AR24),
                presentation(
                    "unsupported",
                    PixelRect{40, 10, 20, 20},
                    AB4H),
            });
            const std::array formats{
                CaptureFormatLayout{AR24, 4},
            };
            const auto result = buildCaptureScene(
                input,
                formats,
                limits());
            require(result.hasValue(),
                    "unsupported sibling discarded the capture scene");
            require(result.value().assignments.size() == 1U &&
                        result.value().assignments.front()
                                .presentation.presentation.key
                                .identity.targetId ==
                            "supported",
                    "supported presentation was lost");
            require(result.value().captureFailures.size() == 1U &&
                        result.value().captureFailures.front()
                                .key.identity.targetId ==
                            "unsupported",
                    "unsupported format failure was not isolated");
        }},
        Case{"over-limit capture fails only its presentation", [] {
            const auto input = scene({
                presentation(
                    "small",
                    PixelRect{10, 10, 20, 20},
                    AR24,
                    0),
                presentation(
                    "large",
                    PixelRect{100, 100, 300, 100},
                    AR24,
                    0),
            });
            const std::array formats{
                CaptureFormatLayout{AR24, 4},
            };
            const auto result = buildCaptureScene(
                input,
                formats,
                limits(200, 200));
            require(result.hasValue(),
                    "large sibling discarded the capture scene");
            require(result.value().assignments.size() == 1U,
                    "small capture was lost");
            require(result.value().captureFailures.size() == 1U &&
                        result.value().captureFailures.front()
                                .error.code ==
                            ErrorCode::ResourceLimited,
                    "large capture failure was not isolated");
        }},
        Case{"format catalog and limits fail closed", [] {
            const auto input = scene({});
            const std::array duplicateFormats{
                CaptureFormatLayout{AR24, 4},
                CaptureFormatLayout{AR24, 4},
            };
            require(!buildCaptureScene(
                        input,
                        duplicateFormats,
                        limits()),
                    "duplicate format layout was accepted");

            auto invalidLimits = limits();
            invalidLimits.maxBytes = 0;
            require(!buildCaptureScene(
                        input,
                        std::span<const CaptureFormatLayout>{},
                        invalidLimits),
                    "invalid capture limits were accepted");
        }},
        Case{"presentation identity cannot diverge or duplicate", [] {
            auto mismatched = presentation(
                "mismatched",
                PixelRect{10, 10, 20, 20});
            mismatched.presentation.key.output = "DP-2";
            const std::array formats{
                CaptureFormatLayout{AR24, 4},
            };
            require(!buildCaptureScene(
                        scene({std::move(mismatched)}),
                        formats,
                        limits()),
                    "mismatched presentation identity was accepted");

            auto duplicate = presentation(
                "duplicate",
                PixelRect{10, 10, 20, 20});
            require(!buildCaptureScene(
                        scene({duplicate, duplicate}),
                        formats,
                        limits()),
                    "duplicate presentation key was accepted");
        }},
        Case{"presentation scene state is retained", [] {
            PresentationScene input{
                .presentations = {},
                .inactive = {{"client:a:s1", "inactive"}},
                .suppressed = {{"config", "suppressed"}},
                .failures = {{
                    .identity = {"client:b:s2", "failed"},
                    .error = {
                        .code = ErrorCode::UnresolvedTarget,
                        .path = "target",
                        .message = "missing",
                    },
                }},
            };
            const auto result = buildCaptureScene(
                input,
                std::span<const CaptureFormatLayout>{},
                limits());
            require(result.hasValue(), "empty capture scene failed");
            require(result.value().inactive == input.inactive,
                    "inactive state was lost");
            require(result.value().suppressed == input.suppressed,
                    "suppressed state was lost");
            require(result.value().targetFailures == input.failures,
                    "target failures were lost");
        }},
    });
}
