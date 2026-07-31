#include "v2/ipc/Protocol.hpp"

#include "v2/core/Limits.hpp"
#include "v2/model/Material.hpp"
#include "v2/model/Target.hpp"

#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace hfg::v2 {
namespace {

using json = nlohmann::json;

class ParseConstraintError final : public std::runtime_error {
  public:
    explicit ParseConstraintError(Error error)
        : std::runtime_error(error.message), m_error(std::move(error)) {}

    [[nodiscard]] const Error& error() const noexcept {
        return m_error;
    }

  private:
    Error m_error;
};

class StrictParseState {
  public:
    bool operator()(int depth, json::parse_event_t event, json& parsed) {
        if (depth < 0 || static_cast<std::size_t>(depth) > Limits::MAX_JSON_NESTING) {
            throw ParseConstraintError({
                .code = ErrorCode::ResourceLimited,
                .path = "",
                .message = "JSON nesting limit exceeded",
            });
        }

        const auto objectIndex = static_cast<std::size_t>(depth) + 1U;
        const auto index = event == json::parse_event_t::key
            ? static_cast<std::size_t>(depth)
            : objectIndex;
        if (m_objectKeys.size() <= index)
            m_objectKeys.resize(index + 1U);

        if (event == json::parse_event_t::object_start) {
            m_objectKeys[index].clear();
        } else if (event == json::parse_event_t::key) {
            const auto& key = parsed.get_ref<const std::string&>();
            if (!m_objectKeys[index].insert(key).second) {
                throw ParseConstraintError({
                    .code = ErrorCode::InvalidRequest,
                    .path = key,
                    .message = "duplicate field",
                });
            }
        } else if (event == json::parse_event_t::object_end) {
            m_objectKeys[index].clear();
        }

        return true;
    }

