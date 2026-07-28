#include "v2/render/Geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace hfg::v2 {
namespace {

constexpr double MAX_LOGICAL_VALUE = 1'000'000.0;
constexpr double METRIC_TOLERANCE  = 1.0;

Result<std::optional<MappedGeometry>> invalid(
    std::string path,
    std::string message,
    ErrorCode code = ErrorCode::InvalidRequest) {
    return Result<std::optional<MappedGeometry>>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

template <typename T>
Result<T> mappingFailure(
    std::string path,
    std::string message,
    ErrorCode code = ErrorCode::InvalidRequest) {
    return Result<T>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

bool validCoordinate(double value) {
    return std::isfinite(value) && std::abs(value) <= MAX_LOGICAL_VALUE;
}

bool validSize(double value) {
    return std::isfinite(value) && value > 0.0 && value <= MAX_LOGICAL_VALUE;
}

bool swapsAxes(OutputTransform transform) {
    return transform == OutputTransform::Rotate90 ||
        transform == OutputTransform::Rotate270 ||
        transform == OutputTransform::Flipped90 ||
        transform == OutputTransform::Flipped270;
}

OutputTransform inverseTransform(OutputTransform transform) {
    switch (transform) {
        case OutputTransform::Rotate90:  return OutputTransform::Rotate270;
        case OutputTransform::Rotate270: return OutputTransform::Rotate90;
        default:                         return transform;
    }
}

Rect transformRect(
    const Rect& rect,
    OutputTransform transform,
    double width,
    double height) {
    Rect result;
    if (swapsAxes(transform)) {
        result.width = rect.height;
        result.height = rect.width;
    } else {
        result.width = rect.width;
        result.height = rect.height;
    }

    switch (transform) {
        case OutputTransform::Normal:
            result.x = rect.x;
            result.y = rect.y;
            break;
        case OutputTransform::Rotate90:
            result.x = height - rect.y - rect.height;
            result.y = rect.x;
            break;
        case OutputTransform::Rotate180:
            result.x = width - rect.x - rect.width;
            result.y = height - rect.y - rect.height;
            break;
        case OutputTransform::Rotate270:
            result.x = rect.y;
            result.y = width - rect.x - rect.width;
            break;
        case OutputTransform::Flipped:
            result.x = width - rect.x - rect.width;
            result.y = rect.y;
            break;
        case OutputTransform::Flipped90:
            result.x = rect.y;
            result.y = rect.x;
            break;
        case OutputTransform::Flipped180:
            result.x = rect.x;
            result.y = height - rect.y - rect.height;
            break;
        case OutputTransform::Flipped270:
            result.x = height - rect.y - rect.height;
            result.y = width - rect.x - rect.width;
            break;
    }
    return result;
}

Point transformPoint(
    Point point,
    OutputTransform transform,
    double width,
    double height) {
    switch (transform) {
        case OutputTransform::Normal:
            return point;
        case OutputTransform::Rotate90:
            return {height - point.y, point.x};
        case OutputTransform::Rotate180:
            return {width - point.x, height - point.y};
        case OutputTransform::Rotate270:
            return {point.y, width - point.x};
        case OutputTransform::Flipped:
            return {width - point.x, point.y};
        case OutputTransform::Flipped90:
            return {point.y, point.x};
        case OutputTransform::Flipped180:
            return {point.x, height - point.y};
        case OutputTransform::Flipped270:
            return {height - point.y, width - point.x};
    }
    return point;
}

Rect clampRect(const Rect& rect, double width, double height) {
    const double left = std::clamp(rect.x, 0.0, width);
    const double top = std::clamp(rect.y, 0.0, height);
    const double right = std::clamp(rect.x + rect.width, 0.0, width);
    const double bottom = std::clamp(rect.y + rect.height, 0.0, height);
    return {
        .x = left,
        .y = top,
        .width = std::max(0.0, right - left),
        .height = std::max(0.0, bottom - top),
    };
}

Point clampPoint(Point point, double width, double height) {
    point.x = std::clamp(point.x, 0.0, width);
    point.y = std::clamp(point.y, 0.0, height);
    return point;
}

bool containedPixelRect(
    const PixelRect& rect,
    std::int32_t width,
    std::int32_t height) {
    if (rect.x < 0 || rect.y < 0 ||
        rect.width <= 0 || rect.height <= 0)
        return false;
    const auto right =
        static_cast<std::int64_t>(rect.x) + rect.width;
    const auto bottom =
        static_cast<std::int64_t>(rect.y) + rect.height;
    return right <= width && bottom <= height;
}

PixelRect exactPixelRect(const Rect& rect) {
    return {
        .x = static_cast<std::int32_t>(std::lround(rect.x)),
        .y = static_cast<std::int32_t>(std::lround(rect.y)),
        .width = static_cast<std::int32_t>(std::lround(rect.width)),
        .height = static_cast<std::int32_t>(std::lround(rect.height)),
    };
}

} // namespace

Result<std::optional<MappedGeometry>> mapGlobalLogicalRect(
    const Rect& globalRect,
    const OutputGeneration& output) {
    if (output.generation == 0U)
        return invalid("output.generation", "output generation must not be zero");
    if (auto validation = validateOutputSnapshot(output.snapshot); !validation)
        return Result<std::optional<MappedGeometry>>::failure(validation.error());
    if (!validCoordinate(globalRect.x))
        return invalid("geometry.x", "expected a finite logical coordinate");
    if (!validCoordinate(globalRect.y))
        return invalid("geometry.y", "expected a finite logical coordinate");
    if (!validSize(globalRect.width))
        return invalid("geometry.width", "expected a finite positive logical size");
    if (!validSize(globalRect.height))
        return invalid("geometry.height", "expected a finite positive logical size");

    const auto& snapshot = output.snapshot;
    const double orientedWidth = swapsAxes(snapshot.transform)
        ? static_cast<double>(snapshot.bufferHeight)
        : static_cast<double>(snapshot.bufferWidth);
    const double orientedHeight = swapsAxes(snapshot.transform)
        ? static_cast<double>(snapshot.bufferWidth)
        : static_cast<double>(snapshot.bufferHeight);
    if (std::abs(snapshot.logicalWidth * snapshot.scale - orientedWidth) > METRIC_TOLERANCE ||
        std::abs(snapshot.logicalHeight * snapshot.scale - orientedHeight) > METRIC_TOLERANCE)
        return invalid(
            "output.metrics",
            "logical size, scale and buffer size are inconsistent");

    const double left = std::max(globalRect.x, snapshot.logicalX);
    const double top = std::max(globalRect.y, snapshot.logicalY);
    const double right = std::min(
        globalRect.x + globalRect.width,
        snapshot.logicalX + snapshot.logicalWidth);
    const double bottom = std::min(
        globalRect.y + globalRect.height,
        snapshot.logicalY + snapshot.logicalHeight);
    if (right <= left || bottom <= top)
        return Result<std::optional<MappedGeometry>>::success(std::nullopt);

    const Rect clippedGlobal{
        .x = left,
        .y = top,
        .width = right - left,
        .height = bottom - top,
    };
    const Rect outputLocal{
        .x = clippedGlobal.x - snapshot.logicalX,
        .y = clippedGlobal.y - snapshot.logicalY,
        .width = clippedGlobal.width,
        .height = clippedGlobal.height,
    };
    const Rect scaled{
        .x = outputLocal.x * snapshot.scale,
        .y = outputLocal.y * snapshot.scale,
        .width = outputLocal.width * snapshot.scale,
        .height = outputLocal.height * snapshot.scale,
    };

    const auto inverse = inverseTransform(snapshot.transform);
    const Rect transformed = transformRect(scaled, inverse, orientedWidth, orientedHeight);
    const Rect bufferRect = clampRect(
        transformed,
        static_cast<double>(snapshot.bufferWidth),
        static_cast<double>(snapshot.bufferHeight));
    if (bufferRect.width <= 0.0 || bufferRect.height <= 0.0)
        return Result<std::optional<MappedGeometry>>::success(std::nullopt);

    std::array<Point, 4> corners{
        Point{scaled.x, scaled.y},
        Point{scaled.x + scaled.width, scaled.y},
        Point{scaled.x + scaled.width, scaled.y + scaled.height},
        Point{scaled.x, scaled.y + scaled.height},
    };
    for (auto& corner : corners)
        corner = clampPoint(
            transformPoint(corner, inverse, orientedWidth, orientedHeight),
            static_cast<double>(snapshot.bufferWidth),
            static_cast<double>(snapshot.bufferHeight));

    const double pixelLeft = std::floor(bufferRect.x);
    const double pixelTop = std::floor(bufferRect.y);
    const double pixelRight = std::ceil(bufferRect.x + bufferRect.width);
    const double pixelBottom = std::ceil(bufferRect.y + bufferRect.height);
    if (pixelLeft < 0.0 || pixelTop < 0.0 ||
        pixelRight > static_cast<double>(snapshot.bufferWidth) ||
        pixelBottom > static_cast<double>(snapshot.bufferHeight) ||
        pixelRight > static_cast<double>(std::numeric_limits<std::int32_t>::max()) ||
        pixelBottom > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
        return invalid(
            "geometry.coverage",
            "mapped coverage is outside the output buffer",
            ErrorCode::InternalError);

    return Result<std::optional<MappedGeometry>>::success(MappedGeometry{
        .clippedGlobal = clippedGlobal,
        .outputLocal = outputLocal,
        .bufferRect = bufferRect,
        .coverage = PixelRect{
            .x = static_cast<std::int32_t>(pixelLeft),
            .y = static_cast<std::int32_t>(pixelTop),
            .width = static_cast<std::int32_t>(pixelRight - pixelLeft),
            .height = static_cast<std::int32_t>(pixelBottom - pixelTop),
        },
        .semanticCorners = corners,
    });
}

Result<PixelSize> outputOrientedPixelSize(
    const OutputSnapshot& output) {
    if (auto validation = validateOutputSnapshot(output); !validation)
        return Result<PixelSize>::failure(validation.error());
    return Result<PixelSize>::success({
        .width = static_cast<std::int32_t>(
            swapsAxes(output.transform)
                ? output.bufferHeight
                : output.bufferWidth),
        .height = static_cast<std::int32_t>(
            swapsAxes(output.transform)
                ? output.bufferWidth
                : output.bufferHeight),
    });
}

Result<PixelRect> mapOutputPixelRectToBuffer(
    const PixelRect& rect,
    const OutputSnapshot& output) {
    auto oriented = outputOrientedPixelSize(output);
    if (!oriented)
        return Result<PixelRect>::failure(oriented.error());
    if (!containedPixelRect(
            rect,
            oriented.value().width,
            oriented.value().height))
        return mappingFailure<PixelRect>(
            "rect",
            "output-oriented pixel rectangle lies outside the output");

    const auto mapped = exactPixelRect(transformRect(
        Rect{
            .x = static_cast<double>(rect.x),
            .y = static_cast<double>(rect.y),
            .width = static_cast<double>(rect.width),
            .height = static_cast<double>(rect.height),
        },
        inverseTransform(output.transform),
        static_cast<double>(oriented.value().width),
        static_cast<double>(oriented.value().height)));
    if (!containedPixelRect(
            mapped,
            static_cast<std::int32_t>(output.bufferWidth),
            static_cast<std::int32_t>(output.bufferHeight)))
        return mappingFailure<PixelRect>(
            "rect",
            "mapped buffer pixel rectangle lies outside the output",
            ErrorCode::InternalError);
    return Result<PixelRect>::success(mapped);
}

Result<PixelRect> mapBufferPixelRectToOutput(
    const PixelRect& rect,
    const OutputSnapshot& output) {
    auto oriented = outputOrientedPixelSize(output);
    if (!oriented)
        return Result<PixelRect>::failure(oriented.error());
    if (!containedPixelRect(
            rect,
            static_cast<std::int32_t>(output.bufferWidth),
            static_cast<std::int32_t>(output.bufferHeight)))
        return mappingFailure<PixelRect>(
            "rect",
            "buffer pixel rectangle lies outside the output");

    const auto mapped = exactPixelRect(transformRect(
        Rect{
            .x = static_cast<double>(rect.x),
            .y = static_cast<double>(rect.y),
            .width = static_cast<double>(rect.width),
            .height = static_cast<double>(rect.height),
        },
        output.transform,
        static_cast<double>(output.bufferWidth),
        static_cast<double>(output.bufferHeight)));
    if (!containedPixelRect(
            mapped,
            oriented.value().width,
            oriented.value().height))
        return mappingFailure<PixelRect>(
            "rect",
            "mapped output-oriented pixel rectangle lies outside the output",
            ErrorCode::InternalError);
    return Result<PixelRect>::success(mapped);
}

} // namespace hfg::v2
