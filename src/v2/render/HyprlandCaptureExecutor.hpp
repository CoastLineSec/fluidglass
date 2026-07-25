#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/HyprlandCaptureResource.hpp"
#include "v2/render/OutputGeneration.hpp"

namespace hfg::v2 {

[[nodiscard]] Result<void> captureCurrentFramebuffer(
    const HyprlandCaptureResource& resource,
    const OutputGeneration& output);

} // namespace hfg::v2
