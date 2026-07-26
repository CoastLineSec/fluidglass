#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/GlassDrawPlan.hpp"
#include "v2/render/HyprlandCaptureResource.hpp"
#include "v2/render/HyprlandGlassShader.hpp"
#include "v2/render/OutputGeneration.hpp"

#include <cstdint>

namespace hfg::v2 {

[[nodiscard]] Result<bool> drawGlass(const GlassDrawPlan &plan,
                                     std::uint64_t resourceToken,
                                     const HyprlandCaptureResource &resource,
                                     const OutputGeneration &output,
                                     HyprlandGlassShader &shader);

} // namespace hfg::v2
