#include "TestHarness.hpp"

#include "v2/model/Material.hpp"

#include <cmath>
#include <limits>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

int main() {
    return hfg::test::run({
        Case{"compatibility defaults", [] {
            const auto result = validateMaterial("fluid", {});
            require(result.hasValue(), "default material must be valid");
            require(result.value().glassLevel == 0.5, "glass default changed");
            require(!result.value().blurLevel, "blur must derive when omitted");
            require(result.value().refraction == 45.0, "refraction default changed");
            require(result.value().tintColor == RgbColor{}, "default tint must be white");
        }},
        Case{"explicit material", [] {
            MaterialInput input;
            input.blurLevel = 0.25;
            input.tintEnabled = true;
            input.tintColor = "#3366CC";
            const auto result = validateMaterial("night.clear-1", input);
            require(result.hasValue(), "valid explicit material was rejected");
            require(result.value().blurLevel == 0.25, "explicit blur changed");
            require(std::abs(result.value().tintColor.blue - 0.8) < 0.000001, "blue channel parsed incorrectly");
        }},
        Case{"invalid names", [] {
            require(!validateMaterial("", {}), "empty name must fail");
            require(!validateMaterial("_hfg_internal", {}), "reserved name must fail");
            require(!validateMaterial("bad/name", {}), "path separator must fail");
            require(!validateMaterial(std::string(129, 'a'), {}), "overlong name must fail");
        }},
        Case{"invalid color", [] {
            MaterialInput input;
            input.tintColor = "#12345Z";
            const auto result = validateMaterial("fluid", input);
            require(!result, "invalid color must fail");
            require(result.error().path == "tint_color", "invalid color path changed");
        }},
        Case{"non-finite values", [] {
            MaterialInput input;
            input.glassLevel = std::numeric_limits<double>::quiet_NaN();
            require(!validateMaterial("fluid", input), "NaN must fail");
            input.glassLevel = 0.5;
            input.refraction = std::numeric_limits<double>::infinity();
            require(!validateMaterial("fluid", input), "infinity must fail");
        }},
        Case{"out of range values", [] {
            MaterialInput input;
            input.glassLevel = 1.01;
            require(!validateMaterial("fluid", input), "glass level above one must fail");
            input.glassLevel = 0.5;
            input.rimWidth = -0.01;
            require(!validateMaterial("fluid", input), "negative rim width must fail");
            input.rimWidth = 3.0;
            input.lightAngle = 361.0;
            require(!validateMaterial("fluid", input), "angle above 360 must fail");
        }},
        Case{"optional levels are validated", [] {
            MaterialInput input;
            input.blurLevel = -0.1;
            require(!validateMaterial("fluid", input), "negative explicit blur must fail");
            input.blurLevel = std::nullopt;
            input.tintLevel = 1.1;
            require(!validateMaterial("fluid", input), "tint above one must fail");
        }},
    });
}
