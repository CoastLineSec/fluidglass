#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Target.hpp"

#include <cstdint>

namespace hfg::v2 {

struct ResolvedShapeMotion {
    Shape shape;
    bool  active = false;

    friend bool operator==(
        const ResolvedShapeMotion&,
        const ResolvedShapeMotion&) = default;
};

[[nodiscard]] Result<ResolvedShapeMotion>
resolveShapeMotion(
    const Shape& shape,
    std::uint64_t anchorMs,
    std::uint64_t nowMs);

} // namespace hfg::v2
