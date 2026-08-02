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
constexpr double MAX_EASING_VALUE  = 16.0;
constexpr double ENDPOINT_EPSILON  = 1e-9;

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

std::optional<Error> validateTransition(const Transition& transition, std::string path) {
    if (!validIdentifier(transition.id))
        return Error{
            ErrorCode::InvalidTarget,
            path + ".id",
            "expected a bounded transition identifier",
        };
    switch (transition.phase) {
        case TransitionPhase::Enter:
        case TransitionPhase::Exit:
            break;
        default:
            return Error{
                ErrorCode::InvalidTarget,
                path + ".phase",
                "expected enter or exit",
            };
    }
    switch (transition.edge) {
        case TransitionEdge::Top:
        case TransitionEdge::Bottom:
        case TransitionEdge::Left:
        case TransitionEdge::Right:
            break;
        default:
            return Error{
                ErrorCode::InvalidTarget,
                path + ".edge",
                "expected top, bottom, left or right",
            };
    }
    if (transition.durationMs == 0U || transition.durationMs > Limits::MAX_TRANSITION_MS)
        return Error{
            ErrorCode::InvalidTarget,
            path + ".duration_ms",
            "expected a duration within the supported millisecond limit",
        };
    if (transition.elapsedMs > transition.durationMs)
        return Error{
            ErrorCode::InvalidTarget,
            path + ".elapsed_ms",
            "elapsed time must not exceed duration",
        };
    if (!finiteCoordinate(transition.travel) || transition.travel < 0.0)
        return Error{
            ErrorCode::InvalidTarget,
            path + ".travel",
            "expected a finite non-negative logical distance",
        };
    if (transition.easing.size() > Limits::MAX_BEZIER_SEGMENTS)
        return Error{
            ErrorCode::ResourceLimited,
            path + ".easing",
            "transition exceeds the Bezier segment limit",
        };

    double previousEndX = 0.0;
    for (std::size_t index = 0; index < transition.easing.size(); ++index) {
        const auto& segment = transition.easing[index];
        const auto segmentPath = path + ".easing[" + std::to_string(index) + "]";
        for (const auto& [name, value] : {
                 std::pair<std::string_view, double>{"control1_x", segment.control1X},
                 {"control1_y", segment.control1Y},
                 {"control2_x", segment.control2X},
                 {"control2_y", segment.control2Y},
                 {"end_x", segment.endX},
                 {"end_y", segment.endY},
             }) {
            if (!std::isfinite(value) || std::abs(value) > MAX_EASING_VALUE)
                return Error{
                    ErrorCode::InvalidTarget,
                    segmentPath + "." + std::string(name),
                    "expected a finite bounded easing coordinate",
                };
        }
        if (segment.endX <= previousEndX || segment.endX > 1.0)
            return Error{
                ErrorCode::InvalidTarget,
                segmentPath + ".end_x",
                "Bezier segment endpoints must advance through x in (0, 1]",
            };
        if (segment.control1X < previousEndX || segment.control1X > segment.endX ||
            segment.control2X < previousEndX || segment.control2X > segment.endX)
            return Error{
                ErrorCode::InvalidTarget,
                segmentPath,
                "Bezier control-point x values must remain inside their segment",
            };
        previousEndX = segment.endX;
    }
    if (!transition.easing.empty()) {
        const auto& endpoint = transition.easing.back();
        if (std::abs(endpoint.endX - 1.0) > ENDPOINT_EPSILON ||
            std::abs(endpoint.endY - 1.0) > ENDPOINT_EPSILON)
            return Error{
                ErrorCode::InvalidTarget,
                path + ".easing",
                "Bezier easing must end at (1, 1)",
            };
    }
    return std::nullopt;
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

std::optional<Error> validateCornerRadii(const CornerRadii& corners, std::string path) {
    for (const auto& [name, value] : {
             std::pair<std::string_view, double>{"top_left", corners.topLeft},
             {"top_right", corners.topRight},
             {"bottom_right", corners.bottomRight},
             {"bottom_left", corners.bottomLeft},
         }) {
        if (!std::isfinite(value) || value < 0.0 || value > MAX_LOGICAL_VALUE)
            return Error{
                ErrorCode::InvalidTarget,
                path + "." + std::string(name),
                "expected a finite non-negative radius",
            };
    }
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
            if (!value.base && value.parts.empty())
                return Error{ErrorCode::InvalidTarget, "shape", "compound shape requires a base or at least one part"};
            if (value.cutout && !value.base)
                return Error{ErrorCode::InvalidTarget, "shape.cutout", "compound cutout requires a base"};
            if (value.parts.size() > Limits::MAX_COMPOUND_PARTS)
                return Error{ErrorCode::ResourceLimited, "shape.parts", "compound shape exceeds the part limit"};
            if (value.connectors.size() > Limits::MAX_COMPOUND_CONNECTORS)
                return Error{ErrorCode::ResourceLimited, "shape.connectors", "compound shape exceeds the connector limit"};
            if (!std::isfinite(value.connectorCurve) || value.connectorCurve < 0.0 ||
                value.connectorCurve > MAX_LOGICAL_VALUE)
                return Error{
                    ErrorCode::InvalidTarget,
                    "shape.connector_curve",
                    "expected a finite non-negative connector curve",
                };
            if (value.base)
                if (auto error = validateCornerRadii(value.base->corners, "shape.base.corner_radii"))
                    return error;
            if (value.cutout) {
                if (auto error = validateRect(value.cutout->rect, "shape.cutout.rect"))
                    return error;
                if (auto error = validateCornerRadii(value.cutout->corners, "shape.cutout.corner_radii"))
                    return error;
            }
            for (std::size_t index = 0; index < value.parts.size(); ++index) {
                const auto path = "shape.parts[" + std::to_string(index) + "]";
                if (auto error = validateRect(value.parts[index].rect, path + ".rect"))
                    return error;
                if (auto error = validateCornerRadii(value.parts[index].corners, path + ".corner_radii"))
                    return error;
                if (auto error = validateCornerRadii(value.parts[index].junctions, path + ".junctions"))
                    return error;
                if (value.parts[index].materialExtent)
                    if (auto error = validateRect(*value.parts[index].materialExtent, path + ".material_extent"))
                        return error;
                if (value.parts[index].transition) {
                    if (auto error = validateTransition(
                            value.parts[index].transition->motion,
                            path + ".transition"))
                        return error;
                    const double protrusion = value.parts[index].transition->protrusion;
                    if (!finiteCoordinate(protrusion) || protrusion < 0.0)
                        return Error{
                            ErrorCode::InvalidTarget,
                            path + ".transition.protrusion",
                            "expected a finite non-negative logical distance",
                        };
                }
                const double opacity = value.parts[index].opacity;
                if (!std::isfinite(opacity) || opacity < 0.0 || opacity > 1.0)
                    return Error{ErrorCode::InvalidTarget, path + ".opacity", "expected a finite value from 0 to 1"};
            }
            for (std::size_t index = 0; index < value.connectors.size(); ++index)
                if (auto error = validateRect(
                        value.connectors[index],
                        "shape.connectors[" + std::to_string(index) + "]"))
                    return error;
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
    if (input.transition)
        if (auto error = validateTransition(*input.transition, "transition"))
            return Result<Target>::failure(std::move(*error));
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
            // pre-window capture exists only for exact window targets; a
            // region target accepted here would fail at capture time every
            // frame with nothing reported against the target.
            if (*input.stage == RenderStage::PreWindow)
                return invalid(
                    "stage", "region targets cannot use the pre-window stage");
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
        .transition = std::move(input.transition),
        .enabled = input.enabled,
    });
}

} // namespace hfg::v2
