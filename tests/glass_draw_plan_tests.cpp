#include "TestHarness.hpp"

#include "v2/model/Material.hpp"
#include "v2/model/Target.hpp"
#include "v2/render/CaptureScene.hpp"
#include "v2/render/GlassDrawPlan.hpp"
#include "v2/render/MaterialSampling.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <variant>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

constexpr std::uint32_t AR24 = 0x34325241U;
constexpr double EPSILON = 1e-9;

bool near(double left, double right) {
    return std::abs(left - right) <= EPSILON;
}

OutputGeneration output(
    OutputTransform transform = OutputTransform::Normal,
    double scale = 1.0,
    std::uint64_t generation = 1) {
    const bool swapped =
        transform == OutputTransform::Rotate90 ||
        transform == OutputTransform::Rotate270 ||
        transform == OutputTransform::Flipped90 ||
        transform == OutputTransform::Flipped270;
    const auto logicalWidth = 800.0;
    const auto logicalHeight = 600.0;
    const auto orientedWidth =
        static_cast<std::uint32_t>(
            logicalWidth * scale);
    const auto orientedHeight =
        static_cast<std::uint32_t>(
            logicalHeight * scale);
    return {
        .snapshot = {
            .name = "DP-1",
            .objectToken = 10,
            .modeToken = 20,
            .bufferWidth = swapped
                ? orientedHeight
                : orientedWidth,
            .bufferHeight = swapped
                ? orientedWidth
                : orientedHeight,
            .logicalX = 0.0,
            .logicalY = 0.0,
            .logicalWidth = logicalWidth,
            .logicalHeight = logicalHeight,
            .scale = scale,
            .transform = transform,
            .renderFormat = AR24,
            .colorStateToken = 30,
        },
        .generation = generation,
    };
}

Material material(
    bool tintEnabled = false,
    bool lightMode = false) {
    MaterialInput input;
    input.tintEnabled = tintEnabled;
    input.tintColor = "#336699";
    input.lightMode = lightMode;
    const auto result = validateMaterial("fluid", input);
    if (!result)
        throw hfg::test::Failure("material fixture failed");
    return result.value();
}

Target target(
    Rect geometry,
    Shape shape = RoundedRectShape{.radius = 20.0},
    RenderStage stage = RenderStage::PostWindows,
    TargetKind kind = TargetKind::Region) {
    TargetInput input{
        .id = "surface",
        .kind = kind,
        .material = {
            .source = MaterialSource::Session,
            .name = "fluid",
        },
        .shape = std::move(shape),
        .selector = kind == TargetKind::Window
            ? TargetSelector(WindowSelector{
                  .address = "0x1234",
                  .pid = 1234,
                  .initialClass = std::nullopt,
              })
            : TargetSelector(RegionSelector{
                  .output = "DP-1",
              }),
        .geometry = kind == TargetKind::Region
            ? std::optional<Rect>(geometry)
            : std::nullopt,
        .stage = kind == TargetKind::Region
            ? std::optional<RenderStage>(stage)
            : std::nullopt,
        .transition = std::nullopt,
        .enabled = true,
    };
    const auto result = validateTarget(std::move(input));
    if (!result)
        throw hfg::test::Failure("target fixture failed");
    return result.value();
}

struct Fixture {
    CaptureAssignment assignment;
    CaptureResource   resource;
};

