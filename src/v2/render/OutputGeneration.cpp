#include "v2/render/OutputGeneration.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace hfg::v2 {
namespace {

constexpr double MAX_LOGICAL_VALUE = 1'000'000.0;
constexpr double MAX_OUTPUT_SCALE  = 16.0;

bool validName(std::string_view name) {
    return !name.empty() && name.size() <= Limits::MAX_IDENTIFIER_BYTES &&
        std::ranges::none_of(name, [](const unsigned char character) {
            return std::iscntrl(character);
        });
}

bool validLogicalCoordinate(double value) {
    return std::isfinite(value) && std::abs(value) <= MAX_LOGICAL_VALUE;
}

bool validLogicalSize(double value) {
    return std::isfinite(value) && value > 0.0 && value <= MAX_LOGICAL_VALUE;
}

bool validTransform(OutputTransform transform) {
    switch (transform) {
        case OutputTransform::Normal:
        case OutputTransform::Rotate90:
        case OutputTransform::Rotate180:
        case OutputTransform::Rotate270:
        case OutputTransform::Flipped:
        case OutputTransform::Flipped90:
        case OutputTransform::Flipped180:
        case OutputTransform::Flipped270:
            return true;
    }
    return false;
}

} // namespace

Result<void> validateOutputSnapshot(const OutputSnapshot& snapshot) {
    if (!validName(snapshot.name))
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "output.name",
            "expected a non-empty bounded output name",
        });
    if (snapshot.objectToken == 0U)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "output.object_token",
            "output object token must not be zero",
        });
    if (snapshot.modeToken == 0U)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "output.mode_token",
            "output mode token must not be zero",
        });
    if (snapshot.bufferWidth == 0U || snapshot.bufferWidth > Limits::MAX_OUTPUT_BUFFER_DIMENSION)
        return Result<void>::failure({
            ErrorCode::ResourceLimited,
            "output.buffer_width",
            "output buffer width is outside the supported limit",
        });
    if (snapshot.bufferHeight == 0U || snapshot.bufferHeight > Limits::MAX_OUTPUT_BUFFER_DIMENSION)
        return Result<void>::failure({
            ErrorCode::ResourceLimited,
            "output.buffer_height",
            "output buffer height is outside the supported limit",
        });
    if (!validLogicalCoordinate(snapshot.logicalX))
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "output.logical_x",
            "expected a finite logical coordinate",
        });
    if (!validLogicalCoordinate(snapshot.logicalY))
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "output.logical_y",
            "expected a finite logical coordinate",
        });
    if (!validLogicalSize(snapshot.logicalWidth))
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "output.logical_width",
            "expected a finite positive logical size",
        });
    if (!validLogicalSize(snapshot.logicalHeight))
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "output.logical_height",
            "expected a finite positive logical size",
        });
    if (!std::isfinite(snapshot.scale) || snapshot.scale <= 0.0 || snapshot.scale > MAX_OUTPUT_SCALE)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "output.scale",
            "expected a finite positive output scale",
        });
    if (!validTransform(snapshot.transform))
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "output.transform",
            "unsupported output transform",
        });
    if (snapshot.renderFormat == 0U)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "output.render_format",
            "render format must not be zero",
        });
    return Result<void>::success();
}

Result<OutputGenerationUpdate> OutputGenerationTracker::update(OutputSnapshot snapshot) {
    if (auto validation = validateOutputSnapshot(snapshot); !validation)
        return Result<OutputGenerationUpdate>::failure(validation.error());

    const auto currentEntry = m_current.find(snapshot.name);
    if (currentEntry != m_current.end() && currentEntry->second.snapshot == snapshot)
        return Result<OutputGenerationUpdate>::success({
            .current = currentEntry->second,
            .retired = std::nullopt,
            .changed = false,
        });

    auto& lastGeneration = m_lastGeneration[snapshot.name];
    if (lastGeneration == std::numeric_limits<std::uint64_t>::max())
        return Result<OutputGenerationUpdate>::failure({
            .code = ErrorCode::ResourceLimited,
            .path = "output.generation",
            .message = "output generation is exhausted",
        });

    OutputGeneration current{
        .snapshot = std::move(snapshot),
        .generation = ++lastGeneration,
    };
    std::optional<OutputGeneration> retired;
    if (currentEntry != m_current.end())
        retired = currentEntry->second;
    m_current.insert_or_assign(current.snapshot.name, current);
    return Result<OutputGenerationUpdate>::success({
        .current = std::move(current),
        .retired = std::move(retired),
        .changed = true,
    });
}

std::optional<OutputGeneration> OutputGenerationTracker::remove(std::string_view name) {
    const auto entry = m_current.find(name);
    if (entry == m_current.end())
        return std::nullopt;
    auto retired = std::move(entry->second);
    m_current.erase(entry);
    return retired;
}

std::optional<OutputGeneration> OutputGenerationTracker::current(std::string_view name) const {
    const auto entry = m_current.find(name);
    if (entry == m_current.end())
        return std::nullopt;
    return entry->second;
}

std::vector<OutputGeneration> OutputGenerationTracker::currents() const {
    std::vector<OutputGeneration> result;
    result.reserve(m_current.size());
    for (const auto& [name, generation] : m_current) {
        static_cast<void>(name);
        result.push_back(generation);
    }
    return result;
}

void OutputGenerationTracker::clearCurrent() noexcept {
    m_current.clear();
}

std::size_t OutputGenerationTracker::activeCount() const noexcept {
    return m_current.size();
}

} // namespace hfg::v2
