#include "v2/model/Target.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>
#include <type_traits>

namespace hfg::v2 {
namespace {

constexpr double MAX_LOGICAL_VALUE = 1'000'000.0;

Result<Target> invalid(std::string path, std::string message) {
    return Result<Target>::failure({
        .code = ErrorCode::InvalidTarget,
        .path = std::move(path),
        .message = std::move(message),
    });
}

bool validIdentifier(std::string_view value) {
    if (value.empty() || value.size() > Limits::MAX_IDENTIFIER_BYTES || value.starts_with("_hfg_"))
        return false;
    for (const unsigned char character : value) {
        if (!std::isalnum(character) && character != '_' && character != '-' && character != '.')
            return false;
    }
    return true;
}

bool finiteCoordinate(double value) {
    return std::isfinite(value) && std::abs(value) <= MAX_LOGICAL_VALUE;
}

std::optional<Error> validateRect(const Rect& rect, std::string path) {
    if (!finiteCoordinate(rect.x))
        return Error{ErrorCode::InvalidTarget, path + ".x", "expected a finite logical coordinate"};
    if (!finiteCoordinate(rect.y))
        return Error{ErrorCode::InvalidTarget, path + ".y", "expected a finite logical coordinate"};
    if (!std::isfinite(rect.width) || rect.width <= 0.0 || rect.width > MAX_LOGICAL_VALUE)
        return Error{ErrorCode::InvalidTarget, path + ".width", "expected a finite positive logical size"};
    if (!std::isfinite(rect.height) || rect.height <= 0.0 || rect.height > MAX_LOGICAL_VALUE)
        return Error{ErrorCode::InvalidTarget, path + ".height", "expected a finite positive logical size"};
    return std::nullopt;
}

std::optional<Error> validateShape(const Shape& shape) {
    return std::visit([](const auto& value) -> std::optional<Error> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, RoundedRectShape>) {
            if (!std::isfinite(value.radius) || value.radius < 0.0 || value.radius > MAX_LOGICAL_VALUE)
                return Error{ErrorCode::InvalidTarget, "shape.radius", "expected a finite non-negative radius"};
        } else if constexpr (std::is_same_v<T, RingShape>) {
            if (!std::isfinite(value.outerRadius) || value.outerRadius < 0.0 || value.outerRadius > MAX_LOGICAL_VALUE)
                return Error{ErrorCode::InvalidTarget, "shape.outer_radius", "expected a finite non-negative radius"};
            if (!std::isfinite(value.thickness) || value.thickness <= 0.0 || value.thickness > MAX_LOGICAL_VALUE)
                return Error{ErrorCode::InvalidTarget, "shape.thickness", "expected a finite positive thickness"};
        } else if constexpr (std::is_same_v<T, CompoundShape>) {
            if (value.parts.empty())
                return Error{ErrorCode::InvalidTarget, "shape.parts", "compound shape must contain at least one part"};
            if (value.parts.size() > Limits::MAX_COMPOUND_PARTS)
                return Error{ErrorCode::ResourceLimited, "shape.parts", "compound shape exceeds the part limit"};
            for (std::size_t index = 0; index < value.parts.size(); ++index) {
                const auto path = "shape.parts[" + std::to_string(index) + "]";
                if (auto error = validateRect(value.parts[index].rect, path + ".rect"))
                    return error;
                const double radius = value.parts[index].radius;
                if (!std::isfinite(radius) || radius < 0.0 || radius > MAX_LOGICAL_VALUE)
                    return Error{ErrorCode::InvalidTarget, path + ".radius", "expected a finite non-negative radius"};
            }
        }
        return std::nullopt;
    }, shape);
}

bool validOpaqueName(std::string_view value) {
    return !value.empty() && value.size() <= Limits::MAX_IDENTIFIER_BYTES &&
        std::ranges::none_of(value, [](const unsigned char character) {
            return std::iscntrl(character);
        });
}

std::optional<std::string> normalizeAddress(std::string address) {
    if (address.starts_with("0x") || address.starts_with("0X"))
        address.erase(0, 2);
    if (address.empty() || address.size() > 2U * sizeof(std::uintptr_t))
        return std::nullopt;
    if (!std::ranges::all_of(address, [](const unsigned char character) {
            return std::isxdigit(character);
        }))
        return std::nullopt;
    std::ranges::transform(address, address.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return "0x" + address;
}

} // namespace

Result<Target> validateTarget(TargetInput input) {
    if (!validIdentifier(input.id))
        return invalid("id", "expected 1-128 ASCII letters, digits, '.', '_' or '-' without the reserved _hfg_ prefix");
    if (!validIdentifier(input.material.name))
        return invalid("material.name", "invalid material name");
    if (auto error = validateShape(input.shape))
        return Result<Target>::failure(std::move(*error));

    switch (input.kind) {
        case TargetKind::Window: {
            auto* selector = std::get_if<WindowSelector>(&input.selector);
            if (!selector)
                return invalid("selector", "window target requires a window selector");
            const auto address = normalizeAddress(selector->address);
            if (!address)
                return invalid("selector.address", "expected a non-empty hexadecimal window address");
            selector->address = *address;
            if (!selector->pid && !selector->initialClass)
                return invalid("selector", "window selector requires pid or initial_class identity evidence");
            if (selector->pid && *selector->pid <= 0)
                return invalid("selector.pid", "pid must be greater than zero");
            if (selector->initialClass && !validOpaqueName(*selector->initialClass))
                return invalid("selector.initial_class", "initial class must be a non-empty bounded string");
            if (input.geometry)
                return invalid("geometry", "window targets use compositor-owned window geometry");
            if (input.stage)
                return invalid("stage", "window targets use the window decoration stage");
            break;
        }
        case TargetKind::Layer: {
            const auto* selector = std::get_if<LayerSelector>(&input.selector);
            if (!selector)
                return invalid("selector", "layer target requires a layer selector");
            if (!validOpaqueName(selector->namespaceName))
                return invalid("selector.namespace", "namespace must be a non-empty bounded string");
            if (input.geometry)
                if (auto error = validateRect(*input.geometry, "geometry"))
                    return Result<Target>::failure(std::move(*error));
            if (input.stage)
                return invalid("stage", "layer targets derive their render stage from the attached layer surface");
            break;
        }
        case TargetKind::Region: {
            const auto* selector = std::get_if<RegionSelector>(&input.selector);
            if (!selector)
                return invalid("selector", "region target requires a region selector");
            if (!validOpaqueName(selector->output))
                return invalid("selector.output", "output must be a non-empty bounded string");
            if (!input.geometry)
                return invalid("geometry", "region target requires output-local geometry");
            if (auto error = validateRect(*input.geometry, "geometry"))
                return Result<Target>::failure(std::move(*error));
            if (!input.stage)
                return invalid("stage", "region target requires an explicit render stage");
            break;
        }
    }

    return Result<Target>::success({
        .id = std::move(input.id),
        .kind = input.kind,
        .material = std::move(input.material),
        .shape = std::move(input.shape),
        .selector = std::move(input.selector),
        .geometry = std::move(input.geometry),
        .stage = input.stage,
        .enabled = input.enabled,
    });
}

} // namespace hfg::v2
