#include "TestHarness.hpp"

#include "v2/render/PresentationScene.hpp"

#include <array>
#include <utility>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

Material material(std::string name, double glassLevel) {
    MaterialInput input;
    input.glassLevel = glassLevel;
    auto result = validateMaterial(std::move(name), input);
    require(result.hasValue(), "test material was invalid");
    return std::move(result.value());
}

OutputGeneration output(
    std::string name,
    double logicalX,
    std::uint64_t objectToken) {
    return {
        .snapshot = OutputSnapshot{
            .name = std::move(name),
            .objectToken = objectToken,
            .modeToken = objectToken,
            .bufferWidth = 100,
            .bufferHeight = 100,
            .logicalX = logicalX,
            .logicalY = 0.0,
            .logicalWidth = 100.0,
            .logicalHeight = 100.0,
            .scale = 1.0,
            .transform = OutputTransform::Normal,
            .renderFormat = 0x34325241U,
            .colorStateToken = 1,
        },
        .generation = 1,
    };
}

ResolvedTarget target(
    std::string owner,
    std::string id,
    MaterialSource source,
    std::string materialName,
    Rect geometry) {
    return {
        .definition = Target{
            .id = id,
            .kind = TargetKind::Region,
            .material = {
                .source = source,
                .name = std::move(materialName),
            },
            .shape = RoundedRectShape{},
            .selector = RegionSelector{.output = "DP-1"},
            .geometry = geometry,
            .stage = RenderStage::PostWindows,
            .transition = std::nullopt,
            .enabled = true,
        },
        .attachment = ResolvedAttachment{
            .identity = {
                .owner = std::move(owner),
                .targetId = std::move(id),
            },
            .kind = TargetKind::Region,
            .objectToken = 1,
            .globalGeometry = geometry,
            .stage = RenderStage::PostWindows,
            .outputFilter = std::nullopt,
            .opacity = 1.0,
        },
        .roundingPower = 2.0,
    };
}

ConfigSnapshot config() {
    return {
        .version = 2,
        .enabled = true,
        .defaultMaterial = "shared",
        .materials = {
            {"shared", material("shared", 0.25)},
        },
        .windowRules = {},
        .layerRules = {},
    };
}

SessionSnapshot session() {
    return {
        .owner = "client:demo:s1",
        .clientId = "demo",
        .mode = SessionMode::Client,
        .generation = 1,
        .expiresAtMs = 1000,
        .materials = {
            {"local", material("local", 0.75)},
        },
        .targets = {},
    };
}

} // namespace


PlannedPresentation plannedFor(
    std::string owner,
    std::string targetId,
    std::string outputName,
    std::uint64_t attachmentToken = 7) {
    PlannedPresentation planned{};
    planned.presentation.key = PresentationKey{
        .identity = {.owner = std::move(owner), .targetId = std::move(targetId)},
        .output = std::move(outputName),
        .outputGeneration = 1,
        .stage = RenderStage::PostLayer,
    };
    planned.presentation.attachmentToken = attachmentToken;
    return planned;
}

