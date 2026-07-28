#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/HyprlandCaptureResource.hpp"
#include "v2/render/OutputGeneration.hpp"

#include <span>

namespace hfg::v2 {

[[nodiscard]] Result<void> captureCurrentFramebuffer(
    HyprlandCaptureResource& resource,
    const OutputGeneration& output,
    std::span<const PixelRect> outputDamage);

} // namespace hfg::v2
