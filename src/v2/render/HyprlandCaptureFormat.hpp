#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/CaptureScene.hpp"

#include <hyprgraphics/egl/Egl.hpp>

#include <cstdint>

namespace hfg::v2 {

[[nodiscard]] Result<CaptureFormatLayout>
validateHyprlandCaptureFormat(
    std::uint32_t renderFormat,
    const Hyprgraphics::Egl::SPixelFormat* pixelFormat);

[[nodiscard]] Result<CaptureFormatLayout>
hyprlandCaptureFormatLayout(std::uint32_t renderFormat);

} // namespace hfg::v2
