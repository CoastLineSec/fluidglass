#include "v2/targets/TargetMotion.hpp"

#include "v2/render/TransitionMotion.hpp"

#include <cmath>

namespace hfg::v2 {

Result<ResolvedTarget>
resolveTargetMotion(
    const ResolvedTarget& target,
    std::uint64_t nowMs) {
    auto resolved = target;
    resolved.transitionActive = false;
    if (!target.definition.transition)
        return Result<ResolvedTarget>::success(
            std::move(resolved));

    auto motion = resolveTransitionMotion(
        *target.definition.transition,
        target.transitionAnchorMs,
        nowMs);
    if (!motion)
        return Result<ResolvedTarget>::failure(
            motion.error());
    resolved.attachment.globalGeometry.x +=
        motion.value().translation.x;
    resolved.attachment.globalGeometry.y +=
        motion.value().translation.y;
    resolved.attachment.opacity *=
        motion.value().opacity;
    if (!std::isfinite(
            resolved.attachment.globalGeometry.x) ||
        !std::isfinite(
            resolved.attachment.globalGeometry.y) ||
        !std::isfinite(resolved.attachment.opacity))
        return Result<ResolvedTarget>::failure({
            ErrorCode::InvalidTarget,
            "target.transition",
            "target transition produced non-finite attachment state",
        });
    resolved.transitionActive = motion.value().active;
    return Result<ResolvedTarget>::success(
        std::move(resolved));
}

} // namespace hfg::v2
