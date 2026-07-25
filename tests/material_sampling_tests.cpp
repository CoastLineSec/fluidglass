#include "TestHarness.hpp"

#include "v2/render/MaterialSampling.hpp"

#include <cmath>
#include <limits>
#include <utility>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

Material material(const MaterialInput& input = {}) {
    auto result = validateMaterial("fluid", input);
    require(result.hasValue(), "test material was invalid");
    return std::move(result.value());
}

void requireNear(
    double actual,
    double expected,
    std::string_view message) {
    require(std::abs(actual - expected) < 1e-9, message);
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"default material footprint includes every sampling displacement", [] {
            const auto result = resolveMaterialSampling(
                material(),
                100.0,
                50.0,
                1.0);
            require(result.hasValue(), "default footprint failed");
            requireNear(result.value().blurPixels, 18.0,
                        "default blur changed");
            requireNear(result.value().refractionPixels, 20.0,
                        "small-surface refraction clamp changed");
            requireNear(result.value().chromaticPixels, 3.0,
                        "chromatic reach was not included");
            requireNear(result.value().lensPixels, 1.65,
                        "lens reach was not included");
            require(result.value().apronPixels == 73U,
                    "default capture apron changed");
        }},
        Case{"blur override is independent from glass level", [] {
            MaterialInput input;
            input.glassLevel = 0.0;
            input.blurLevel = 1.0;
            const auto result = resolveMaterialSampling(
                material(input),
                1000.0,
                1000.0,
                1.0);
            require(result.hasValue(), "override footprint failed");
            requireNear(result.value().blurPixels, 88.0,
                        "blur override did not select maximum frost");
        }},
        Case{"fractional scale applies exactly once", [] {
            MaterialInput input;
            input.refraction = 10.0;
            input.chroma = 0.0;
            input.lens = 0.0;
            input.blurLevel = 0.0;
            const auto result = resolveMaterialSampling(
                material(input),
                200.0,
                100.0,
                1.25);
            require(result.hasValue(), "fractional footprint failed");
            requireNear(result.value().blurPixels, 2.5,
                        "blur scale changed");
            requireNear(result.value().refractionPixels, 12.5,
                        "refraction scale changed");
            require(result.value().apronPixels == 30U,
                    "fractional apron did not round outward");
        }},
        Case{"maximum chroma and lens reach are bounded conservatively", [] {
            MaterialInput input;
            input.glassLevel = 1.0;
            input.refraction = 200.0;
            input.chroma = 1.0;
            input.lens = 1.0;
            input.lensBand = 200.0;
            const auto result = resolveMaterialSampling(
                material(input),
                1000.0,
                1000.0,
                2.0);
            require(result.hasValue(), "maximum footprint failed");
            requireNear(result.value().blurPixels, 176.0,
                        "maximum blur changed");
            requireNear(result.value().refractionPixels, 400.0,
                        "maximum refraction changed");
            requireNear(result.value().chromaticPixels, 400.0,
                        "maximum chromatic reach changed");
            requireNear(result.value().lensPixels, 400.0,
                        "maximum lens reach changed");
            require(result.value().apronPixels == 1564U,
                    "maximum conservative apron changed");
        }},
        Case{"malformed geometry scale and material fields fail closed", [] {
            const auto valid = material();
            require(!resolveMaterialSampling(
                        valid,
                        std::numeric_limits<double>::quiet_NaN(),
                        10.0,
                        1.0),
                    "NaN geometry was accepted");
            require(!resolveMaterialSampling(
                        valid,
                        10.0,
                        10.0,
                        std::numeric_limits<double>::infinity()),
                    "infinite scale was accepted");

            auto malformed = valid;
            malformed.chroma =
                std::numeric_limits<double>::quiet_NaN();
            require(!resolveMaterialSampling(
                        malformed,
                        10.0,
                        10.0,
                        1.0),
                    "malformed material was accepted");
        }},
    });
}
