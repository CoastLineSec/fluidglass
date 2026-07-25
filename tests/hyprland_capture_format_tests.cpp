#include "TestHarness.hpp"

#include "v2/render/HyprlandCaptureFormat.hpp"

#include <drm_fourcc.h>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

Hyprgraphics::Egl::SPixelFormat format(
    std::uint32_t drmFormat,
    std::uint32_t bytesPerPixel) {
    return {
        .drmFormat = drmFormat,
        .glInternalFormat = GL_RGBA8,
        .glFormat = GL_RGBA,
        .glType = GL_UNSIGNED_BYTE,
        .withAlpha = true,
        .alphaStripped = 0,
        .bytesPerBlock = bytesPerPixel,
        .blockSize = {1.0, 1.0},
        .swizzle = std::nullopt,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"common SDR formats preserve four-byte layouts", [] {
            const auto argbFormat = format(
                DRM_FORMAT_ARGB8888,
                4);
            const auto abgrFormat = format(
                DRM_FORMAT_ABGR8888,
                4);
            const auto argb = validateHyprlandCaptureFormat(
                DRM_FORMAT_ARGB8888,
                &argbFormat);
            const auto abgr = validateHyprlandCaptureFormat(
                DRM_FORMAT_ABGR8888,
                &abgrFormat);
            require(argb.hasValue() &&
                        argb.value().bytesPerPixel == 4U,
                    "ARGB8888 layout changed");
            require(abgr.hasValue() &&
                        abgr.value().bytesPerPixel == 4U,
                    "ABGR8888 layout changed");
        }},
        Case{"FP16 format preserves its exact eight-byte layout", [] {
            auto fp16 = format(
                DRM_FORMAT_ABGR16161616F,
                8);
            fp16.glInternalFormat = GL_RGBA16F;
            fp16.glType = GL_HALF_FLOAT;
            const auto result = validateHyprlandCaptureFormat(
                DRM_FORMAT_ABGR16161616F,
                &fp16);
            require(result.hasValue(),
                    "ABGR16161616F is not capturable");
            require(result.value().bytesPerPixel == 8U,
                    "FP16 layout was narrowed");
        }},
        Case{"invalid and mismatched formats fail closed", [] {
            auto argb = format(DRM_FORMAT_ARGB8888, 4);
            require(!validateHyprlandCaptureFormat(
                        0U,
                        &argb),
                    "zero DRM format was accepted");
            require(!validateHyprlandCaptureFormat(
                        DRM_FORMAT_ABGR8888,
                        &argb),
                    "mismatched DRM layout was accepted");
            require(!validateHyprlandCaptureFormat(
                        DRM_FORMAT_ARGB8888,
                        nullptr),
                    "missing runtime layout was accepted");
        }},
        Case{"compressed and malformed layouts fail closed", [] {
            auto compressed = format(DRM_FORMAT_ARGB8888, 4);
            compressed.blockSize = {2.0, 2.0};
            require(!validateHyprlandCaptureFormat(
                        DRM_FORMAT_ARGB8888,
                        &compressed),
                    "block-compressed layout was accepted");

            auto missingGl = format(DRM_FORMAT_ARGB8888, 4);
            missingGl.glType = 0;
            require(!validateHyprlandCaptureFormat(
                        DRM_FORMAT_ARGB8888,
                        &missingGl),
                    "unrepresentable GL layout was accepted");

            auto oversized = format(DRM_FORMAT_ARGB8888, 65);
            require(!validateHyprlandCaptureFormat(
                        DRM_FORMAT_ARGB8888,
                        &oversized),
                    "oversized pixel layout was accepted");
        }},
    });
}