Fixture fixture(
    Rect geometry = Rect{100.0, 80.0, 240.0, 120.0},
    OutputTransform transform = OutputTransform::Normal,
    double scale = 1.0,
    Shape shape = RoundedRectShape{.radius = 20.0},
    bool tintEnabled = false,
    bool lightMode = false,
    RenderStage stage = RenderStage::PostWindows,
    TargetKind kind = TargetKind::Region) {
    auto selectedOutput = output(transform, scale);
    auto selectedTarget = target(
        geometry,
        std::move(shape),
        stage,
        kind);
    const TargetIdentity identity{
        .owner = "client:demo:s1",
        .targetId = selectedTarget.id,
    };
    ResolvedTarget resolved{
        .definition = std::move(selectedTarget),
        .attachment = {
            .identity = identity,
            .kind = kind,
            .objectToken = 41,
            .globalGeometry = geometry,
            .stage = stage,
            .outputFilter = "DP-1",
            .opacity = 0.8,
        },
        .roundingPower = 2.0,
    };
    auto presentations = resolvePresentations(
        resolved.attachment,
        std::array{selectedOutput});
    if (!presentations ||
        presentations.value().size() != 1U)
        throw hfg::test::Failure(
            "presentation fixture failed");
    auto selectedMaterial =
        material(tintEnabled, lightMode);
    auto sampling = resolveMaterialSampling(
        selectedMaterial,
        geometry.width,
        geometry.height,
        scale);
    if (!sampling)
        throw hfg::test::Failure(
            "sampling fixture failed");
    PlannedPresentation planned{
        .target = std::move(resolved),
        .material = std::move(selectedMaterial),
        .presentation =
            std::move(presentations.value().front()),
        .output = selectedOutput,
        .sampling = sampling.value(),
    };
    const CaptureRequest request{
        .output = selectedOutput,
        .stage = stage,
        .coverage =
            planned.presentation.geometry.coverage,
        .apronPixels = sampling.value().apronPixels,
        .bytesPerPixel = 4,
        .stageObjectToken =
            stage == RenderStage::PreWindow
            ? planned.target.attachment.objectToken
            : 0U,
    };
    const auto maxPixels =
        static_cast<std::uint64_t>(
            selectedOutput.snapshot.bufferWidth) *
        selectedOutput.snapshot.bufferHeight;
    const CaptureLimits limits{
        .maxWidth = selectedOutput.snapshot.bufferWidth,
        .maxHeight = selectedOutput.snapshot.bufferHeight,
        .maxApronPixels = 1000,
        .maxBytesPerPixel = 16,
        .maxPixels = maxPixels,
        .maxBytes = maxPixels * 16U,
    };
    auto capture = planCaptures(
        std::array{request},
        limits);
    if (!capture ||
        capture.value().size() != 1U)
        throw hfg::test::Failure(
            "capture fixture failed");
    return {
        .assignment = {
            .presentation = std::move(planned),
            .required = capture.value().front(),
            .captureIndex = 0,
        },
        .resource = {
            .token = 99,
            .plan = capture.value().front(),
        },
    };
}

