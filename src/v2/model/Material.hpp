#pragma once

#include "v2/core/Result.hpp"

#include <optional>
#include <string>

namespace hfg::v2 {

struct RgbColor {
    double red   = 1.0;
    double green = 1.0;
    double blue  = 1.0;

    friend bool operator==(const RgbColor&, const RgbColor&) = default;
};

struct MaterialInput {
    double                glassLevel  = 0.50;
    std::optional<double> blurLevel;
    std::optional<double> tintLevel;
    bool                  tintEnabled = false;
    std::string           tintColor   = "#FFFFFF";
    bool                  lightMode   = false;

    double refraction = 45.0;
    double rimBand    = 30.0;
    double bevel      = 30.0;
    double rimWidth   = 3.0;
    double highlight  = 0.10;
    double shadow     = 0.10;
    double lightAngle = 90.0;
    double specular   = 0.21;
    double chroma     = 0.15;
    double edgeDepth  = 0.14;
    double lens       = 0.12;
    double lensBand   = 40.0;
    double gloss      = 0.14;
};

struct Material {
    std::string           name;
    double                glassLevel = 0.50;
    std::optional<double> blurLevel;
    std::optional<double> tintLevel;
    bool                  tintEnabled = false;
    RgbColor              tintColor;
    bool                  lightMode = false;

    double refraction = 45.0;
    double rimBand    = 30.0;
    double bevel      = 30.0;
    double rimWidth   = 3.0;
    double highlight  = 0.10;
    double shadow     = 0.10;
    double lightAngle = 90.0;
    double specular   = 0.21;
    double chroma     = 0.15;
    double edgeDepth  = 0.14;
    double lens       = 0.12;
    double lensBand   = 40.0;
    double gloss      = 0.14;

    friend bool operator==(const Material&, const Material&) = default;
};

[[nodiscard]] Result<Material> validateMaterial(std::string name, const MaterialInput& input);

} // namespace hfg::v2
