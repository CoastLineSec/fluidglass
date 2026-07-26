#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Target.hpp"
#include "v2/render/Geometry.hpp"

#include <cstdint>

namespace hfg::v2 {

struct TransitionMotion {
    double linearProgress = 0.0;
    double easedProgress = 0.0;
    Point  translation;
    double opacity = 1.0;
    bool   active = false;

    friend bool operator==(
        const TransitionMotion&,
        const TransitionMotion&) = default;
};

[[nodiscard]] Result<TransitionMotion>
resolveTransitionMotion(
    const Transition& transition,
    std::uint64_t anchorMs,
    std::uint64_t nowMs);

[[nodiscard]] std::uint64_t transitionElapsedAt(
    const Transition& transition,
    std::uint64_t anchorMs,
    std::uint64_t nowMs) noexcept;

} // namespace hfg::v2
