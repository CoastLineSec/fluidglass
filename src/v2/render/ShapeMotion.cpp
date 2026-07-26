#include "v2/render/ShapeMotion.hpp"

#include "v2/render/TransitionMotion.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

namespace hfg::v2 {
namespace {

void translateRect(Rect& rect, const Point& translation) {
    rect.x += translation.x;
    rect.y += translation.y;
}

void collapseRect(
    Rect& rect,
    TransitionEdge edge,
    double collapse) {
    switch (edge) {
        case TransitionEdge::Top: {
            const auto amount =
                std::min(collapse, rect.height);
            rect.y += amount;
            rect.height -= amount;
            break;
        }
        case TransitionEdge::Bottom:
            rect.height -=
                std::min(collapse, rect.height);
            break;
        case TransitionEdge::Left: {
            const auto amount =
                std::min(collapse, rect.width);
            rect.x += amount;
            rect.width -= amount;
            break;
        }
        case TransitionEdge::Right:
            rect.width -=
                std::min(collapse, rect.width);
            break;
    }
}

Result<CompoundPart> resolvePart(
    const CompoundPart& part,
    std::uint64_t anchorMs,
    std::uint64_t nowMs,
    bool& active) {
    auto resolved = part;
    if (!part.transition)
        return Result<CompoundPart>::success(
            std::move(resolved));
    auto motion = resolveTransitionMotion(
        part.transition->motion,
        anchorMs,
        nowMs);
    if (!motion)
        return Result<CompoundPart>::failure(
            motion.error());

    translateRect(
        resolved.rect,
        motion.value().translation);
    if (resolved.materialExtent)
        translateRect(
            *resolved.materialExtent,
            motion.value().translation);
    const auto normalizedProgress = std::clamp(
        motion.value().easedProgress,
        0.0,
        1.0);
    const auto collapseProgress =
        part.transition->motion.phase ==
                TransitionPhase::Enter
            ? 1.0 - normalizedProgress
            : normalizedProgress;
    const auto collapse =
        part.transition->protrusion *
        collapseProgress;
    if (!std::isfinite(collapse))
        return Result<CompoundPart>::failure({
            ErrorCode::InvalidTarget,
            "shape.parts.transition.protrusion",
            "part transition produced a non-finite collapse",
        });
    collapseRect(
        resolved.rect,
        part.transition->motion.edge,
        collapse);
    resolved.opacity *= motion.value().opacity;
    if (!std::isfinite(resolved.rect.x) ||
        !std::isfinite(resolved.rect.y) ||
        !std::isfinite(resolved.rect.width) ||
        !std::isfinite(resolved.rect.height) ||
        resolved.rect.width < 0.0 ||
        resolved.rect.height < 0.0 ||
        !std::isfinite(resolved.opacity))
        return Result<CompoundPart>::failure({
            ErrorCode::InvalidTarget,
            "shape.parts.transition",
            "part transition produced invalid geometry or opacity",
        });
    resolved.transition.reset();
    active = active || motion.value().active;
    return Result<CompoundPart>::success(
        std::move(resolved));
}

} // namespace

Result<ResolvedShapeMotion>
resolveShapeMotion(
    const Shape& shape,
    std::uint64_t anchorMs,
    std::uint64_t nowMs) {
    return std::visit(
        [anchorMs, nowMs](
            const auto& value)
            -> Result<ResolvedShapeMotion> {
            using T = std::decay_t<decltype(value)>;
            if constexpr (!std::is_same_v<T, CompoundShape>) {
                return Result<ResolvedShapeMotion>::success({
                    .shape = value,
                    .active = false,
                });
            } else {
                auto compound = value;
                compound.parts.clear();
                compound.parts.reserve(value.parts.size());
                bool active = false;
                for (const auto& part : value.parts) {
                    auto resolved = resolvePart(
                        part,
                        anchorMs,
                        nowMs,
                        active);
                    if (!resolved)
                        return Result<ResolvedShapeMotion>::failure(
                            resolved.error());
                    compound.parts.push_back(
                        std::move(resolved.value()));
                }
                return Result<ResolvedShapeMotion>::success({
                    .shape = std::move(compound),
                    .active = active,
                });
            }
        },
        shape);
}

} // namespace hfg::v2
