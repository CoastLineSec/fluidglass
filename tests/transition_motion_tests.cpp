#include "TestHarness.hpp"

#include "v2/render/TransitionMotion.hpp"

#include <cmath>
#include <limits>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

constexpr double EPSILON = 1e-7;

bool near(double left, double right) {
    return std::abs(left - right) <= EPSILON;
}

Transition transition(
    TransitionPhase phase = TransitionPhase::Enter,
    TransitionEdge edge = TransitionEdge::Top) {
    return {
        .id = "motion-1",
        .phase = phase,
        .edge = edge,
        .durationMs = 200,
        .elapsedMs = 0,
        .travel = 40.0,
        .easing = {},
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"linear enter advances from offset to rest", [] {
            const auto start = resolveTransitionMotion(
                transition(),
                1000,
                1000);
            const auto middle = resolveTransitionMotion(
                transition(),
                1000,
                1100);
            const auto end = resolveTransitionMotion(
                transition(),
                1000,
                1200);
            require(
                start.hasValue() &&
                    start.value().translation ==
                        Point{0.0, -40.0} &&
                    start.value().opacity == 0.0 &&
                    start.value().active,
                "enter start changed");
            require(
                middle.hasValue() &&
                    near(middle.value().linearProgress, 0.5) &&
                    near(middle.value().translation.y, -20.0) &&
                    near(middle.value().opacity, 0.5) &&
                    middle.value().active,
                "enter midpoint changed");
            require(
                end.hasValue() &&
                    end.value().translation ==
                        Point{} &&
                    end.value().opacity == 1.0 &&
                    !end.value().active,
                "enter completion changed");
        }},
        Case{"linear exit advances from rest to offset", [] {
            const auto motion = resolveTransitionMotion(
                transition(
                    TransitionPhase::Exit,
                    TransitionEdge::Right),
                1000,
                1100);
            require(
                motion.hasValue() &&
                    near(motion.value().translation.x, 20.0) &&
                    motion.value().translation.y == 0.0 &&
                    near(motion.value().opacity, 0.5),
                "exit midpoint changed");
        }},
        Case{"all edges preserve signed direction", [] {
            for (const auto& [edge, expected] : {
                     std::pair{
                         TransitionEdge::Top,
                         Point{0.0, -20.0}},
                     {TransitionEdge::Bottom,
                      Point{0.0, 20.0}},
                     {TransitionEdge::Left,
                      Point{-20.0, 0.0}},
                     {TransitionEdge::Right,
                      Point{20.0, 0.0}},
                 }) {
                const auto motion = resolveTransitionMotion(
                    transition(
                        TransitionPhase::Enter,
                        edge),
                    1000,
                    1100);
                require(
                    motion.hasValue() &&
                        near(motion.value().translation.x, expected.x) &&
                        near(motion.value().translation.y, expected.y),
                    "transition edge direction changed");
            }
        }},
        Case{"client elapsed time anchors compositor progress", [] {
            auto input = transition();
            input.elapsedMs = 50;
            const auto motion = resolveTransitionMotion(
                input,
                1000,
                1050);
            require(
                motion.hasValue() &&
                    near(motion.value().linearProgress, 0.5),
                "client elapsed time was not advanced from its anchor");
        }},
        Case{"elapsed time saturates without overflow", [] {
            auto input = transition();
            input.elapsedMs = 199;
            require(
                transitionElapsedAt(
                    input,
                    1,
                    std::numeric_limits<std::uint64_t>::max()) ==
                    200,
                "elapsed time did not saturate at duration");
        }},
        Case{"single Bezier segment is solved by x", [] {
            auto input = transition();
            input.easing.push_back({
                .control1X = 0.2,
                .control1Y = 0.0,
                .control2X = 0.3,
                .control2Y = 1.0,
                .endX = 1.0,
                .endY = 1.0,
            });
            const auto motion = resolveTransitionMotion(
                input,
                1000,
                1100);
            require(
                motion.hasValue() &&
                    motion.value().easedProgress > 0.5 &&
                    motion.value().easedProgress < 1.0 &&
                    motion.value().translation.y > -20.0,
                "Bezier easing was treated as linear");
        }},
        Case{"segmented Bezier selects the matching x interval", [] {
            auto input = transition();
            input.easing = {
                {
                    .control1X = 0.1,
                    .control1Y = 0.0,
                    .control2X = 0.4,
                    .control2Y = 0.2,
                    .endX = 0.5,
                    .endY = 0.25,
                },
                {
                    .control1X = 0.6,
                    .control1Y = 0.3,
                    .control2X = 0.8,
                    .control2Y = 1.0,
                    .endX = 1.0,
                    .endY = 1.0,
                },
            };
            const auto first = resolveTransitionMotion(
                input,
                1000,
                1050);
            const auto second = resolveTransitionMotion(
                input,
                1000,
                1150);
            require(
                first.hasValue() &&
                    first.value().easedProgress < 0.25 &&
                    second.hasValue() &&
                    second.value().easedProgress > 0.25,
                "segmented easing did not preserve segment endpoints");
        }},
        Case{"Bezier overshoot affects motion but not opacity", [] {
            auto input = transition();
            input.easing.push_back({
                .control1X = 0.2,
                .control1Y = 0.0,
                .control2X = 0.8,
                .control2Y = 2.0,
                .endX = 1.0,
                .endY = 1.0,
            });
            const auto motion = resolveTransitionMotion(
                input,
                1000,
                1150);
            require(
                motion.hasValue() &&
                    motion.value().easedProgress > 1.0 &&
                    motion.value().translation.y > 0.0 &&
                    motion.value().opacity == 1.0,
                "bounded easing overshoot was discarded or leaked into alpha");
        }},
        Case{"backward monotonic time fails closed", [] {
            const auto motion = resolveTransitionMotion(
                transition(),
                1000,
                999);
            require(
                !motion &&
                    motion.error().code ==
                        ErrorCode::StaleGeneration,
                "backward time reached motion evaluation");
        }},
        Case{"malformed timing and easing fail closed", [] {
            auto zeroDuration = transition();
            zeroDuration.durationMs = 0;
            require(
                !resolveTransitionMotion(
                    zeroDuration,
                    1000,
                    1000),
                "zero duration reached motion evaluation");

            auto nonFinite = transition();
            nonFinite.travel =
                std::numeric_limits<double>::infinity();
            require(
                !resolveTransitionMotion(
                    nonFinite,
                    1000,
                    1000),
                "non-finite travel reached motion evaluation");

            auto nonMonotonic = transition();
            nonMonotonic.easing.push_back({
                .control1X = -0.1,
                .control1Y = 0.0,
                .control2X = 0.8,
                .control2Y = 1.0,
                .endX = 1.0,
                .endY = 1.0,
            });
            require(
                !resolveTransitionMotion(
                    nonMonotonic,
                    1000,
                    1000),
                "non-monotonic Bezier reached motion evaluation");
        }},
    });
}