  private:
    std::vector<std::set<std::string>> m_objectKeys;
};

template <typename T>
Result<T> invalid(ErrorCode code, std::string path, std::string message) {
    return Result<T>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

std::optional<Error> rejectUnknown(
    const json& object,
    const std::set<std::string_view>& allowed,
    std::string_view path,
    ErrorCode code = ErrorCode::InvalidRequest) {
    for (const auto& [key, value] : object.items()) {
        static_cast<void>(value);
        if (!allowed.contains(key))
            return Error{
                .code = code,
                .path = path.empty() ? key : std::string(path) + "." + key,
                .message = "unknown field",
            };
    }
    return std::nullopt;
}

Result<std::string> requiredString(
    const json& object,
    std::string_view key,
    std::string path,
    ErrorCode code = ErrorCode::InvalidRequest) {
    const auto value = object.find(key);
    if (value == object.end() || !value->is_string())
        return invalid<std::string>(code, std::move(path), "expected a string");
    const auto result = value->get<std::string>();
    if (result.empty() || result.size() > Limits::MAX_IDENTIFIER_BYTES)
        return invalid<std::string>(code, std::move(path), "expected a non-empty string no longer than 128 bytes");
    return Result<std::string>::success(result);
}

Result<std::uint64_t> requiredUnsigned(
    const json& object,
    std::string_view key,
    std::string path,
    ErrorCode code = ErrorCode::InvalidRequest) {
    const auto value = object.find(key);
    if (value == object.end())
        return invalid<std::uint64_t>(code, std::move(path), "expected an integer");
    if (value->is_number_unsigned())
        return Result<std::uint64_t>::success(value->get<std::uint64_t>());
    if (value->is_number_integer()) {
        const auto signedValue = value->get<std::int64_t>();
        if (signedValue >= 0)
            return Result<std::uint64_t>::success(static_cast<std::uint64_t>(signedValue));
    }
    return invalid<std::uint64_t>(code, std::move(path), "expected a non-negative integer");
}

std::optional<Error> assignNumber(
    const json& object,
    std::string_view key,
    double& destination,
    std::string path,
    ErrorCode code) {
    const auto value = object.find(key);
    if (value == object.end())
        return std::nullopt;
    if (!value->is_number())
        return Error{code, std::move(path), "expected a number"};
    const double number = value->get<double>();
    if (!std::isfinite(number))
        return Error{code, std::move(path), "expected a finite number"};
    destination = number;
    return std::nullopt;
}

std::optional<Error> assignOptionalNumber(
    const json& object,
    std::string_view key,
    std::optional<double>& destination,
    std::string path,
    ErrorCode code) {
    const auto value = object.find(key);
    if (value == object.end())
        return std::nullopt;
    if (!value->is_number())
        return Error{code, std::move(path), "expected a number"};
    const double number = value->get<double>();
    if (!std::isfinite(number))
        return Error{code, std::move(path), "expected a finite number"};
    destination = number;
    return std::nullopt;
}

std::optional<Error> assignBoolean(
    const json& object,
    std::string_view key,
    bool& destination,
    std::string path,
    ErrorCode code) {
    const auto value = object.find(key);
    if (value == object.end())
        return std::nullopt;
    if (!value->is_boolean())
        return Error{code, std::move(path), "expected a boolean"};
    destination = value->get<bool>();
    return std::nullopt;
}

std::optional<Error> assignString(
    const json& object,
    std::string_view key,
    std::string& destination,
    std::string path,
    ErrorCode code) {
    const auto value = object.find(key);
    if (value == object.end())
        return std::nullopt;
    if (!value->is_string())
        return Error{code, std::move(path), "expected a string"};
    destination = value->get<std::string>();
    return std::nullopt;
}

Result<Material> parseMaterial(std::string name, const json& object, std::string path) {
    if (!object.is_object())
        return invalid<Material>(ErrorCode::InvalidMaterial, std::move(path), "material must be an object");
    static const std::set<std::string_view> fields{
        "glass_level", "blur_level", "tint_level", "tint_enabled", "tint_color", "light_mode",
        "refraction", "rim_band", "bevel", "rim_width", "highlight", "shadow", "light_angle",
        "specular", "chroma", "edge_depth", "lens", "lens_band", "gloss",
    };
    if (auto error = rejectUnknown(object, fields, path, ErrorCode::InvalidMaterial))
        return Result<Material>::failure(std::move(*error));

    MaterialInput input;
    if (auto error = assignNumber(object, "glass_level", input.glassLevel, path + ".glass_level", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignOptionalNumber(object, "blur_level", input.blurLevel, path + ".blur_level", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignOptionalNumber(object, "tint_level", input.tintLevel, path + ".tint_level", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignBoolean(object, "tint_enabled", input.tintEnabled, path + ".tint_enabled", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignString(object, "tint_color", input.tintColor, path + ".tint_color", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignBoolean(object, "light_mode", input.lightMode, path + ".light_mode", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignNumber(object, "refraction", input.refraction, path + ".refraction", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignNumber(object, "rim_band", input.rimBand, path + ".rim_band", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignNumber(object, "bevel", input.bevel, path + ".bevel", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignNumber(object, "rim_width", input.rimWidth, path + ".rim_width", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignNumber(object, "highlight", input.highlight, path + ".highlight", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignNumber(object, "shadow", input.shadow, path + ".shadow", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignNumber(object, "light_angle", input.lightAngle, path + ".light_angle", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignNumber(object, "specular", input.specular, path + ".specular", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignNumber(object, "chroma", input.chroma, path + ".chroma", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignNumber(object, "edge_depth", input.edgeDepth, path + ".edge_depth", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignNumber(object, "lens", input.lens, path + ".lens", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignNumber(object, "lens_band", input.lensBand, path + ".lens_band", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));
    if (auto error = assignNumber(object, "gloss", input.gloss, path + ".gloss", ErrorCode::InvalidMaterial)) return Result<Material>::failure(std::move(*error));

    auto material = validateMaterial(std::move(name), input);
    if (!material) {
        auto error = material.error();
        error.path = path + (error.path.empty() ? "" : "." + error.path);
        return Result<Material>::failure(std::move(error));
    }
    return material;
}

Result<Rect> parseRect(const json& object, std::string path, std::string_view expectedSpace) {
    if (!object.is_object())
        return invalid<Rect>(ErrorCode::InvalidTarget, std::move(path), "geometry must be an object");
    static const std::set<std::string_view> fields{"space", "x", "y", "width", "height"};
    if (auto error = rejectUnknown(object, fields, path, ErrorCode::InvalidTarget))
        return Result<Rect>::failure(std::move(*error));
    const auto space = requiredString(object, "space", path + ".space", ErrorCode::InvalidTarget);
    if (!space)
        return Result<Rect>::failure(space.error());
    if (space.value() != expectedSpace)
        return invalid<Rect>(ErrorCode::InvalidTarget, path + ".space", "unexpected geometry space");

    Rect rect;
    for (const auto& [key, destination] : {
             std::pair<std::string_view, double*>{"x", &rect.x},
             {"y", &rect.y},
             {"width", &rect.width},
             {"height", &rect.height},
         }) {
        const auto value = object.find(key);
        if (value == object.end() || !value->is_number())
            return invalid<Rect>(ErrorCode::InvalidTarget, path + "." + std::string(key), "expected a number");
        *destination = value->get<double>();
        if (!std::isfinite(*destination))
            return invalid<Rect>(ErrorCode::InvalidTarget, path + "." + std::string(key), "expected a finite number");
    }
    return Result<Rect>::success(rect);
}

Result<Rect> parseLocalRect(const json& object, std::string path) {
    if (!object.is_object())
        return invalid<Rect>(ErrorCode::InvalidTarget, std::move(path), "rectangle must be an object");
    static const std::set<std::string_view> fields{"x", "y", "width", "height"};
    if (auto error = rejectUnknown(object, fields, path, ErrorCode::InvalidTarget))
        return Result<Rect>::failure(std::move(*error));

    Rect rect;
    for (const auto& [key, destination] : {
             std::pair<std::string_view, double*>{"x", &rect.x},
             {"y", &rect.y},
             {"width", &rect.width},
             {"height", &rect.height},
         }) {
        const auto value = object.find(key);
        if (value == object.end() || !value->is_number())
            return invalid<Rect>(ErrorCode::InvalidTarget, path + "." + std::string(key), "expected a number");
        *destination = value->get<double>();
        if (!std::isfinite(*destination))
            return invalid<Rect>(ErrorCode::InvalidTarget, path + "." + std::string(key), "expected a finite number");
    }
    return Result<Rect>::success(rect);
}

Result<PresentationHandoffRequest::MorphEndpoint> parseMorphEndpoint(
    const json& object,
    std::string path) {
    if (!object.is_object())
        return invalid<PresentationHandoffRequest::MorphEndpoint>(
            ErrorCode::InvalidRequest,
            std::move(path),
            "morph endpoint must be an object");
    if (auto error = rejectUnknown(
            object, {"rect", "radius"}, path))
        return Result<PresentationHandoffRequest::MorphEndpoint>::failure(
            std::move(*error));
    const auto rectValue = object.find("rect");
    if (rectValue == object.end())
        return invalid<PresentationHandoffRequest::MorphEndpoint>(
            ErrorCode::InvalidRequest,
            path + ".rect",
            "morph endpoint requires a rectangle");
    auto rect = parseLocalRect(*rectValue, path + ".rect");
    if (!rect)
        return Result<PresentationHandoffRequest::MorphEndpoint>::failure(
            rect.error());
    const auto radiusValue = object.find("radius");
    if (radiusValue == object.end() || !radiusValue->is_number())
        return invalid<PresentationHandoffRequest::MorphEndpoint>(
            ErrorCode::InvalidRequest,
            path + ".radius",
            "morph endpoint radius must be a number");
    const auto radius = radiusValue->get<double>();
    if (!std::isfinite(radius))
        return invalid<PresentationHandoffRequest::MorphEndpoint>(
            ErrorCode::InvalidRequest,
            path + ".radius",
            "morph endpoint radius must be finite");
    return Result<PresentationHandoffRequest::MorphEndpoint>::success({
        .rect = rect.value(),
        .radius = radius,
    });
}

Result<CornerRadii> parseCornerRadii(const json& object, std::string path) {
    if (!object.is_object())
        return invalid<CornerRadii>(ErrorCode::InvalidTarget, std::move(path), "corner radii must be an object");
    static const std::set<std::string_view> fields{
        "top_left", "top_right", "bottom_right", "bottom_left",
    };
    if (auto error = rejectUnknown(object, fields, path, ErrorCode::InvalidTarget))
        return Result<CornerRadii>::failure(std::move(*error));

    CornerRadii corners;
    for (const auto& [key, destination] : {
             std::pair<std::string_view, double*>{"top_left", &corners.topLeft},
             {"top_right", &corners.topRight},
             {"bottom_right", &corners.bottomRight},
             {"bottom_left", &corners.bottomLeft},
         }) {
        const auto value = object.find(key);
        if (value == object.end() || !value->is_number())
            return invalid<CornerRadii>(
                ErrorCode::InvalidTarget,
                path + "." + std::string(key),
                "expected a number");
        *destination = value->get<double>();
        if (!std::isfinite(*destination))
            return invalid<CornerRadii>(
                ErrorCode::InvalidTarget,
                path + "." + std::string(key),
                "expected a finite number");
    }
    return Result<CornerRadii>::success(corners);
}

Result<CornerRadii> parseCornerFields(
    const json& object,
    std::string path,
    std::string_view radiusField = "radius",
    std::string_view cornersField = "corner_radii") {
    const auto radius = object.find(radiusField);
    const auto corners = object.find(cornersField);
    if (radius != object.end() && corners != object.end())
        return invalid<CornerRadii>(
            ErrorCode::InvalidTarget,
            path,
            std::string(radiusField) + " and " + std::string(cornersField) + " are mutually exclusive");
    if (corners != object.end())
        return parseCornerRadii(*corners, path + "." + std::string(cornersField));
    if (radius == object.end())
        return Result<CornerRadii>::success({});
    if (!radius->is_number())
        return invalid<CornerRadii>(
            ErrorCode::InvalidTarget,
            path + "." + std::string(radiusField),
            "expected a number");
    const double value = radius->get<double>();
    if (!std::isfinite(value))
        return invalid<CornerRadii>(
            ErrorCode::InvalidTarget,
            path + "." + std::string(radiusField),
            "expected a finite number");
    return Result<CornerRadii>::success({value, value, value, value});
}

Result<Transition> parseTransition(const json& object, std::string path, bool partTransition) {
    if (!object.is_object())
        return invalid<Transition>(ErrorCode::InvalidTarget, std::move(path), "transition must be an object");
    std::set<std::string_view> fields{
        "id", "phase", "edge", "duration_ms", "elapsed_ms", "travel", "easing",
    };
    if (partTransition)
        fields.insert("protrusion");
    if (auto error = rejectUnknown(object, fields, path, ErrorCode::InvalidTarget))
        return Result<Transition>::failure(std::move(*error));

    const auto id = requiredString(object, "id", path + ".id", ErrorCode::InvalidTarget);
    const auto phase = requiredString(object, "phase", path + ".phase", ErrorCode::InvalidTarget);
    const auto edge = requiredString(object, "edge", path + ".edge", ErrorCode::InvalidTarget);
    const auto duration = requiredUnsigned(
        object,
        "duration_ms",
        path + ".duration_ms",
        ErrorCode::InvalidTarget);
    if (!id) return Result<Transition>::failure(id.error());
    if (!phase) return Result<Transition>::failure(phase.error());
    if (!edge) return Result<Transition>::failure(edge.error());
    if (!duration) return Result<Transition>::failure(duration.error());

    Transition parsed;
    parsed.id = id.value();
    parsed.durationMs = duration.value();
    if (phase.value() == "enter")
        parsed.phase = TransitionPhase::Enter;
    else if (phase.value() == "exit")
        parsed.phase = TransitionPhase::Exit;
    else
        return invalid<Transition>(ErrorCode::InvalidTarget, path + ".phase", "phase must be enter or exit");

    if (edge.value() == "top")
        parsed.edge = TransitionEdge::Top;
    else if (edge.value() == "bottom")
        parsed.edge = TransitionEdge::Bottom;
    else if (edge.value() == "left")
        parsed.edge = TransitionEdge::Left;
    else if (edge.value() == "right")
        parsed.edge = TransitionEdge::Right;
    else
        return invalid<Transition>(
            ErrorCode::InvalidTarget,
            path + ".edge",
            "edge must be top, bottom, left or right");

    if (object.contains("elapsed_ms")) {
        auto elapsed = requiredUnsigned(
            object,
            "elapsed_ms",
            path + ".elapsed_ms",
            ErrorCode::InvalidTarget);
        if (!elapsed)
            return Result<Transition>::failure(elapsed.error());
        parsed.elapsedMs = elapsed.value();
    }
    if (auto error = assignNumber(
            object,
            "travel",
            parsed.travel,
            path + ".travel",
            ErrorCode::InvalidTarget))
        return Result<Transition>::failure(std::move(*error));

    if (const auto easing = object.find("easing"); easing != object.end()) {
        if (!easing->is_array())
            return invalid<Transition>(ErrorCode::InvalidTarget, path + ".easing", "easing must be an array");
        if (easing->size() > Limits::MAX_BEZIER_SEGMENTS)
            return invalid<Transition>(
                ErrorCode::ResourceLimited,
                path + ".easing",
                "Bezier segment limit exceeded");
        static const std::set<std::string_view> segmentFields{
            "control1_x", "control1_y", "control2_x", "control2_y", "end_x", "end_y",
        };
        for (std::size_t index = 0; index < easing->size(); ++index) {
            const auto& segment = (*easing)[index];
            const auto segmentPath = path + ".easing[" + std::to_string(index) + "]";
            if (!segment.is_object())
                return invalid<Transition>(
                    ErrorCode::InvalidTarget,
                    segmentPath,
                    "Bezier segment must be an object");
            if (auto error = rejectUnknown(segment, segmentFields, segmentPath, ErrorCode::InvalidTarget))
                return Result<Transition>::failure(std::move(*error));
            CubicBezierSegment parsedSegment;
            for (const auto& [key, destination] : {
                     std::pair<std::string_view, double*>{"control1_x", &parsedSegment.control1X},
                     {"control1_y", &parsedSegment.control1Y},
                     {"control2_x", &parsedSegment.control2X},
                     {"control2_y", &parsedSegment.control2Y},
                     {"end_x", &parsedSegment.endX},
                     {"end_y", &parsedSegment.endY},
                 }) {
                const auto value = segment.find(key);
                if (value == segment.end() || !value->is_number())
                    return invalid<Transition>(
                        ErrorCode::InvalidTarget,
                        segmentPath + "." + std::string(key),
                        "expected a number");
                *destination = value->get<double>();
                if (!std::isfinite(*destination))
                    return invalid<Transition>(
                        ErrorCode::InvalidTarget,
                        segmentPath + "." + std::string(key),
                        "expected a finite number");
            }
            parsed.easing.push_back(parsedSegment);
        }
    }
    return Result<Transition>::success(std::move(parsed));
}

Result<Shape> parseShape(const json& object, std::string path) {
    if (!object.is_object())
        return invalid<Shape>(ErrorCode::InvalidTarget, std::move(path), "shape must be an object");
    const auto kind = requiredString(object, "kind", path + ".kind", ErrorCode::InvalidTarget);
    if (!kind)
        return Result<Shape>::failure(kind.error());

    if (kind.value() == "rounded-rect") {
        static const std::set<std::string_view> fields{"kind", "radius"};
        if (auto error = rejectUnknown(object, fields, path, ErrorCode::InvalidTarget))
            return Result<Shape>::failure(std::move(*error));
        RoundedRectShape shape;
        if (auto error = assignNumber(object, "radius", shape.radius, path + ".radius", ErrorCode::InvalidTarget))
            return Result<Shape>::failure(std::move(*error));
        return Result<Shape>::success(shape);
    }
    if (kind.value() == "ring") {
        static const std::set<std::string_view> fields{"kind", "outer_radius", "thickness"};
        if (auto error = rejectUnknown(object, fields, path, ErrorCode::InvalidTarget))
            return Result<Shape>::failure(std::move(*error));
        RingShape shape;
        if (auto error = assignNumber(object, "outer_radius", shape.outerRadius, path + ".outer_radius", ErrorCode::InvalidTarget))
            return Result<Shape>::failure(std::move(*error));
        if (auto error = assignNumber(object, "thickness", shape.thickness, path + ".thickness", ErrorCode::InvalidTarget))
            return Result<Shape>::failure(std::move(*error));
        return Result<Shape>::success(shape);
    }
    if (kind.value() == "compound") {
        static const std::set<std::string_view> fields{
            "kind", "base", "cutout", "parts", "connectors", "connector_curve",
        };
        if (auto error = rejectUnknown(object, fields, path, ErrorCode::InvalidTarget))
            return Result<Shape>::failure(std::move(*error));

        CompoundShape shape;
        if (const auto base = object.find("base"); base != object.end()) {
            const auto basePath = path + ".base";
            if (!base->is_object())
                return invalid<Shape>(ErrorCode::InvalidTarget, basePath, "base must be an object");
            static const std::set<std::string_view> baseFields{"radius", "corner_radii"};
            if (auto error = rejectUnknown(*base, baseFields, basePath, ErrorCode::InvalidTarget))
                return Result<Shape>::failure(std::move(*error));
            auto corners = parseCornerFields(*base, basePath);
            if (!corners)
                return Result<Shape>::failure(corners.error());
            shape.base = CompoundBase{.corners = std::move(corners.value())};
        }

        if (const auto cutout = object.find("cutout"); cutout != object.end()) {
            const auto cutoutPath = path + ".cutout";
            if (!cutout->is_object())
                return invalid<Shape>(ErrorCode::InvalidTarget, cutoutPath, "cutout must be an object");
            static const std::set<std::string_view> cutoutFields{
                "x", "y", "width", "height", "radius", "corner_radii",
            };
            if (auto error = rejectUnknown(*cutout, cutoutFields, cutoutPath, ErrorCode::InvalidTarget))
                return Result<Shape>::failure(std::move(*error));
            json rectObject = {
                {"x", cutout->value("x", json())},
                {"y", cutout->value("y", json())},
                {"width", cutout->value("width", json())},
                {"height", cutout->value("height", json())},
            };
            auto rect = parseLocalRect(rectObject, cutoutPath);
            if (!rect)
                return Result<Shape>::failure(rect.error());
            auto corners = parseCornerFields(*cutout, cutoutPath);
            if (!corners)
                return Result<Shape>::failure(corners.error());
            shape.cutout = CompoundCutout{
                .rect = std::move(rect.value()),
                .corners = std::move(corners.value()),
            };
        }

        const auto parts = object.find("parts");
        if (parts != object.end() && !parts->is_array())
            return invalid<Shape>(ErrorCode::InvalidTarget, path + ".parts", "parts must be an array");
        const std::size_t partCount = parts == object.end() ? 0U : parts->size();
        if (partCount > Limits::MAX_COMPOUND_PARTS)
            return invalid<Shape>(ErrorCode::ResourceLimited, path + ".parts", "compound part limit exceeded");

        for (std::size_t index = 0; index < partCount; ++index) {
            const auto& part = (*parts)[index];
            const auto partPath = path + ".parts[" + std::to_string(index) + "]";
            if (!part.is_object())
                return invalid<Shape>(ErrorCode::InvalidTarget, partPath, "compound part must be an object");
            static const std::set<std::string_view> partFields{
                "x", "y", "width", "height", "radius", "corner_radii",
                "junctions", "material_extent", "transition", "opacity",
            };
            if (auto error = rejectUnknown(part, partFields, partPath, ErrorCode::InvalidTarget))
                return Result<Shape>::failure(std::move(*error));

            json rectObject = {
                {"x", part.value("x", json())},
                {"y", part.value("y", json())},
                {"width", part.value("width", json())},
                {"height", part.value("height", json())},
            };
            auto rect = parseLocalRect(rectObject, partPath);
            if (!rect)
                return Result<Shape>::failure(rect.error());
            auto corners = parseCornerFields(part, partPath);
            if (!corners)
                return Result<Shape>::failure(corners.error());

            CompoundPart parsed;
            parsed.rect = std::move(rect.value());
            parsed.corners = std::move(corners.value());
            if (const auto junctions = part.find("junctions"); junctions != part.end()) {
                auto parsedJunctions = parseCornerRadii(*junctions, partPath + ".junctions");
                if (!parsedJunctions)
                    return Result<Shape>::failure(parsedJunctions.error());
                parsed.junctions = std::move(parsedJunctions.value());
            }
            if (const auto extent = part.find("material_extent"); extent != part.end()) {
                auto parsedExtent = parseLocalRect(*extent, partPath + ".material_extent");
                if (!parsedExtent)
                    return Result<Shape>::failure(parsedExtent.error());
                parsed.materialExtent = std::move(parsedExtent.value());
            }
            if (const auto transition = part.find("transition"); transition != part.end()) {
                auto motion = parseTransition(*transition, partPath + ".transition", true);
                if (!motion)
                    return Result<Shape>::failure(motion.error());
                double protrusion = motion.value().travel;
                if (auto error = assignNumber(
                        *transition,
                        "protrusion",
                        protrusion,
                        partPath + ".transition.protrusion",
                        ErrorCode::InvalidTarget))
                    return Result<Shape>::failure(std::move(*error));
                parsed.transition = PartTransition{
                    .motion = std::move(motion.value()),
                    .protrusion = protrusion,
                };
            }
            if (auto error = assignNumber(
                    part,
                    "opacity",
                    parsed.opacity,
                    partPath + ".opacity",
                    ErrorCode::InvalidTarget))
                return Result<Shape>::failure(std::move(*error));
            shape.parts.push_back(parsed);
        }

        if (const auto connectors = object.find("connectors"); connectors != object.end()) {
            if (!connectors->is_array())
                return invalid<Shape>(ErrorCode::InvalidTarget, path + ".connectors", "connectors must be an array");
            if (connectors->size() > Limits::MAX_COMPOUND_CONNECTORS)
                return invalid<Shape>(
                    ErrorCode::ResourceLimited,
                    path + ".connectors",
                    "compound connector limit exceeded");
            for (std::size_t index = 0; index < connectors->size(); ++index) {
                auto connector = parseLocalRect(
                    (*connectors)[index],
                    path + ".connectors[" + std::to_string(index) + "]");
                if (!connector)
                    return Result<Shape>::failure(connector.error());
                shape.connectors.push_back(std::move(connector.value()));
            }
        }
        if (auto error = assignNumber(
                object,
                "connector_curve",
                shape.connectorCurve,
                path + ".connector_curve",
                ErrorCode::InvalidTarget))
            return Result<Shape>::failure(std::move(*error));
        return Result<Shape>::success(std::move(shape));
    }
    return invalid<Shape>(ErrorCode::InvalidTarget, path + ".kind", "unsupported shape kind");
}

Result<MaterialReference> parseMaterialReference(const json& object, std::string path) {
    if (!object.is_object())
        return invalid<MaterialReference>(ErrorCode::InvalidTarget, std::move(path), "material reference must be an object");
    static const std::set<std::string_view> fields{"source", "name"};
    if (auto error = rejectUnknown(object, fields, path, ErrorCode::InvalidTarget))
        return Result<MaterialReference>::failure(std::move(*error));
    const auto source = requiredString(object, "source", path + ".source", ErrorCode::InvalidTarget);
    const auto name = requiredString(object, "name", path + ".name", ErrorCode::InvalidTarget);
    if (!source)
        return Result<MaterialReference>::failure(source.error());
    if (!name)
        return Result<MaterialReference>::failure(name.error());
    MaterialSource parsedSource;
    if (source.value() == "session")
        parsedSource = MaterialSource::Session;
    else if (source.value() == "config")
        parsedSource = MaterialSource::Config;
    else
        return invalid<MaterialReference>(ErrorCode::InvalidTarget, path + ".source", "source must be session or config");
    return Result<MaterialReference>::success({
        .source = parsedSource,
        .name = name.value(),
    });
}

std::optional<RenderStage> parseStage(std::string_view value) {
    if (value == "post-wallpaper") return RenderStage::PostWallpaper;
    if (value == "pre-window") return RenderStage::PreWindow;
    if (value == "post-windows") return RenderStage::PostWindows;
    if (value == "post-layer") return RenderStage::PostLayer;
    return std::nullopt;
}

Result<Target> parseTarget(const json& object, std::size_t index) {
    const auto path = "targets[" + std::to_string(index) + "]";
    if (!object.is_object())
        return invalid<Target>(ErrorCode::InvalidTarget, path, "target must be an object");
    static const std::set<std::string_view> fields{
        "id", "kind", "selector", "geometry", "stage", "material", "shape", "transition", "enabled",
    };
    if (auto error = rejectUnknown(object, fields, path, ErrorCode::InvalidTarget))
        return Result<Target>::failure(std::move(*error));

    const auto id = requiredString(object, "id", path + ".id", ErrorCode::InvalidTarget);
    const auto kind = requiredString(object, "kind", path + ".kind", ErrorCode::InvalidTarget);
    if (!id) return Result<Target>::failure(id.error());
    if (!kind) return Result<Target>::failure(kind.error());
    const auto materialValue = object.find("material");
    const auto shapeValue = object.find("shape");
    const auto selectorValue = object.find("selector");
    if (materialValue == object.end())
        return invalid<Target>(ErrorCode::InvalidTarget, path + ".material", "material reference is required");
    if (shapeValue == object.end())
        return invalid<Target>(ErrorCode::InvalidTarget, path + ".shape", "shape is required");
    if (selectorValue == object.end() || !selectorValue->is_object())
        return invalid<Target>(ErrorCode::InvalidTarget, path + ".selector", "selector must be an object");
    auto material = parseMaterialReference(*materialValue, path + ".material");
    auto shape = parseShape(*shapeValue, path + ".shape");
    if (!material) return Result<Target>::failure(material.error());
    if (!shape) return Result<Target>::failure(shape.error());

    TargetInput input{
        .id = id.value(),
        .kind = TargetKind::Region,
        .material = std::move(material.value()),
        .shape = std::move(shape.value()),
        .selector = RegionSelector{},
        .geometry = std::nullopt,
        .stage = std::nullopt,
        .transition = std::nullopt,
        .enabled = true,
    };
    if (auto enabled = object.find("enabled"); enabled != object.end()) {
        if (!enabled->is_boolean())
            return invalid<Target>(ErrorCode::InvalidTarget, path + ".enabled", "expected a boolean");
        input.enabled = enabled->get<bool>();
    }
    if (const auto transition = object.find("transition"); transition != object.end()) {
        auto parsed = parseTransition(*transition, path + ".transition", false);
        if (!parsed)
            return Result<Target>::failure(parsed.error());
        input.transition = std::move(parsed.value());
    }

    if (kind.value() == "window") {
        input.kind = TargetKind::Window;
        static const std::set<std::string_view> selectorFields{"address", "pid", "initial_class"};
        if (auto error = rejectUnknown(*selectorValue, selectorFields, path + ".selector", ErrorCode::InvalidTarget))
            return Result<Target>::failure(std::move(*error));
        const auto address = requiredString(*selectorValue, "address", path + ".selector.address", ErrorCode::InvalidTarget);
        if (!address) return Result<Target>::failure(address.error());
        WindowSelector selector{
            .address = address.value(),
            .pid = std::nullopt,
            .initialClass = std::nullopt,
        };
        if (auto pid = selectorValue->find("pid"); pid != selectorValue->end()) {
            if (pid->is_number_unsigned()) {
                const auto value = pid->get<std::uint64_t>();
                if (value == 0 || value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                    return invalid<Target>(ErrorCode::InvalidTarget, path + ".selector.pid", "expected a positive signed 64-bit integer");
                selector.pid = static_cast<std::int64_t>(value);
            } else if (pid->is_number_integer()) {
                const auto value = pid->get<std::int64_t>();
                if (value <= 0)
                    return invalid<Target>(ErrorCode::InvalidTarget, path + ".selector.pid", "expected a positive signed 64-bit integer");
                selector.pid = value;
            } else {
                return invalid<Target>(ErrorCode::InvalidTarget, path + ".selector.pid", "expected a positive signed 64-bit integer");
            }
        }
        if (auto initialClass = selectorValue->find("initial_class"); initialClass != selectorValue->end()) {
            const auto parsed = requiredString(
                *selectorValue,
                "initial_class",
                path + ".selector.initial_class",
                ErrorCode::InvalidTarget);
            if (!parsed)
                return Result<Target>::failure(parsed.error());
            selector.initialClass = parsed.value();
        }
        input.selector = std::move(selector);
        if (object.contains("geometry"))
            return invalid<Target>(ErrorCode::InvalidTarget, path + ".geometry", "window targets use compositor-owned window geometry");
        if (object.contains("stage"))
            return invalid<Target>(ErrorCode::InvalidTarget, path + ".stage", "window targets use the window decoration stage");
    } else if (kind.value() == "layer") {
        input.kind = TargetKind::Layer;
        static const std::set<std::string_view> selectorFields{"namespace"};
        if (auto error = rejectUnknown(*selectorValue, selectorFields, path + ".selector", ErrorCode::InvalidTarget))
            return Result<Target>::failure(std::move(*error));
        const auto layerNamespace = requiredString(*selectorValue, "namespace", path + ".selector.namespace", ErrorCode::InvalidTarget);
        if (!layerNamespace) return Result<Target>::failure(layerNamespace.error());
        input.selector = LayerSelector{.namespaceName = layerNamespace.value()};
        if (auto geometry = object.find("geometry"); geometry != object.end()) {
            auto parsed = parseRect(*geometry, path + ".geometry", "surface-local");
            if (!parsed) return Result<Target>::failure(parsed.error());
            input.geometry = parsed.value();
        }
        if (object.contains("stage"))
            return invalid<Target>(ErrorCode::InvalidTarget, path + ".stage", "layer targets derive their render stage from the attached surface");
    } else if (kind.value() == "region") {
        input.kind = TargetKind::Region;
        static const std::set<std::string_view> selectorFields{"output"};
        if (auto error = rejectUnknown(*selectorValue, selectorFields, path + ".selector", ErrorCode::InvalidTarget))
            return Result<Target>::failure(std::move(*error));
        const auto output = requiredString(*selectorValue, "output", path + ".selector.output", ErrorCode::InvalidTarget);
        if (!output) return Result<Target>::failure(output.error());
        input.selector = RegionSelector{.output = output.value()};
        const auto geometry = object.find("geometry");
        if (geometry == object.end())
            return invalid<Target>(ErrorCode::InvalidTarget, path + ".geometry", "region geometry is required");
        auto parsed = parseRect(*geometry, path + ".geometry", "output-logical");
        if (!parsed) return Result<Target>::failure(parsed.error());
        input.geometry = parsed.value();
        const auto stage = requiredString(object, "stage", path + ".stage", ErrorCode::InvalidTarget);
        if (!stage) return Result<Target>::failure(stage.error());
        input.stage = parseStage(stage.value());
        if (!input.stage)
            return invalid<Target>(ErrorCode::InvalidTarget, path + ".stage", "unsupported render stage");
    } else {
        return invalid<Target>(ErrorCode::InvalidTarget, path + ".kind", "unsupported target kind");
    }

    auto target = validateTarget(std::move(input));
    if (!target) {
        auto error = target.error();
        error.path = path + (error.path.empty() ? "" : "." + error.path);
        return Result<Target>::failure(std::move(error));
    }
    return target;
}

Result<Request> parseDocument(const json& document) {
    if (!document.is_object())
        return invalid<Request>(ErrorCode::InvalidRequest, "", "request must be an object");
    const auto version = requiredUnsigned(document, "version", "version");
    if (!version)
        return Result<Request>::failure(version.error());
    if (version.value() != 2U)
        return invalid<Request>(ErrorCode::UnsupportedVersion, "version", "protocol version must be 2");
    const auto operation = requiredString(document, "operation", "operation");
    if (!operation)
        return Result<Request>::failure(operation.error());

    std::optional<std::string> requestId;
    if (auto value = document.find("request_id"); value != document.end()) {
        if (!value->is_string())
            return invalid<Request>(ErrorCode::InvalidRequest, "request_id", "expected a string");
        const auto parsed = value->get<std::string>();
        if (parsed.empty() || parsed.size() > Limits::MAX_IDENTIFIER_BYTES)
            return invalid<Request>(ErrorCode::InvalidRequest, "request_id", "expected a non-empty string no longer than 128 bytes");
        requestId = parsed;
    }

    const auto finish = [&](RequestBody body, std::set<std::string_view> fields) -> Result<Request> {
        fields.insert("version");
        fields.insert("operation");
        fields.insert("request_id");
        if (auto error = rejectUnknown(document, fields, ""))
            return Result<Request>::failure(std::move(*error));
        return Result<Request>::success({
            .requestId = requestId,
            .body = std::move(body),
        });
    };

    if (operation.value() == "capabilities")
        return finish(CapabilitiesRequest{}, {});
    if (operation.value() == "status")
        return finish(StatusRequest{}, {});
    if (operation.value() == "session.open") {
        const auto clientId = requiredString(document, "client_id", "client_id");
        const auto mode = requiredString(document, "mode", "mode");
        if (!clientId) return Result<Request>::failure(clientId.error());
        if (!mode) return Result<Request>::failure(mode.error());
        SessionMode parsedMode;
        if (mode.value() == "client")
            parsedMode = SessionMode::Client;
        else if (mode.value() == "preview")
            parsedMode = SessionMode::Preview;
        else
            return invalid<Request>(ErrorCode::InvalidRequest, "mode", "mode must be client or preview");
        return finish(OpenSessionRequest{.clientId = clientId.value(), .mode = parsedMode}, {"client_id", "mode"});
    }
    if (operation.value() == "session.replace") {
        const auto sessionId = requiredString(document, "session_id", "session_id");
        const auto token = requiredString(document, "token", "token");
        const auto generation = requiredUnsigned(document, "generation", "generation");
        if (!sessionId) return Result<Request>::failure(sessionId.error());
        if (!token) return Result<Request>::failure(token.error());
        if (!generation) return Result<Request>::failure(generation.error());

        SessionReplacement replacement{
            .generation = generation.value(),
            .materials = {},
            .targets = {},
            .handoffs = {},
            .visibilityTransitions = {},
        };
        const auto materials = document.find("materials");
        if (materials == document.end() || !materials->is_object())
            return invalid<Request>(ErrorCode::InvalidRequest, "materials", "materials must be an object");
        if (materials->size() > Limits::MAX_MATERIALS_PER_OWNER)
            return invalid<Request>(ErrorCode::ResourceLimited, "materials", "material limit exceeded");
        for (const auto& [name, value] : materials->items()) {
            auto material = parseMaterial(name, value, "materials." + name);
            if (!material) return Result<Request>::failure(material.error());
            replacement.materials.emplace(name, std::move(material.value()));
        }

        const auto targets = document.find("targets");
        if (targets == document.end() || !targets->is_array())
            return invalid<Request>(ErrorCode::InvalidRequest, "targets", "targets must be an array");
        if (targets->size() > Limits::MAX_TARGETS_PER_SESSION)
            return invalid<Request>(ErrorCode::ResourceLimited, "targets", "per-session target limit exceeded");
        for (std::size_t index = 0; index < targets->size(); ++index) {
            auto target = parseTarget((*targets)[index], index);
            if (!target) return Result<Request>::failure(target.error());
            replacement.targets.push_back(std::move(target.value()));
        }

        if (const auto handoffs = document.find("handoffs");
            handoffs != document.end()) {
            if (!handoffs->is_array())
                return invalid<Request>(
                    ErrorCode::InvalidRequest,
                    "handoffs",
                    "handoffs must be an array");
            if (handoffs->size() > Limits::MAX_TARGETS_PER_SESSION)
                return invalid<Request>(
                    ErrorCode::ResourceLimited,
                    "handoffs",
                    "handoff limit exceeded");
            for (std::size_t index = 0; index < handoffs->size(); ++index) {
                const auto& value = (*handoffs)[index];
                const auto path = "handoffs[" + std::to_string(index) + "]";
                if (!value.is_object())
                    return invalid<Request>(
                        ErrorCode::InvalidRequest,
                        path,
                        "handoff must be an object");
                if (auto error = rejectUnknown(
                        value,
                        {"target_id", "source_generation", "mode", "timeout_ms",
                         "morph"},
                        path))
                    return Result<Request>::failure(std::move(*error));
                const auto targetId = requiredString(
                    value,
                    "target_id",
                    path + ".target_id");
                const auto sourceGeneration = requiredUnsigned(
                    value,
                    "source_generation",
                    path + ".source_generation");
                const auto mode = requiredString(
                    value,
                    "mode",
                    path + ".mode");
                const auto timeoutMs = requiredUnsigned(
                    value,
                    "timeout_ms",
                    path + ".timeout_ms");
                if (!targetId)
                    return Result<Request>::failure(targetId.error());
                if (!sourceGeneration)
                    return Result<Request>::failure(sourceGeneration.error());
                if (!mode)
                    return Result<Request>::failure(mode.error());
                if (!timeoutMs)
                    return Result<Request>::failure(timeoutMs.error());
                if (mode.value() != "retain-until-drawn")
                    return invalid<Request>(
                        ErrorCode::UnsupportedOperation,
                        path + ".mode",
                        "handoff mode must be retain-until-drawn");
                std::optional<PresentationHandoffRequest::Morph> morph;
                if (const auto found = value.find("morph");
                    found != value.end()) {
                    if (!found->is_object())
                        return invalid<Request>(
                            ErrorCode::InvalidRequest,
                            path + ".morph",
                            "handoff morph must be an object");
                    if (auto error = rejectUnknown(
                            *found,
                            {"transition_id", "duration_ms", "easing",
                             "anchor", "coordinate_space", "source",
                             "destination"},
                            path + ".morph"))
                        return Result<Request>::failure(std::move(*error));
                    const auto transitionId = requiredString(
                        *found, "transition_id",
                        path + ".morph.transition_id");
                    const auto duration = requiredUnsigned(
                        *found, "duration_ms",
                        path + ".morph.duration_ms");
                    const auto easing = requiredString(
                        *found, "easing",
                        path + ".morph.easing");
                    const auto anchor = requiredString(
                        *found, "anchor",
                        path + ".morph.anchor");
                    if (!transitionId)
                        return Result<Request>::failure(transitionId.error());
                    if (!duration)
                        return Result<Request>::failure(duration.error());
                    if (!easing)
                        return Result<Request>::failure(easing.error());
                    if (!anchor)
                        return Result<Request>::failure(anchor.error());
                    if (easing.value() != "ease-out-cubic")
                        return invalid<Request>(
                            ErrorCode::UnsupportedOperation,
                            path + ".morph.easing",
                            "handoff morph easing is unsupported");
                    if (anchor.value() != "compositor-monotonic")
                        return invalid<Request>(
                            ErrorCode::UnsupportedOperation,
                            path + ".morph.anchor",
                            "handoff morph anchor is unsupported");
                    auto coordinateSpace =
                        PresentationHandoffRequest::MorphCoordinateSpace::
                            SurfaceLocal;
                    if (const auto space =
                            found->find("coordinate_space");
                        space != found->end()) {
                        if (!space->is_string())
                            return invalid<Request>(
                                ErrorCode::InvalidRequest,
                                path + ".morph.coordinate_space",
                                "handoff morph coordinate space must be a string");
                        if (space->get<std::string>() == "surface-local")
                            coordinateSpace =
                                PresentationHandoffRequest::
                                    MorphCoordinateSpace::SurfaceLocal;
                        else if (space->get<std::string>() ==
                                 "output-local")
                            coordinateSpace =
                                PresentationHandoffRequest::
                                    MorphCoordinateSpace::OutputLocal;
                        else
                            return invalid<Request>(
                                ErrorCode::UnsupportedOperation,
                                path + ".morph.coordinate_space",
                                "handoff morph coordinate space is unsupported");
                    }
                    std::optional<
                        PresentationHandoffRequest::MorphEndpoint> source;
                    std::optional<
                        PresentationHandoffRequest::MorphEndpoint>
                        destination;
                    const auto sourceValue = found->find("source");
                    const auto destinationValue =
                        found->find("destination");
                    if (coordinateSpace ==
                        PresentationHandoffRequest::MorphCoordinateSpace::
                            OutputLocal) {
                        if (sourceValue == found->end() ||
                            destinationValue == found->end())
                            return invalid<Request>(
                                ErrorCode::InvalidRequest,
                                path + ".morph",
                                "output-local morph requires source and destination endpoints");
                        auto parsedSource = parseMorphEndpoint(
                            *sourceValue,
                            path + ".morph.source");
                        if (!parsedSource)
                            return Result<Request>::failure(
                                parsedSource.error());
                        auto parsedDestination = parseMorphEndpoint(
                            *destinationValue,
                            path + ".morph.destination");
                        if (!parsedDestination)
                            return Result<Request>::failure(
                                parsedDestination.error());
                        source = parsedSource.value();
                        destination = parsedDestination.value();
                    } else if (sourceValue != found->end() ||
                               destinationValue != found->end())
                        return invalid<Request>(
                            ErrorCode::InvalidRequest,
                            path + ".morph",
                            "surface-local morph endpoints are derived from the targets");
                    morph = PresentationHandoffRequest::Morph{
                        .transitionId = transitionId.value(),
                        .durationMs = duration.value(),
                        .coordinateSpace = coordinateSpace,
                        .source = std::move(source),
                        .destination = std::move(destination),
                    };
                }
                replacement.handoffs.push_back({
                    .targetId = targetId.value(),
                    .sourceGeneration = sourceGeneration.value(),
                    .timeoutMs = timeoutMs.value(),
                    .morph = std::move(morph),
                });
            }
        }
        if (const auto transitions =
                document.find("visibility_transitions");
            transitions != document.end()) {
            if (!transitions->is_array())
                return invalid<Request>(
                    ErrorCode::InvalidRequest,
                    "visibility_transitions",
                    "visibility transitions must be an array");
            if (transitions->size() > Limits::MAX_TARGETS_PER_SESSION)
                return invalid<Request>(
                    ErrorCode::ResourceLimited,
                    "visibility_transitions",
                    "visibility transition limit exceeded");
            for (std::size_t index = 0;
                 index < transitions->size();
                 ++index) {
                const auto& value = (*transitions)[index];
                const auto path =
                    "visibility_transitions[" +
                    std::to_string(index) + "]";
                if (!value.is_object())
                    return invalid<Request>(
                        ErrorCode::InvalidRequest,
                        path,
                        "visibility transition must be an object");
                if (auto error = rejectUnknown(
                        value,
                        {"target_id", "transition_id",
                         "source_generation", "direction", "edge",
                         "source_rect", "source_radius", "travel", "duration_ms",
                         "easing", "anchor", "activation",
                         "timeout_ms", "output", "namespace"},
                        path))
                    return Result<Request>::failure(std::move(*error));
                const auto targetId = requiredString(
                    value, "target_id", path + ".target_id");
                const auto transitionId = requiredString(
                    value, "transition_id", path + ".transition_id");
                const auto sourceGeneration = requiredUnsigned(
                    value, "source_generation",
                    path + ".source_generation");
                const auto direction = requiredString(
                    value, "direction", path + ".direction");
                const auto edge = requiredString(
                    value, "edge", path + ".edge");
                const auto duration = requiredUnsigned(
                    value, "duration_ms", path + ".duration_ms");
                const auto easing = requiredString(
                    value, "easing", path + ".easing");
                const auto anchor = requiredString(
                    value, "anchor", path + ".anchor");
                const auto activation = requiredString(
                    value, "activation", path + ".activation");
                const auto timeout = requiredUnsigned(
                    value, "timeout_ms", path + ".timeout_ms");
                const auto output = requiredString(
                    value, "output", path + ".output");
                const auto namespaceName = requiredString(
                    value, "namespace", path + ".namespace");
                if (!targetId) return Result<Request>::failure(targetId.error());
                if (!transitionId) return Result<Request>::failure(transitionId.error());
                if (!sourceGeneration) return Result<Request>::failure(sourceGeneration.error());
                if (!direction) return Result<Request>::failure(direction.error());
                if (!edge) return Result<Request>::failure(edge.error());
                if (!duration) return Result<Request>::failure(duration.error());
                if (!easing) return Result<Request>::failure(easing.error());
                if (!anchor) return Result<Request>::failure(anchor.error());
                if (!activation) return Result<Request>::failure(activation.error());
                if (!timeout) return Result<Request>::failure(timeout.error());
                if (!output) return Result<Request>::failure(output.error());
                if (!namespaceName) return Result<Request>::failure(namespaceName.error());
                if (easing.value() != "ease-out-cubic" ||
                    anchor.value() != "compositor-monotonic" ||
                    activation.value() != "first-successful-draw")
                    return invalid<Request>(
                        ErrorCode::UnsupportedOperation,
                        path,
                        "visibility transition timing contract is unsupported");
                VisibilityTransitionDirection parsedDirection;
                if (direction.value() == "hide")
                    parsedDirection = VisibilityTransitionDirection::Hide;
                else if (direction.value() == "reveal")
                    parsedDirection = VisibilityTransitionDirection::Reveal;
                else
                    return invalid<Request>(
                        ErrorCode::InvalidRequest,
                        path + ".direction",
                        "visibility transition direction must be hide or reveal");
                TransitionEdge parsedEdge;
                if (edge.value() == "top") parsedEdge = TransitionEdge::Top;
                else if (edge.value() == "bottom") parsedEdge = TransitionEdge::Bottom;
                else if (edge.value() == "left") parsedEdge = TransitionEdge::Left;
                else if (edge.value() == "right") parsedEdge = TransitionEdge::Right;
                else
                    return invalid<Request>(
                        ErrorCode::InvalidRequest,
                        path + ".edge",
                        "visibility transition edge is unsupported");
                const auto rectValue = value.find("source_rect");
                if (rectValue == value.end())
                    return invalid<Request>(
                        ErrorCode::InvalidRequest,
                        path + ".source_rect",
                        "visibility transition requires a source rectangle");
                auto sourceRect = parseLocalRect(
                    *rectValue, path + ".source_rect");
                if (!sourceRect)
                    return Result<Request>::failure(sourceRect.error());
                const auto sourceRadiusValue =
                    value.find("source_radius");
                if (sourceRadiusValue == value.end() ||
                    !sourceRadiusValue->is_number())
                    return invalid<Request>(
                        ErrorCode::InvalidRequest,
                        path + ".source_radius",
                        "visibility transition source radius must be a number");
                const auto sourceRadius =
                    sourceRadiusValue->get<double>();
                if (!std::isfinite(sourceRadius))
                    return invalid<Request>(
                        ErrorCode::InvalidRequest,
                        path + ".source_radius",
                        "visibility transition source radius must be finite");
                const auto travelValue = value.find("travel");
                if (travelValue == value.end() ||
                    !travelValue->is_number())
                    return invalid<Request>(
                        ErrorCode::InvalidRequest,
                        path + ".travel",
                        "visibility transition travel must be a number");
                const auto travel = travelValue->get<double>();
                if (!std::isfinite(travel))
                    return invalid<Request>(
                        ErrorCode::InvalidRequest,
                        path + ".travel",
                        "visibility transition travel must be finite");
                replacement.visibilityTransitions.push_back({
                    .targetId = targetId.value(),
                    .transitionId = transitionId.value(),
                    .sourceGeneration = sourceGeneration.value(),
                    .direction = parsedDirection,
                    .edge = parsedEdge,
                    .sourceRect = sourceRect.value(),
                    .sourceRadius = sourceRadius,
                    .travel = travel,
                    .durationMs = duration.value(),
                    .timeoutMs = timeout.value(),
                    .output = output.value(),
                    .namespaceName = namespaceName.value(),
                });
            }
        }
        return finish(ReplaceSessionRequest{
            .sessionId = sessionId.value(),
            .token = token.value(),
            .replacement = std::move(replacement),
        }, {"session_id", "token", "generation", "materials", "targets",
            "handoffs", "visibility_transitions"});
    }
    if (operation.value() == "session.heartbeat") {
        const auto sessionId = requiredString(document, "session_id", "session_id");
        const auto token = requiredString(document, "token", "token");
        const auto generation = requiredUnsigned(document, "generation", "generation");
        if (!sessionId) return Result<Request>::failure(sessionId.error());
        if (!token) return Result<Request>::failure(token.error());
        if (!generation) return Result<Request>::failure(generation.error());
        return finish(HeartbeatSessionRequest{
            .sessionId = sessionId.value(),
            .token = token.value(),
            .generation = generation.value(),
        }, {"session_id", "token", "generation"});
    }
    if (operation.value() == "session.close") {
        const auto sessionId = requiredString(document, "session_id", "session_id");
        const auto token = requiredString(document, "token", "token");
        if (!sessionId) return Result<Request>::failure(sessionId.error());
        if (!token) return Result<Request>::failure(token.error());
        return finish(CloseSessionRequest{
            .sessionId = sessionId.value(),
            .token = token.value(),
        }, {"session_id", "token"});
    }
    if (operation.value() == "target.inspect") {
        const auto sessionId = requiredString(document, "session_id", "session_id");
        const auto token = requiredString(document, "token", "token");
        const auto targetId = requiredString(document, "target_id", "target_id");
        if (!sessionId) return Result<Request>::failure(sessionId.error());
        if (!token) return Result<Request>::failure(token.error());
        if (!targetId) return Result<Request>::failure(targetId.error());
        return finish(InspectTargetRequest{
            .sessionId = sessionId.value(),
            .token = token.value(),
            .targetId = targetId.value(),
        }, {"session_id", "token", "target_id"});
    }

    return invalid<Request>(ErrorCode::UnsupportedOperation, "operation", "unsupported operation");
}

} // namespace

Result<Request> parseRequest(std::string_view payload) {
    if (payload.size() > Limits::MAX_REQUEST_BYTES)
        return invalid<Request>(ErrorCode::ResourceLimited, "", "request exceeds 256 KiB");
    try {
        StrictParseState state;
        return parseDocument(json::parse(
            payload,
            [&state](int depth, json::parse_event_t event, json& parsed) {
                return state(depth, event, parsed);
            }));
    } catch (const ParseConstraintError& error) {
        return Result<Request>::failure(error.error());
    } catch (const json::parse_error&) {
        return invalid<Request>(ErrorCode::InvalidJson, "", "request is not valid JSON");
    } catch (const json::exception&) {
        return invalid<Request>(ErrorCode::InvalidRequest, "", "request contains an invalid JSON value");
    } catch (const std::exception&) {
        return invalid<Request>(ErrorCode::InvalidRequest, "", "request could not be validated");
    }
}

std::string successResponse(
    const std::optional<std::string>& requestId,
    const nlohmann::json& result) {
    json response{
        {"ok", true},
        {"version", 2},
        {"result", result},
    };
    if (requestId)
        response["request_id"] = *requestId;
    return response.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string failureResponse(
    const std::optional<std::string>& requestId,
    const Error& error) {
    json response{
        {"ok", false},
        {"version", 2},
        {"error", {
            {"code", errorCodeName(error.code)},
            {"path", error.path},
            {"message", error.message},
        }},
    };
    if (requestId)
        response["request_id"] = *requestId;
    return response.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace hfg::v2
