#include "TestHarness.hpp"

#include "v2/render/ShapeMotion.hpp"

#include <limits>
#include <variant>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

Shape shape(
    TransitionPhase phase = TransitionPhase::Enter,
    TransitionEdge edge = TransitionEdge::Top) {
    CompoundShape compound;
    compound.parts.push_back({
        .rect = {100.0, 100.0, 80.0, 40.0},
        .corners = {},
        .junctions = {},
        .materialExtent =
            Rect{90.0, 90.0, 100.0, 60.0},
        .transition = PartTransition{
            .motion = Transition{
                .id = "part-1",
                .phase = phase,
                .edge = edge,
                .durationMs = 200,
                .elapsedMs = 0,
                .travel = 40.0,
                .easing = {},
            },
            .protrusion = 20.0,
        },
        .opacity = 0.8,
    });
    return compound;
}

const CompoundPart& part(
    const ResolvedShapeMotion& motion) {
    return std::get<CompoundShape>(
        motion.shape).parts.front();
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"top enter translates collapses and fades part", [] {
            const auto result = resolveShapeMotion(
                shape(),
                1000,
                1100);
            require(
                result.hasValue() &&
                    result.value().active,
                "active part transition was lost");
            const auto& resolved = part(result.value());
            require(
                resolved.rect ==
                        Rect{100.0, 90.0, 80.0, 30.0} &&
                    resolved.materialExtent ==
                        Rect{90.0, 70.0, 100.0, 60.0} &&
                    resolved.opacity == 0.4 &&
                    !resolved.transition,
                "top enter part motion changed");
        }},
        Case{"bottom enter grows toward its resting top edge", [] {
            const auto result = resolveShapeMotion(
                shape(
                    TransitionPhase::Enter,
                    TransitionEdge::Bottom),
                1000,
                1100);
            require(
                result.hasValue() &&
                    part(result.value()).rect ==
                        Rect{100.0, 120.0, 80.0, 30.0},
                "bottom enter collapse anchor changed");
        }},
        Case{"left and right collapse along width", [] {
            const auto left = resolveShapeMotion(
                shape(
                    TransitionPhase::Enter,
                    TransitionEdge::Left),
                1000,
                1100);
            const auto right = resolveShapeMotion(
                shape(
                    TransitionPhase::Enter,
                    TransitionEdge::Right),
                1000,
                1100);
            require(
                left.hasValue() &&
                    part(left.value()).rect ==
                        Rect{90.0, 100.0, 70.0, 40.0} &&
                    right.hasValue() &&
                    part(right.value()).rect ==
                        Rect{120.0, 100.0, 70.0, 40.0},
                "horizontal part collapse changed");
        }},
        Case{"exit collapses as the part leaves", [] {
            const auto result = resolveShapeMotion(
                shape(
                    TransitionPhase::Exit,
                    TransitionEdge::Bottom),
                1000,
                1100);
            require(
                result.hasValue() &&
                    part(result.value()).rect ==
                        Rect{100.0, 120.0, 80.0, 30.0} &&
                    part(result.value()).opacity == 0.4,
                "exit collapse changed");
        }},
        Case{"completed enter settles exact source shape", [] {
            const auto source = shape();
            const auto result = resolveShapeMotion(
                source,
                1000,
                1200);
            require(result.hasValue(), "settled shape failed");
            auto expected = std::get<CompoundShape>(source);
            expected.parts.front().transition.reset();
            require(
                result.value().shape == Shape(expected) &&
                    !result.value().active,
                "completed part did not settle");
        }},
        Case{"static shapes and parts remain unchanged", [] {
            const Shape rounded =
                RoundedRectShape{.radius = 20.0};
            const auto roundedResult = resolveShapeMotion(
                rounded,
                1000,
                0);
            require(
                roundedResult.hasValue() &&
                    roundedResult.value().shape == rounded,
                "static rounded shape changed");

            auto compound =
                std::get<CompoundShape>(shape());
            compound.parts.front().transition.reset();
            const auto compoundResult = resolveShapeMotion(
                compound,
                1000,
                0);
            require(
                compoundResult.hasValue() &&
                    compoundResult.value().shape ==
                        Shape(compound),
                "static compound part changed");
        }},
        Case{"collapse is bounded by current part size", [] {
            auto input =
                std::get<CompoundShape>(shape());
            input.parts.front().transition->protrusion =
                1000.0;
            const auto result = resolveShapeMotion(
                input,
                1000,
                1000);
            require(
                result.hasValue() &&
                    part(result.value()).rect.height == 0.0,
                "part collapse produced negative size");
        }},
        Case{"backward time and non-finite collapse fail closed", [] {
            require(
                !resolveShapeMotion(shape(), 1000, 999),
                "backward time reached shape motion");
            auto input =
                std::get<CompoundShape>(shape());
            input.parts.front().transition->protrusion =
                std::numeric_limits<double>::infinity();
            require(
                !resolveShapeMotion(input, 1000, 1000),
                "non-finite collapse reached draw shape");
        }},
    });
}
