#pragma once

#include "v2/core/Result.hpp"

#include <hyprland/src/render/Shader.hpp>

#include <GLES3/gl32.h>

#include <cstdint>

namespace hfg::v2 {

/**
 * Separable Gaussian frost for the captured backdrop.
 *
 * Two 1D Gaussian passes run at half resolution through a dedicated ping-pong
 * pair, leaving the glass shader a single texture tap. The caller retains the
 * sharp capture as a non-fatal fallback when blur is unavailable.
 */
class HyprlandGlassBlur {
public:
  ~HyprlandGlassBlur();

  HyprlandGlassBlur() = default;
  HyprlandGlassBlur(const HyprlandGlassBlur &) = delete;
  HyprlandGlassBlur &operator=(const HyprlandGlassBlur &) = delete;
  HyprlandGlassBlur(HyprlandGlassBlur &&) = delete;
  HyprlandGlassBlur &operator=(HyprlandGlassBlur &&) = delete;

  /**
   * Frosts `sourceTexture` and returns the blurred texture.
   *
   * `radiusPixels` is the full-resolution blur reach; the passes run at half
   * resolution, so the sampled radius is halved with it. The result keeps the
   * source's normalized coordinate space, so callers substitute it for the
   * capture without touching their own UV mapping.
   */
  [[nodiscard]] Result<GLuint> execute(GLuint sourceTexture,
                                       std::int32_t sourceWidth,
                                       std::int32_t sourceHeight,
                                       double radiusPixels);

  void reset() noexcept;

private:
  [[nodiscard]] Result<void> ensureShader();
  [[nodiscard]] Result<void> ensureTargets(std::int32_t width,
                                           std::int32_t height);
  void releaseTargets() noexcept;

  SP<CShader> m_shader;
  GLint m_source = -1;
  GLint m_direction = -1;
  GLint m_radius = -1;

  GLuint m_texture[2] = {0U, 0U};
  GLuint m_framebuffer[2] = {0U, 0U};
  std::int32_t m_width = 0;
  std::int32_t m_height = 0;
};

} // namespace hfg::v2