int main() {
    return hfg::test::run({
        Case{"config-rule targets enter and leave readiness with the scene", [] {
            ReadinessTracker readiness;
            PresentationScene scene{};
            scene.presentations.push_back(plannedFor(
                std::string(CONFIG_TARGET_OWNER), "rule:bar", "DP-1"));

            reconcilePresentationReadiness(readiness, scene, {}, {});
            const TargetIdentity identity{
                .owner = std::string(CONFIG_TARGET_OWNER),
                .targetId = "rule:bar",
            };
            require(readiness.target(identity).has_value(),
                    "config-rule target was not accepted from the scene");
            const auto rows = outputGlassLiveness(readiness);
            require(rows.size() == 1U && rows[0].awaiting == 1,
                    "config-rule presentation is invisible to liveness");

            // The rule stops matching: the record must leave with the scene
            // rather than surviving as a phantom accepted target.
            reconcilePresentationReadiness(readiness, PresentationScene{}, {}, {});
            require(!readiness.target(identity).has_value(),
                    "config-rule target outlived the scene");
        }},
        Case{"an unchanged inactive target stops bumping the sequence", [] {
            ReadinessTracker readiness;
            require(readiness
                        .accept({.owner = "client:1", .targetId = "bar"})
                        .hasValue(),
                    "accept failed");
            PresentationScene scene{};
            scene.inactive.push_back({
                .identity = {.owner = "client:1", .targetId = "bar"},
                .reason = TargetInactiveReason::Offscreen,
            });

            reconcilePresentationReadiness(readiness, scene, {}, {});
            const auto first = readiness.target(
                {.owner = "client:1", .targetId = "bar"});
            require(first.has_value() &&
                        first->state == ReadinessState::Inactive,
                    "inactive target was not reported");

            reconcilePresentationReadiness(readiness, scene, {}, {});
            const auto second = readiness.target(
                {.owner = "client:1", .targetId = "bar"});
            require(second.has_value() &&
                        second->sequence == first->sequence,
                    "an unchanged inactive report bumped the sequence");
        }},
        Case{"target spanning outputs creates independent presentations", [] {
            const auto active = config();
            const std::array outputs{
                output("DP-1", 0.0, 1),
                output("DP-2", 100.0, 2),
            };
            TargetScene targets{
                .effective = {
                    target(
                        "config",
                        "wide",
                        MaterialSource::Config,
                        "shared",
                        Rect{50.0, 10.0, 100.0, 40.0}),
                },
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            const auto result = buildPresentationScene(
                targets,
                &active,
                std::span<const SessionSnapshot>{},
                outputs,
                0);
            require(result.hasValue(), "presentation scene failed");
            require(result.value().presentations.size() == 2U,
                    "spanning target did not create two presentations");
            require(result.value().presentations[0].output.snapshot.name !=
                        result.value().presentations[1].output.snapshot.name,
                    "presentations reused one output generation");
            require(result.value().presentations[0].material.name == "shared",
                    "configuration material was not attached");
        }},
        Case{"owner material and sampling reach follow each presentation", [] {
            const std::array sessions{session()};
            const std::array outputs{
                output("DP-1", 0.0, 1),
            };
            TargetScene targets{
                .effective = {
                    target(
                        "client:demo:s1",
                        "local",
                        MaterialSource::Session,
                        "local",
                        Rect{10.0, 10.0, 40.0, 40.0}),
                },
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            const auto result = buildPresentationScene(
                targets,
                nullptr,
                sessions,
                outputs,
                0);
            require(result.hasValue() &&
                        result.value().presentations.size() == 1U,
                    "session presentation did not plan");
            const auto& planned =
                result.value().presentations.front();
            require(planned.material.glassLevel == 0.75,
                    "owner-session material changed");
            require(planned.sampling.apronPixels > 0U,
                    "material sampling footprint was omitted");
        }},
        Case{"active handoff morph drives geometry with a stable envelope", [] {
            const std::array sessions{session()};
            const std::array outputs{output("DP-1", 0.0, 1)};
            auto moving = target(
                "client:demo:s1",
                "bar",
                MaterialSource::Session,
                "local",
                Rect{0.0, 0.0, 100.0, 44.0});
            moving.definition.kind = TargetKind::Layer;
            moving.definition.selector =
                LayerSelector{.namespaceName = "hgs:bar:DP-1"};
            moving.definition.shape = RoundedRectShape{.radius = 0.0};
            moving.attachment.kind = TargetKind::Layer;
            moving.attachment.stage = RenderStage::PostLayer;
            moving.definition.stage = RenderStage::PostLayer;
            TargetScene targets{
                .effective = {moving},
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            PresentationHandoffTracker tracker;
            const PreparedPresentationHandoff prepared{
                .identity = moving.attachment.identity,
                .sourceGeneration = 1,
                .timeoutMs = 500,
                .presentations = {},
                .morph = PreparedPresentationMorph{
                    .transitionId = "attach-1",
                    .source = {
                        .rect = {12.0, 8.0, 76.0, 44.0},
                        .radius = 18.0,
                    },
                    .destination = {
                        .rect = {0.0, 0.0, 100.0, 44.0},
                        .radius = 0.0,
                    },
                    .durationMs = 240,
                },
            };
            tracker.commit(
                moving.attachment.identity.owner,
                2,
                std::array{prepared},
                1'000);
            const auto result = buildPresentationScene(
                targets,
                nullptr,
                sessions,
                outputs,
                1'120,
                &tracker);
            require(result && result.value().presentations.size() == 1U,
                    "active morph did not produce a presentation");
            const auto& planned = result.value().presentations.front();
            const auto* shape = std::get_if<RoundedRectShape>(
                &planned.target.definition.shape);
            require(planned.target.transitionActive &&
                        planned.transitionEnvelope &&
                        planned.transitionEnvelope->outputLocal ==
                            Rect{0.0, 0.0, 100.0, 52.0} &&
                        planned.target.attachment.globalGeometry.x > 0.0 &&
                        planned.target.attachment.globalGeometry.x < 12.0 &&
                        shape && shape->radius > 0.0 &&
                        shape->radius < 18.0,
                    "morph scene did not preserve envelope or interpolate intent");
        }},
        Case{"bottom output-local morph stays fixed while its layer origin settles", [] {
            const std::array sessions{session()};
            const std::array outputs{output("DP-1", 0.0, 1)};
            auto moving = target(
                "client:demo:s1",
                "bar",
                MaterialSource::Session,
                "local",
                Rect{0.0, 40.0, 100.0, 44.0});
            moving.definition.kind = TargetKind::Layer;
            moving.definition.selector =
                LayerSelector{.namespaceName = "hgs:bar:DP-1"};
            moving.definition.geometry = Rect{0.0, 0.0, 100.0, 44.0};
            moving.definition.shape = RoundedRectShape{.radius = 0.0};
            moving.attachment.kind = TargetKind::Layer;
            moving.attachment.stage = RenderStage::PostLayer;
            moving.attachment.outputFilter = "DP-1";
            moving.attachment.containerGlobalGeometry =
                Rect{0.0, 40.0, 100.0, 60.0};
            moving.definition.stage = RenderStage::PostLayer;
            TargetScene targets{
                .effective = {moving},
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            PresentationHandoffTracker tracker;
            const PreparedPresentationHandoff prepared{
                .identity = moving.attachment.identity,
                .sourceGeneration = 1,
                .timeoutMs = 500,
                .presentations = {},
                .morph = PreparedPresentationMorph{
                    .transitionId = "bottom-attach",
                    .coordinateSpace =
                        PresentationHandoffRequest::MorphCoordinateSpace::
                            OutputLocal,
                    .source = {
                        .rect = {12.0, 48.0, 76.0, 44.0},
                        .radius = 18.0,
                    },
                    .destination = {
                        .rect = {0.0, 56.0, 100.0, 44.0},
                        .radius = 0.0,
                    },
                    .durationMs = 240,
                },
            };
            tracker.commit(
                moving.attachment.identity.owner,
                2,
                std::array{prepared},
                1'000);
            tracker.expire(1'240);

            const auto completed = buildPresentationScene(
                targets, nullptr, sessions, outputs, 1'240, &tracker);
            require(completed &&
                        completed.value().presentations.size() == 1U,
                    "completed bottom morph stopped drawing");
            const auto& override =
                completed.value().presentations.front().target;
            require(override.transitionActive &&
                        override.attachment.globalGeometry ==
                            Rect{0.0, 56.0, 100.0, 44.0} &&
                        tracker.target(moving.attachment.identity)->morph->
                            state == PresentationMorphState::Settling,
                    "bottom destination jumped to the unsettled layer origin");

            auto settledLayer = moving;
            settledLayer.attachment.globalGeometry =
                Rect{0.0, 56.0, 100.0, 44.0};
            settledLayer.attachment.containerGlobalGeometry =
                Rect{0.0, 56.0, 100.0, 44.0};
            TargetScene settledTargets{
                .effective = {settledLayer},
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            const auto settled = buildPresentationScene(
                settledTargets, nullptr, sessions, outputs, 1'241, &tracker);
            require(settled &&
                        settled.value().presentations.size() == 1U &&
                        !settled.value().presentations.front().target
                             .transitionActive &&
                        tracker.target(moving.attachment.identity)->morph->
                            state == PresentationMorphState::Completed,
                    "matching layer attachment did not retire the override");
        }},
        Case{"right output-local morph remains isolated to its output", [] {
            const std::array sessions{session()};
            const std::array outputs{
                output("DP-1", 0.0, 1),
                output("DP-2", 100.0, 2),
            };
            auto moving = target(
                "client:demo:s1",
                "bar",
                MaterialSource::Session,
                "local",
                Rect{40.0, 0.0, 44.0, 100.0});
            moving.definition.kind = TargetKind::Layer;
            moving.definition.selector =
                LayerSelector{.namespaceName = "hgs:bar:DP-1"};
            moving.definition.geometry = Rect{0.0, 0.0, 44.0, 100.0};
            moving.definition.shape = RoundedRectShape{.radius = 0.0};
            moving.attachment.kind = TargetKind::Layer;
            moving.attachment.stage = RenderStage::PostLayer;
            moving.attachment.outputFilter = "DP-1";
            moving.attachment.containerGlobalGeometry =
                Rect{40.0, 0.0, 60.0, 100.0};
            moving.definition.stage = RenderStage::PostLayer;
            TargetScene targets{
                .effective = {moving},
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            PresentationHandoffTracker tracker;
            const PreparedPresentationHandoff prepared{
                .identity = moving.attachment.identity,
                .sourceGeneration = 1,
                .timeoutMs = 500,
                .presentations = {},
                .morph = PreparedPresentationMorph{
                    .transitionId = "right-attach",
                    .coordinateSpace =
                        PresentationHandoffRequest::MorphCoordinateSpace::
                            OutputLocal,
                    .source = {
                        .rect = {48.0, 12.0, 44.0, 76.0},
                        .radius = 18.0,
                    },
                    .destination = {
                        .rect = {56.0, 0.0, 44.0, 100.0},
                        .radius = 0.0,
                    },
                    .durationMs = 240,
                },
            };
            tracker.commit(
                moving.attachment.identity.owner,
                2,
                std::array{prepared},
                1'000);
            const auto scene = buildPresentationScene(
                targets, nullptr, sessions, outputs, 1'120, &tracker);
            require(scene && scene.value().presentations.size() == 1U &&
                        scene.value().presentations.front().output.snapshot
                                .name == "DP-1" &&
                        scene.value().presentations.front().target
                                .attachment.globalGeometry.x > 48.0 &&
                        scene.value().presentations.front().target
                                .attachment.globalGeometry.x < 56.0,
                    "right morph crossed output ownership or jumped");
        }},
        Case{"output-local morph maps through a transformed portrait output once", [] {
            const std::array sessions{session()};
            const std::array outputs{
                OutputGeneration{
                    .snapshot = OutputSnapshot{
                        .name = "DP-1",
                        .objectToken = 9,
                        .modeToken = 9,
                        .bufferWidth = 60,
                        .bufferHeight = 100,
                        .logicalX = 20.0,
                        .logicalY = 30.0,
                        .logicalWidth = 100.0,
                        .logicalHeight = 60.0,
                        .scale = 1.0,
                        .transform = OutputTransform::Rotate270,
                        .renderFormat = 0x34325241U,
                        .colorStateToken = 1,
                    },
                    .generation = 2,
                },
            };
            auto moving = target(
                "client:demo:s1",
                "bar",
                MaterialSource::Session,
                "local",
                Rect{60.0, 30.0, 44.0, 60.0});
            moving.definition.kind = TargetKind::Layer;
            moving.definition.selector =
                LayerSelector{.namespaceName = "hgs:bar:DP-1"};
            moving.definition.geometry = Rect{0.0, 0.0, 44.0, 60.0};
            moving.definition.shape = RoundedRectShape{.radius = 0.0};
            moving.attachment.kind = TargetKind::Layer;
            moving.attachment.stage = RenderStage::PostLayer;
            moving.attachment.outputFilter = "DP-1";
            moving.attachment.containerGlobalGeometry =
                Rect{60.0, 30.0, 60.0, 60.0};
            moving.definition.stage = RenderStage::PostLayer;
            TargetScene targets{
                .effective = {moving},
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            PresentationHandoffTracker tracker;
            const PreparedPresentationHandoff prepared{
                .identity = moving.attachment.identity,
                .sourceGeneration = 1,
                .timeoutMs = 500,
                .presentations = {},
                .morph = PreparedPresentationMorph{
                    .transitionId = "portrait-right",
                    .coordinateSpace =
                        PresentationHandoffRequest::MorphCoordinateSpace::
                            OutputLocal,
                    .source = {
                        .rect = {48.0, 8.0, 44.0, 44.0},
                        .radius = 18.0,
                    },
                    .destination = {
                        .rect = {56.0, 0.0, 44.0, 60.0},
                        .radius = 0.0,
                    },
                    .durationMs = 240,
                },
            };
            tracker.commit(
                moving.attachment.identity.owner,
                2,
                std::array{prepared},
                1'000);
            const auto scene = buildPresentationScene(
                targets, nullptr, sessions, outputs, 1'120, &tracker);
            require(scene && scene.value().presentations.size() == 1U,
                    "transformed output-local morph did not plan");
            const auto& planned = scene.value().presentations.front();
            require(planned.output.snapshot.transform ==
                        OutputTransform::Rotate270 &&
                        planned.target.attachment.globalGeometry.x > 68.0 &&
                        planned.target.attachment.globalGeometry.x < 76.0 &&
                        planned.transitionEnvelope &&
                        planned.transitionEnvelope->coverage.width > 0 &&
                        planned.transitionEnvelope->coverage.height > 0,
                    "output transform was skipped or applied before logical morphing");
        }},
        Case{"missing material fails only its target", [] {
            const auto active = config();
            const std::array outputs{
                output("DP-1", 0.0, 1),
            };
            TargetScene targets{
                .effective = {
                    target(
                        "config",
                        "missing",
                        MaterialSource::Config,
                        "missing",
                        Rect{10.0, 10.0, 20.0, 20.0}),
                    target(
                        "config",
                        "valid",
                        MaterialSource::Config,
                        "shared",
                        Rect{40.0, 10.0, 20.0, 20.0}),
                },
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            const auto result = buildPresentationScene(
                targets,
                &active,
                std::span<const SessionSnapshot>{},
                outputs,
                0);
            require(result.hasValue(), "one material failure discarded the scene");
            require(result.value().presentations.size() == 1U,
                    "valid sibling presentation was lost");
            require(result.value().failures.size() == 1U &&
                        result.value().failures.front().identity.targetId ==
                            "missing",
                    "material failure was not retained by target");
        }},
        Case{"fully clipped target becomes inactive", [] {
            const auto active = config();
            const std::array outputs{
                output("DP-1", 0.0, 1),
            };
            TargetScene targets{
                .effective = {
                    target(
                        "config",
                        "offscreen",
                        MaterialSource::Config,
                        "shared",
                        Rect{200.0, 10.0, 20.0, 20.0}),
                },
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            const auto result = buildPresentationScene(
                targets,
                &active,
                std::span<const SessionSnapshot>{},
                outputs,
                0);
            require(result.hasValue(), "clipped scene failed");
            require(result.value().presentations.empty(),
                    "clipped target produced a presentation");
            require(result.value().inactive.size() == 1U &&
                        result.value().inactive.front().identity.targetId ==
                            "offscreen",
                    "clipped target was not marked inactive");
            require(result.value().inactive.front().reason ==
                        TargetInactiveReason::Offscreen,
                    "clipped target did not report why it is inactive");
        }},
        Case{"duplicate current output names fail globally", [] {
            const auto active = config();
            const std::array outputs{
                output("DP-1", 0.0, 1),
                output("DP-1", 0.0, 2),
            };
            const auto result = buildPresentationScene(
                TargetScene{},
                &active,
                std::span<const SessionSnapshot>{},
                outputs,
                0);
            require(!result &&
                        result.error().code ==
                            ErrorCode::StaleGeneration,
                    "ambiguous current output set was accepted");
        }},
        Case{"definition and attachment identity cannot diverge", [] {
            const auto active = config();
            auto malformed = target(
                "config",
                "original",
                MaterialSource::Config,
                "shared",
                Rect{10.0, 10.0, 20.0, 20.0});
            malformed.attachment.identity.targetId = "different";
            TargetScene targets{
                .effective = {std::move(malformed)},
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            const auto result = buildPresentationScene(
                targets,
                &active,
                std::span<const SessionSnapshot>{},
                std::span<const OutputGeneration>{},
                0);
            require(!result &&
                        result.error().code == ErrorCode::InvalidTarget,
                    "divergent resolved identity was accepted");
        }},
        Case{"target scene state is carried into presentation state", [] {
            TargetScene targets{
                .effective = {},
                .inactive = {{{"client:a:s1", "inactive"},
                              TargetInactiveReason::Disabled}},
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
            const auto result = buildPresentationScene(
                targets,
                nullptr,
                std::span<const SessionSnapshot>{},
                std::span<const OutputGeneration>{},
                0);
            require(result.hasValue(), "empty presentation scene failed");
            require(result.value().inactive == targets.inactive,
                    "inactive state was lost");
            require(result.value().suppressed == targets.suppressed,
                    "suppressed state was lost");
            require(result.value().failures == targets.failures,
                    "resolution failures were lost");
        }},
        Case{"revealing visibility keeps the full outward damage envelope", [] {
            auto owner = session();
            auto moving = target(
                owner.owner,
                "bar",
                MaterialSource::Session,
                "local",
                Rect{0.0, 0.0, 100.0, 44.0});
            moving.definition.kind = TargetKind::Layer;
            moving.definition.selector =
                LayerSelector{.namespaceName = "hgs:bar:DP-1"};
            moving.definition.shape = RoundedRectShape{.radius = 18.0};
            moving.attachment.kind = TargetKind::Layer;
            moving.attachment.stage = RenderStage::PostLayer;
            moving.attachment.outputFilter = "DP-1";
            moving.definition.stage = RenderStage::PostLayer;
            owner.targets = {moving.definition};
            SessionReplacement replacement{
                .generation = 2,
                .materials = owner.materials,
                .targets = {moving.definition},
                .handoffs = {},
                .visibilityTransitions = {{
                    .targetId = "bar",
                    .transitionId = "reveal-1",
                    .sourceGeneration = 1,
                    .direction = VisibilityTransitionDirection::Reveal,
                    .edge = TransitionEdge::Top,
                    .sourceRect = {0.0, 0.0, 100.0, 44.0},
                    .sourceRadius = 18.0,
                    .travel = 52.0,
                    .durationMs = 200,
                    .timeoutMs = 750,
                    .output = "DP-1",
                    .namespaceName = "hgs:bar:DP-1",
                }},
            };
            VisibilityTransitionTracker visibility;
            auto prepared = visibility.prepare(
                owner, replacement, owner.owner, 1'000);
            require(prepared.hasValue(), "reveal was not prepared");
            visibility.commit(std::move(prepared.value()));
            TargetScene targets{
                .effective = {moving},
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            const std::array sessions{owner};
            const std::array outputs{output("DP-1", 0.0, 1)};
            const auto result = buildPresentationScene(
                targets,
                nullptr,
                sessions,
                outputs,
                1'000,
                nullptr,
                &visibility);
            require(result && result.value().presentations.size() == 1U,
                    "armed reveal did not produce a presentation");
            const auto& planned = result.value().presentations.front();
            require(
                planned.target.attachment.globalGeometry ==
                        Rect{0.0, 0.0, 100.0, 44.0} &&
                    planned.target.transitionEnvelopeGlobal ==
                        Rect{0.0, -52.0, 100.0, 96.0} &&
                    planned.transitionEnvelope &&
                    planned.transitionEnvelope->outputLocal ==
                        Rect{0.0, 0.0, 100.0, 44.0} &&
                    planned.presentation.opacity == 0.0,
                "reveal capture omitted the outward source position");
        }},
        Case{"target motion resolves before output mapping", [] {
            const auto active = config();
            const std::array outputs{
                output("DP-1", 0.0, 1),
            };
            auto moving = target(
                "config",
                "moving",
                MaterialSource::Config,
                "shared",
                Rect{10.0, 10.0, 20.0, 20.0});
            moving.definition.transition = Transition{
                .id = "enter-1",
                .phase = TransitionPhase::Enter,
                .edge = TransitionEdge::Bottom,
                .durationMs = 200,
                .elapsedMs = 0,
                .travel = 20.0,
                .easing = {},
            };
            moving.transitionAnchorMs = 1000;
            TargetScene targets{
                .effective = {std::move(moving)},
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            const auto result = buildPresentationScene(
                targets,
                &active,
                std::span<const SessionSnapshot>{},
                outputs,
                1100);
            require(
                result.hasValue() &&
                    result.value().presentations.size() == 1U,
                "moving target did not produce a presentation");
            const auto& planned =
                result.value().presentations.front();
            require(
                planned.target.attachment.globalGeometry ==
                        Rect{10.0, 20.0, 20.0, 20.0} &&
                    planned.presentation.geometry.outputLocal ==
                        Rect{10.0, 20.0, 20.0, 20.0} &&
                    planned.presentation.opacity == 0.5 &&
                    planned.target.transitionActive &&
                    planned.motionTimeMs == 1100,
                "target motion was applied after mapping or lost");
        }},
    });
}