void requireUvMatchesCapture(
    const GlassDrawPlan& plan,
    const MappedGeometry& geometry,
    const CapturePlan& capture) {
    for (std::size_t index = 0;
         index < plan.sourceCorners.size();
         ++index) {
        const auto& point =
            geometry.semanticCorners[index];
        const auto expectedU =
            (point.x - capture.region.x) /
            capture.region.width;
        const auto expectedV =
            (capture.region.y +
                capture.region.height -
                point.y) /
            capture.region.height;
        require(
            near(plan.sourceCorners[index].u, expectedU) &&
                near(plan.sourceCorners[index].v, expectedV),
            "capture UV did not preserve semantic corner mapping");
    }
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"normal output resolves geometry and capture UVs", [] {
            auto input = fixture();
            const auto result = buildGlassDrawPlan(
                input.assignment,
                input.resource);
            require(result.hasValue(), "draw plan failed");
            const auto& plan = result.value();
            require(
                plan.resourceToken == 99 &&
                    plan.capture == input.resource.plan &&
                    plan.destination ==
                        input.assignment.presentation
                            .presentation.geometry.outputLocal &&
                    plan.destinationPixels ==
                        Rect{100.0, 80.0, 240.0, 120.0} &&
                    plan.damageCoverage ==
                        input.assignment.presentation
                            .presentation.geometry.coverage &&
                    plan.captureDamageCoverage ==
                        input.resource.plan.region,
                "draw identity or destination changed");
            require(
                plan.fullSizePixels ==
                    DrawSize{240.0, 120.0} &&
                    plan.clipOffsetPixels ==
                        Point{0.0, 0.0} &&
                    plan.clippedSizePixels ==
                        DrawSize{240.0, 120.0},
                "normal output dimensions changed");
            requireUvMatchesCapture(
                plan,
                input.assignment.presentation
                    .presentation.geometry,
                input.resource.plan);
        }},
        Case{"fractional scale is applied once", [] {
            auto input = fixture(
                Rect{40.0, 30.0, 200.0, 100.0},
                OutputTransform::Normal,
                1.25);
            const auto result = buildGlassDrawPlan(
                input.assignment,
                input.resource);
            require(
                result.hasValue() &&
                    result.value().fullSizePixels ==
                        DrawSize{250.0, 125.0} &&
                    result.value().destinationPixels ==
                        Rect{50.0, 37.5, 250.0, 125.0} &&
                    result.value().clippedSizePixels ==
                        DrawSize{250.0, 125.0},
                "fractional scale was lost or applied twice");
            const auto* rounded =
                std::get_if<RoundedRectShape>(
                    &result.value().shapePixels);
            require(
                rounded && near(rounded->radius, 25.0),
                "shape radius did not follow output scale");
        }},
        Case{"clipped target retains full shape coordinates", [] {
            auto input = fixture(
                Rect{-40.0, 50.0, 200.0, 100.0});
            const auto result = buildGlassDrawPlan(
                input.assignment,
                input.resource);
            require(
                result.hasValue() &&
                    result.value().destination ==
                        Rect{0.0, 50.0, 160.0, 100.0} &&
                    result.value().fullSizePixels ==
                        DrawSize{200.0, 100.0} &&
                    result.value().clipOffsetPixels ==
                        Point{40.0, 0.0} &&
                    result.value().clippedSizePixels ==
                        DrawSize{160.0, 100.0},
                "clipping collapsed the target-local shape basis");
        }},
        Case{"rotated and flipped outputs preserve semantic UVs", [] {
            for (const auto transform : {
                     OutputTransform::Rotate90,
                     OutputTransform::Rotate180,
                     OutputTransform::Rotate270,
                     OutputTransform::Flipped,
                     OutputTransform::Flipped90,
                     OutputTransform::Flipped180,
                     OutputTransform::Flipped270,
                 }) {
                auto input = fixture(
                    Rect{70.0, 90.0, 210.0, 130.0},
                    transform);
                const auto result = buildGlassDrawPlan(
                    input.assignment,
                    input.resource);
                require(
                    result.hasValue(),
                    "transformed draw plan failed");
                requireUvMatchesCapture(
                    result.value(),
                    input.assignment.presentation
                        .presentation.geometry,
                    input.resource.plan);
                const auto expectedDamage =
                    mapBufferPixelRectToOutput(
                        input.assignment.presentation
                            .presentation.geometry.coverage,
                        input.assignment.presentation
                            .output.snapshot);
                const auto expectedCaptureDamage =
                    mapBufferPixelRectToOutput(
                        input.resource.plan.region,
                        input.assignment.presentation
                            .output.snapshot);
                require(
                    expectedDamage.hasValue() &&
                        expectedCaptureDamage.hasValue() &&
                        result.value().damageCoverage ==
                            expectedDamage.value() &&
                        result.value().captureDamageCoverage ==
                            expectedCaptureDamage.value(),
                    "transformed output damage remained in buffer space");
            }
        }},
        Case{"larger compatible capture drives source UVs", [] {
            auto input = fixture();
            auto larger = input.resource.plan;
            larger.region = {
                .x = 0,
                .y = 0,
                .width = static_cast<std::int32_t>(
                    input.assignment.presentation.output
                        .snapshot.bufferWidth),
                .height = static_cast<std::int32_t>(
                    input.assignment.presentation.output
                        .snapshot.bufferHeight),
            };
            larger.pixelCount =
                static_cast<std::uint64_t>(
                    larger.region.width) *
                larger.region.height;
            larger.byteCount =
                larger.pixelCount * larger.bytesPerPixel;
            input.resource.plan = larger;
            const auto result = buildGlassDrawPlan(
                input.assignment,
                input.resource);
            require(
                result.hasValue(),
                "covering merged capture was rejected");
            requireUvMatchesCapture(
                result.value(),
                input.assignment.presentation
                    .presentation.geometry,
                larger);
        }},
        Case{"capture identity mismatch fails closed", [] {
            auto input = fixture();
            input.resource.plan.key.outputGeneration = 2;
            const auto result = buildGlassDrawPlan(
                input.assignment,
                input.resource);
            require(
                !result &&
                    result.error().code ==
                        ErrorCode::StaleGeneration,
                "stale capture generation reached drawing");
        }},
        Case{"undersized capture fails closed", [] {
            auto input = fixture();
            input.resource.plan.region.width -= 1;
            input.resource.plan.pixelCount =
                static_cast<std::uint64_t>(
                    input.resource.plan.region.width) *
                input.resource.plan.region.height;
            input.resource.plan.byteCount =
                input.resource.plan.pixelCount *
                input.resource.plan.bytesPerPixel;
            const auto result = buildGlassDrawPlan(
                input.assignment,
                input.resource);
            require(
                !result,
                "non-covering capture reached drawing");
        }},
        Case{"capture outside the output fails closed", [] {
            auto input = fixture();
            input.resource.plan.region.width =
                static_cast<std::int32_t>(
                    input.assignment.presentation.output
                        .snapshot.bufferWidth) +
                1;
            input.resource.plan.pixelCount =
                static_cast<std::uint64_t>(
                    input.resource.plan.region.width) *
                input.resource.plan.region.height;
            input.resource.plan.byteCount =
                input.resource.plan.pixelCount *
                input.resource.plan.bytesPerPixel;
            const auto result = buildGlassDrawPlan(
                input.assignment,
                input.resource);
            require(
                !result &&
                    result.error().path ==
                        "resource.plan.region",
                "out-of-output capture reached drawing");
        }},
        Case{"zero resource token is rejected", [] {
            auto input = fixture();
            input.resource.token = 0;
            const auto result = buildGlassDrawPlan(
                input.assignment,
                input.resource);
            require(
                !result &&
                    result.error().path == "resource.token",
                "anonymous GPU resource reached drawing");
        }},
        Case{"noncanonical mapped geometry is rejected", [] {
            auto input = fixture();
            input.assignment.presentation.presentation
                .geometry.outputLocal.x += 1.0;
            const auto result = buildGlassDrawPlan(
                input.assignment,
                input.resource);
            require(
                !result &&
                    result.error().path ==
                        "assignment.presentation.geometry",
                "forged mapped geometry reached drawing");
        }},
        Case{"noncanonical capture requirement is rejected", [] {
            auto input = fixture();
            input.assignment.required.region.x += 1;
            input.assignment.required.region.width -= 1;
            input.assignment.required.pixelCount =
                static_cast<std::uint64_t>(
                    input.assignment.required.region.width) *
                input.assignment.required.region.height;
            input.assignment.required.byteCount =
                input.assignment.required.pixelCount *
                input.assignment.required.bytesPerPixel;
            const auto result = buildGlassDrawPlan(
                input.assignment,
                input.resource);
            require(
                !result &&
                    result.error().path ==
                        "assignment.required",
                "forged capture requirement reached drawing");
        }},
        Case{"pre-window capture keeps exact window identity", [] {
            auto input = fixture(
                Rect{100.0, 80.0, 240.0, 120.0},
                OutputTransform::Normal,
                1.0,
                RoundedRectShape{.radius = 20.0},
                false,
                false,
                RenderStage::PreWindow,
                TargetKind::Window);
            const auto result = buildGlassDrawPlan(
                input.assignment,
                input.resource);
            require(
                result.hasValue() &&
                    input.resource.plan.key.stageObjectToken ==
                        41,
                "exact pre-window identity was lost");
            input.resource.plan.key.stageObjectToken = 42;
            require(
                !buildGlassDrawPlan(
                    input.assignment,
                    input.resource),
                "wrong pre-window object reached drawing");
        }},
        Case{"disabled target and invalid opacity are rejected", [] {
            auto disabled = fixture();
            disabled.assignment.presentation.target
                .definition.enabled = false;
            require(
                !buildGlassDrawPlan(
                    disabled.assignment,
                    disabled.resource),
                "disabled target reached drawing");

            auto opacity = fixture();
            opacity.assignment.presentation.presentation.opacity =
                std::numeric_limits<double>::quiet_NaN();
            require(
                !buildGlassDrawPlan(
                    opacity.assignment,
                    opacity.resource),
                "non-finite opacity reached drawing");
        }},
        Case{"invalid rounding power is rejected", [] {
            auto input = fixture();
            input.assignment.presentation.target.roundingPower =
                std::numeric_limits<double>::infinity();
            require(
                !buildGlassDrawPlan(
                    input.assignment,
                    input.resource),
                "non-finite rounding power reached drawing");
        }},
        Case{"rounded and ring shape lengths scale", [] {
            auto rounded = fixture(
                Rect{100.0, 80.0, 240.0, 120.0},
                OutputTransform::Normal,
                2.0,
                RoundedRectShape{.radius = 12.0});
            auto roundedPlan = buildGlassDrawPlan(
                rounded.assignment,
                rounded.resource);
            const auto* roundedShape =
                std::get_if<RoundedRectShape>(
                    &roundedPlan.value().shapePixels);
            require(
                roundedShape &&
                    roundedShape->radius == 24.0,
                "rounded shape did not scale");

            auto ring = fixture(
                Rect{100.0, 80.0, 240.0, 120.0},
                OutputTransform::Normal,
                2.0,
                RingShape{
                    .outerRadius = 30.0,
                    .thickness = 8.0,
                });
            auto ringPlan = buildGlassDrawPlan(
                ring.assignment,
                ring.resource);
            const auto* ringShape =
                std::get_if<RingShape>(
                    &ringPlan.value().shapePixels);
            require(
                ringShape &&
                    ringShape->outerRadius == 60.0 &&
                    ringShape->thickness == 16.0,
                "ring shape did not scale");
        }},
        Case{"settled compound shape lengths scale", [] {
            CompoundShape shape;
            shape.base = CompoundBase{
                .corners = {10.0, 11.0, 12.0, 13.0},
            };
            shape.cutout = CompoundCutout{
                .rect = {20.0, 30.0, 100.0, 40.0},
                .corners = {4.0, 5.0, 6.0, 7.0},
            };
            shape.parts.push_back({
                .rect = {25.0, 35.0, 20.0, 10.0},
                .corners = {1.0, 2.0, 3.0, 4.0},
                .junctions = {5.0, 6.0, 7.0, 8.0},
                .materialExtent =
                    Rect{20.0, 30.0, 30.0, 20.0},
                .transition = PartTransition{
                    .motion = Transition{
                        .id = "part-in",
                        .phase = TransitionPhase::Enter,
                        .edge = TransitionEdge::Top,
                        .durationMs = 200,
                        .elapsedMs = 200,
                        .travel = 10.0,
                        .easing = {},
                    },
                    .protrusion = 6.0,
                },
                .opacity = 0.5,
            });
            shape.connectors.push_back(
                {10.0, 20.0, 5.0, 6.0});
            shape.connectorCurve = 9.0;
            auto input = fixture(
                Rect{100.0, 80.0, 240.0, 120.0},
                OutputTransform::Normal,
                1.5,
                shape);
            const auto result = buildGlassDrawPlan(
                input.assignment,
                input.resource);
            require(result.hasValue(), "compound plan failed");
            const auto* scaled =
                std::get_if<CompoundShape>(
                    &result.value().shapePixels);
            require(
                scaled &&
                    scaled->cutout->rect ==
                        Rect{30.0, 45.0, 150.0, 60.0} &&
                    scaled->parts.front().rect ==
                        Rect{37.5, 52.5, 30.0, 15.0} &&
                    !scaled->parts.front().transition &&
                    scaled->connectors.front() ==
                        Rect{15.0, 30.0, 7.5, 9.0} &&
                    scaled->connectorCurve == 13.5,
                "compound pixel scaling changed geometry or time");
        }},
        Case{"compound part motion resolves at scene time", [] {
            CompoundShape shape;
            shape.parts.push_back({
                .rect = {100.0, 100.0, 80.0, 40.0},
                .corners = {},
                .junctions = {},
                .materialExtent =
                    Rect{90.0, 90.0, 100.0, 60.0},
                .transition = PartTransition{
                    .motion = Transition{
                        .id = "part-1",
                        .phase = TransitionPhase::Enter,
                        .edge = TransitionEdge::Top,
                        .durationMs = 200,
                        .elapsedMs = 0,
                        .travel = 40.0,
                        .easing = {},
                    },
                    .protrusion = 20.0,
                },
                .opacity = 0.8,
            });
            auto input = fixture(
                Rect{100.0, 80.0, 240.0, 120.0},
                OutputTransform::Normal,
                1.0,
                shape);
            input.assignment.presentation.target
                .transitionAnchorMs = 1000;
            input.assignment.presentation.motionTimeMs = 1100;
            const auto result = buildGlassDrawPlan(
                input.assignment,
                input.resource);
            require(result.hasValue(), "moving shape plan failed");
            const auto* compound =
                std::get_if<CompoundShape>(
                    &result.value().shapePixels);
            require(
                compound &&
                    compound->parts.front().rect ==
                        Rect{100.0, 90.0, 80.0, 30.0} &&
                    compound->parts.front().opacity == 0.4 &&
                    !compound->parts.front().transition &&
                    result.value().transitionActive,
                "part motion was not resolved before drawing");
        }},
        Case{"material uniforms preserve calibrated dark behavior", [] {
            auto input = fixture();
            const auto result = buildGlassDrawPlan(
                input.assignment,
                input.resource);
            require(result.hasValue(), "material plan failed");
            const auto& uniforms = result.value().material;
            require(
                near(uniforms.blurPixels, 18.0) &&
                    near(uniforms.refractionPixels, 45.0) &&
                    near(uniforms.rimBandPixels, 30.0) &&
                    near(uniforms.bevelPixels, 30.0) &&
                    near(uniforms.lensBandPixels, 33.0) &&
                    near(uniforms.rimWidthPixels, 3.0) &&
                    near(uniforms.tintStrength, 0.43) &&
                    near(uniforms.veilSaturation, 0.95) &&
                    near(uniforms.gloss, 0.077),
                "calibrated dark material values changed");
            require(
                uniforms.tintColor == RgbColor{},
                "disabled stain did not use neutral veil color");
        }},
        Case{"enabled tint and light material remain explicit", [] {
            auto input = fixture(
                Rect{100.0, 80.0, 240.0, 120.0},
                OutputTransform::Normal,
                1.0,
                RoundedRectShape{.radius = 20.0},
                true,
                true);
            const auto result = buildGlassDrawPlan(
                input.assignment,
                input.resource);
            require(result.hasValue(), "light material plan failed");
            const auto& uniforms = result.value().material;
            require(
                near(uniforms.tintColor.red, 0x33 / 255.0) &&
                    near(uniforms.tintColor.green, 0x66 / 255.0) &&
                    near(uniforms.tintColor.blue, 0x99 / 255.0) &&
                    near(uniforms.tintStrength, 0.53) &&
                    near(uniforms.veilSaturation, 0.49),
                "light stained material values changed");
        }},
    });
}
