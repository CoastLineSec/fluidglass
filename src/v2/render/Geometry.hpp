#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Target.hpp"
#include "v2/render/OutputGeneration.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace hfg::v2 {

struct Point {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const Point&, const Point&) = default;
};

struct PixelRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;

    friend bool operator==(const PixelRect&, const PixelRect&) = default;
};

struct PixelSize {
    std::int32_t width = 0;
    std::int32_t height = 0;

    friend bool operator==(const PixelSize&, const PixelSize&) = default;
};

struct MappedGeometry {
    Rect                 clippedGlobal;
    Rect                 outputLocal;
    Rect                 bufferRect;
    PixelRect            coverage;
    std::array<Point, 4> semanticCorners;

    friend bool operator==(const MappedGeometry&, const MappedGeometry&) = default;
};

[[nodiscard]] Result<std::optional<MappedGeometry>> mapGlobalLogicalRect(
    const Rect& globalRect,
    const OutputGeneration& output);

[[nodiscard]] Result<PixelSize> outputOrientedPixelSize(
    const OutputSnapshot& output);

[[nodiscard]] Result<PixelRect> mapOutputPixelRectToBuffer(
    const PixelRect& rect,
    const OutputSnapshot& output);

[[nodiscard]] Result<PixelRect> mapBufferPixelRectToOutput(
    const PixelRect& rect,
    const OutputSnapshot& output);

} // namespace hfg::v2
