#include "v2/render/TransitionMotion.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

constexpr double MAX_EASING_VALUE = 16.0;
constexpr double ENDPOINT_EPSILON = 1e-9;
constexpr std::size_t BISECTION_STEPS = 64U;

Result<TransitionMotion> failure(
    std::string path,
    std::string message,
    ErrorCode code = ErrorCode::InvalidTarget) {
    return Result<TransitionMotion>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

bool validCoordinate(double value) {
    return std::isfinite(value) &&
        std::abs(value) <= 1'000'000.0;
}

Result<void> validateMotionTransition(
    const Transition& transition) {
    if (transition.durationMs == 0U ||
        transition.durationMs > Limits::MAX_TRANSITION_MS)
        return Result<void>::failure({
            ErrorCode::InvalidTarget,
            "transition.duration_ms",
            "transition duration is outside the supported limit",
        });
    if (transition.elapsedMs > transition.durationMs)
        return Result<void>::failure({
            ErrorCode::InvalidTarget,
            "transition.elapsed_ms",
            "transition elapsed time exceeds its duration",
        });
    if (!validCoordinate(transition.travel) ||
        transition.travel < 0.0)
        return Result<void>::failure({
            ErrorCode::InvalidTarget,
            "transition.travel",
            "expected a finite non-negative travel distance",
        });
    switch (transition.phase) {
        case TransitionPhase::Enter:
        case TransitionPhase::Exit:
            break;
        default:
            return Result<void>::failure({
                ErrorCode::InvalidTarget,
                "transition.phase",
                "unsupported transition phase",
            });
    }
    switch (transition.edge) {
        case TransitionEdge::Top:
        case TransitionEdge::Bottom:
        case TransitionEdge::Left:
        case TransitionEdge::Right:
            break;
        default:
            return Result<void>::failure({
                ErrorCode::InvalidTarget,
                "transition.edge",
                "unsupported transition edge",
            });
    }
    if (transition.easing.size() >
        Limits::MAX_BEZIER_SEGMENTS)
        return Result<void>::failure({
            ErrorCode::ResourceLimited,
            "transition.easing",
            "transition exceeds the Bezier segment limit",
        });

    double previousEndX = 0.0;
    for (std::size_t index = 0;
         index < transition.easing.size();
         ++index) {
        const auto& segment = transition.easing[index];
        const auto path =
            "transition.easing[" +
            std::to_string(index) + "]";
        for (const auto value : {
                 segment.control1X,
                 segment.control1Y,
                 segment.control2X,
                 segment.control2Y,
                 segment.endX,
                 segment.endY,
             })
            if (!std::isfinite(value) ||
                std::abs(value) > MAX_EASING_VALUE)
                return Result<void>::failure({
                    ErrorCode::InvalidTarget,
                    path,
                    "expected finite bounded Bezier coordinates",
                });
        if (segment.endX <= previousEndX ||
            segment.endX > 1.0 ||
            segment.control1X < previousEndX ||
            segment.control1X > segment.endX ||
            segment.control2X < previousEndX ||
            segment.control2X > segment.endX)
            return Result<void>::failure({
                ErrorCode::InvalidTarget,
                path,
                "Bezier x coordinates are not monotonic within their segment",
            });
        previousEndX = segment.endX;
    }
    if (!transition.easing.empty()) {
        const auto& end = transition.easing.back();
        if (std::abs(end.endX - 1.0) >
                ENDPOINT_EPSILON ||
            std::abs(end.endY - 1.0) >
                ENDPOINT_EPSILON)
            return Result<void>::failure({
                ErrorCode::InvalidTarget,
                "transition.easing",
                "Bezier easing must end at (1, 1)",
            });
    }
    return Result<void>::success();
}

double cubic(
    double start,
    double control1,
    double control2,
    double end,
    double t) {
    const auto inverse = 1.0 - t;
    return inverse * inverse * inverse * start +
        3.0 * inverse * inverse * t * control1 +
        3.0 * inverse * t * t * control2 +
        t * t * t * end;
}

double ease(
    const Transition& transition,
    double progress) {
    if (transition.easing.empty() ||
        progress <= 0.0 ||
        progress >= 1.0)
        return progress;

    double startX = 0.0;
    double startY = 0.0;
    for (const auto& segment : transition.easing) {
        if (progress > segment.endX) {
            startX = segment.endX;
            startY = segment.endY;
            continue;
        }
        double low = 0.0;
        double high = 1.0;
        for (std::size_t step = 0;
             step < BISECTION_STEPS;
             ++step) {
            const auto middle = (low + high) * 0.5;
            const auto x = cubic(
                startX,
                segment.control1X,
                segment.control2X,
                segment.endX,
                middle);
            if (x < progress)
                low = middle;
            else
                high = middle;
        }
        const auto t = (low + high) * 0.5;
        return cubic(
            startY,
            segment.control1Y,
            segment.control2Y,
            segment.endY,
            t);
    }
    return 1.0;
}

Point translationFor(
    TransitionEdge edge,
    double distance) {
    switch (edge) {
        case TransitionEdge::Top:
            return {.x = 0.0, .y = -distance};
        case TransitionEdge::Bottom:
            return {.x = 0.0, .y = distance};
        case TransitionEdge::Left:
            return {.x = -distance, .y = 0.0};
        case TransitionEdge::Right:
            return {.x = distance, .y = 0.0};
    }
    return {};
}

} // namespace

std::uint64_t transitionElapsedAt(
    const Transition& transition,
    std::uint64_t anchorMs,
    std::uint64_t nowMs) noexcept {
    const auto delta = nowMs >= anchorMs
        ? nowMs - anchorMs
        : 0U;
    const auto remaining =
        transition.elapsedMs < transition.durationMs
        ? transition.durationMs - transition.elapsedMs
        : 0U;
    return transition.elapsedMs +
        std::min(delta, remaining);
}

Result<TransitionMotion>
resolveTransitionMotion(
    const Transition& transition,
    std::uint64_t anchorMs,
    std::uint64_t nowMs) {
    if (auto valid = validateMotionTransition(transition);
        !valid)
        return Result<TransitionMotion>::failure(valid.error());
    if (nowMs < anchorMs)
        return failure(
            "transition.anchor_ms",
            "current monotonic time predates the transition anchor",
            ErrorCode::StaleGeneration);

    const auto elapsed = transitionElapsedAt(
        transition,
        anchorMs,
        nowMs);
    const auto linear = static_cast<double>(elapsed) /
        static_cast<double>(transition.durationMs);
    const auto eased = ease(transition, linear);
    if (!std::isfinite(eased))
        return failure(
            "transition.easing",
            "Bezier easing produced a non-finite result");

    const auto entering =
        transition.phase == TransitionPhase::Enter;
    const auto distance =
        (entering ? 1.0 - eased : eased) *
        transition.travel;
    if (!std::isfinite(distance))
        return failure(
            "transition.travel",
            "transition distance is not finite");
    const auto opacity = std::clamp(
        entering ? eased : 1.0 - eased,
        0.0,
        1.0);
    return Result<TransitionMotion>::success({
        .linearProgress = linear,
        .easedProgress = eased,
        .translation = translationFor(
            transition.edge,
            distance),
        .opacity = opacity,
        .active = elapsed < transition.durationMs,
    });
}

} // namespace hfg::v2
