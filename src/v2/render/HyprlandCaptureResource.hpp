#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/CapturePlan.hpp"

#include <GLES3/gl32.h>

#include <memory>

namespace hfg::v2 {

class HyprlandCaptureResource {
  public:
    [[nodiscard]] static Result<std::unique_ptr<HyprlandCaptureResource>> allocate(
        CapturePlan plan);

    ~HyprlandCaptureResource();

    HyprlandCaptureResource(const HyprlandCaptureResource&) = delete;
    HyprlandCaptureResource& operator=(const HyprlandCaptureResource&) = delete;
    HyprlandCaptureResource(HyprlandCaptureResource&&) = delete;
    HyprlandCaptureResource& operator=(HyprlandCaptureResource&&) = delete;

    [[nodiscard]] const CapturePlan& plan() const noexcept;
    [[nodiscard]] GLuint framebuffer() const noexcept;
    [[nodiscard]] GLuint texture() const noexcept;
    [[nodiscard]] bool allocated() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

    void markInitialized() noexcept;
    void invalidate() noexcept;
    void release() noexcept;

  private:
    HyprlandCaptureResource(
        CapturePlan plan,
        GLuint framebuffer,
        GLuint texture);

    CapturePlan m_plan;
    GLuint      m_framebuffer = 0;
    GLuint      m_texture = 0;
    bool        m_initialized = false;
};

} // namespace hfg::v2
