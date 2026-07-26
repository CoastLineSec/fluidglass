#include "v2/render/HyprlandCaptureFormat.hpp"

#include <hyprgraphics/egl/Egl.hpp>

namespace hfg::v2 {

Result<CaptureFormatLayout>
validateHyprlandCaptureFormat(
    std::uint32_t renderFormat,
    const Hyprgraphics::Egl::SPixelFormat* format) {
    if (renderFormat == 0U)
        return Result<CaptureFormatLayout>::failure({
            .code = ErrorCode::InvalidRequest,
            .path = "render_format",
            .message = "render format must not be zero",
        });
    if (!format ||
        format->drmFormat != renderFormat ||
        format->glFormat == 0 ||
        format->glType == 0 ||
        format->bytesPerBlock == 0U ||
        format->bytesPerBlock > 64U ||
        Hyprgraphics::Egl::pixelsPerBlock(format) != 1 ||
        Hyprgraphics::Egl::minStride(format, 1) !=
            static_cast<int>(format->bytesPerBlock))
        return Result<CaptureFormatLayout>::failure({
            .code = ErrorCode::UnsupportedOperation,
            .path = "render_format",
            .message = "render format has no exact capturable pixel layout",
        });
    return Result<CaptureFormatLayout>::success({
        .renderFormat = renderFormat,
        .bytesPerPixel = format->bytesPerBlock,
    });
}

Result<CaptureFormatLayout>
hyprlandCaptureFormatLayout(std::uint32_t renderFormat) {
    return validateHyprlandCaptureFormat(
        renderFormat,
        Hyprgraphics::Egl::getPixelFormatFromDRM(
            renderFormat));
}

} // namespace hfg::v2
