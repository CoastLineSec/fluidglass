#pragma once

#include <string_view>

namespace hfg::v2 {

[[nodiscard]] std::string_view glassVertexShaderSource() noexcept;
[[nodiscard]] std::string_view glassFragmentShaderSource() noexcept;

} // namespace hfg::v2
