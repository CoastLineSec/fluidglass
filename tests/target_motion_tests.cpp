#include "TestHarness.hpp"

#include "v2/targets/TargetMotion.hpp"

#include <limits>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

ResolvedTarget target(
    TransitionPhase phase = TransitionPhase::Enter,
    TransitionEdge edge = TransitionEdge::Bottom) {
    return {
        .definition = Target{
            .id = "surface",
            .kind = TargetKind::Region,
            .material = {
                .source = MaterialSource::Session,
                .name = "fluid",
            },
            .shape = RoundedRectShape{.radius = 10.0},
            .selector = RegionSelector{.output = "DP-1"},
            .geometry = Rect{100.0, 100.0, 200.0, 100.0},
            .stage = RenderStage::PostWindows,
            .transition = Transition{
                .id = "surface-enter",
                .phase = phase,
                .edge = edge,
                .durationMs = 200,
                .elapsedMs = 0,
                .travel = 40.0,
                .easing = {},
            },
            .enabled = true,
        },
        .attachment = {
            .identity = {
                .owner = "client:demo:s1",
                .targetId = "surface",
            },
            .kind = TargetKind::Region,
            .objectToken = 1,
            .globalGeometry = {
                100.0,
                100.0,
                200.0,
                100.0,
            },
            .stage = RenderStage::PostWindows,
            .outputFilter = "DP-1",
            .opacity = 0.8,
        },
        .roundingPower = 2.0,
        .transitionAnchorMs = 1000,
        .transitionActive = false,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"enter motion translates geometry and composes opacity", [] {
            const auto result = resolveTargetMotion(
                target(),
                1100);
            require(
                result.hasValue() &&
                    result.value().attachment.globalGeometry ==
                        Rect{100.0, 120.0, 200.0, 100.0} &&
                    result.value().attachment.opacity == 0.4 &&
                    result.value().transitionActive,
                "target enter motion was not applied");
        }},
        Case{"completed motion preserves base geometry and opacity", [] {
            const auto result = resolveTargetMotion(
                target(),
                1200);
            require(
                result.hasValue() &&
                    result.value().attachment.globalGeometry ==
                        Rect{100.0, 100.0, 200.0, 100.0} &&
                    result.value().attachment.opacity == 0.8 &&
                    !result.value().transitionActive,
                "completed target motion did not settle");
        }},
        Case{"exit motion translates toward its edge", [] {
            const auto result = resolveTargetMotion(
                target(
                    TransitionPhase::Exit,
                    TransitionEdge::Left),
                1100);
            require(
                result.hasValue() &&
                    result.value().attachment.globalGeometry ==
                        Rect{80.0, 100.0, 200.0, 100.0} &&
                    result.value().attachment.opacity == 0.4,
                "target exit motion was not applied");
        }},
        Case{"target without transition remains unchanged", [] {
            auto input = target();
            input.definition.transition.reset();
            const auto result = resolveTargetMotion(
                input,
                0);
            require(
                result.hasValue() &&
                    result.value() == input,
                "static target changed during motion resolution");
        }},
        Case{"backward time and non-finite result fail closed", [] {
            require(
                !resolveTargetMotion(target(), 999),
                "backward time reached target motion");
            auto malformed = target();
            malformed.attachment.globalGeometry.x =
                std::numeric_limits<double>::max();
            malformed.definition.transition->travel =
                std::numeric_limits<double>::max();
            require(
                !resolveTargetMotion(malformed, 1000),
                "non-finite translated geometry reached presentation");
        }},
    });
}
