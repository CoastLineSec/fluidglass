#include "v2/model/Material.hpp"

#include "v2/core/Limits.hpp"

#include <array>
#include <cctype>
#include <cmath>
#include <string_view>

namespace hfg::v2 {
namespace {

Result<Material> invalid(std::string path, std::string message) {
    return Result<Material>::failure({
        .code = ErrorCode::InvalidMaterial,
        .path = std::move(path),
        .message = std::move(message),
    });
}

bool validName(std::string_view name) {
    if (name.empty() || name.size() > Limits::MAX_IDENTIFIER_BYTES || name.starts_with("_hfg_"))
        return false;
    for (const unsigned char character : name) {
        if (!std::isalnum(character) && character != '_' && character != '-' && character != '.')
            return false;
    }
    return true;
}

bool inRange(double value, double minimum, double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

std::optional<RgbColor> parseColor(std::string_view value) {
    if (value.size() != 7 || value.front() != '#')
        return std::nullopt;

    const auto hexValue = [](char character) -> std::optional<unsigned> {
        if (character >= '0' && character <= '9')
            return static_cast<unsigned>(character - '0');
        if (character >= 'a' && character <= 'f')
            return static_cast<unsigned>(character - 'a' + 10);
        if (character >= 'A' && character <= 'F')
            return static_cast<unsigned>(character - 'A' + 10);
        return std::nullopt;
    };

    std::array<unsigned, 6> digits{};
    for (std::size_t index = 0; index < digits.size(); ++index) {
        const auto digit = hexValue(value[index + 1]);
        if (!digit)
            return std::nullopt;
        digits[index] = *digit;
    }

    const auto channel = [&](std::size_t index) {
        return static_cast<double>(digits[index] * 16U + digits[index + 1]) / 255.0;
    };
    return RgbColor{
        .red = channel(0),
        .green = channel(2),
        .blue = channel(4),
    };
}

} // namespace

Result<Material> validateMaterial(std::string name, const MaterialInput& input) {
    if (!validName(name))
        return invalid("name", "expected 1-128 ASCII letters, digits, '.', '_' or '-' without the reserved _hfg_ prefix");

    const auto color = parseColor(input.tintColor);
    if (!color)
        return invalid("tint_color", "expected #RRGGBB");

    const auto check = [&](double value, double minimum, double maximum, std::string_view field) -> std::optional<Result<Material>> {
        if (inRange(value, minimum, maximum))
            return std::nullopt;
        return invalid(std::string(field), "expected a finite number in the documented range");
    };

    if (auto error = check(input.glassLevel, 0.0, 1.0, "glass_level")) return *error;
    if (input.blurLevel)
        if (auto error = check(*input.blurLevel, 0.0, 1.0, "blur_level")) return *error;
    if (input.tintLevel)
        if (auto error = check(*input.tintLevel, 0.0, 1.0, "tint_level")) return *error;
    if (auto error = check(input.refraction, 0.0, 200.0, "refraction")) return *error;
    if (auto error = check(input.rimBand, 0.0, 200.0, "rim_band")) return *error;
    if (auto error = check(input.bevel, 0.0, 200.0, "bevel")) return *error;
    if (auto error = check(input.rimWidth, 0.0, 50.0, "rim_width")) return *error;
    if (auto error = check(input.highlight, 0.0, 2.0, "highlight")) return *error;
    if (auto error = check(input.shadow, 0.0, 2.0, "shadow")) return *error;
    if (auto error = check(input.lightAngle, 0.0, 360.0, "light_angle")) return *error;
    if (auto error = check(input.specular, 0.0, 2.0, "specular")) return *error;
    if (auto error = check(input.chroma, 0.0, 1.0, "chroma")) return *error;
    if (auto error = check(input.edgeDepth, 0.0, 2.0, "edge_depth")) return *error;
    if (auto error = check(input.lens, 0.0, 1.0, "lens")) return *error;
    if (auto error = check(input.lensBand, 0.0, 200.0, "lens_band")) return *error;
    if (auto error = check(input.gloss, 0.0, 2.0, "gloss")) return *error;

    return Result<Material>::success({
        .name = std::move(name),
        .glassLevel = input.glassLevel,
        .blurLevel = input.blurLevel,
        .tintLevel = input.tintLevel,
        .tintEnabled = input.tintEnabled,
        .tintColor = *color,
        .lightMode = input.lightMode,
        .refraction = input.refraction,
        .rimBand = input.rimBand,
        .bevel = input.bevel,
        .rimWidth = input.rimWidth,
        .highlight = input.highlight,
        .shadow = input.shadow,
        .lightAngle = input.lightAngle,
        .specular = input.specular,
        .chroma = input.chroma,
        .edgeDepth = input.edgeDepth,
        .lens = input.lens,
        .lensBand = input.lensBand,
        .gloss = input.gloss,
    });
}

} // namespace hfg::v2
