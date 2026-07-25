#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Material.hpp"

#include <cstdint>

namespace hfg::v2 {

struct MaterialSamplingFootprint {
    double        blurPixels = 0.0;
    double        refractionPixels = 0.0;
    double        chromaticPixels = 0.0;
    double        lensPixels = 0.0;
    std::uint32_t apronPixels = 0;

    friend bool operator==(
        const MaterialSamplingFootprint&,
        const MaterialSamplingFootprint&) = default;
};

[[nodiscard]] Result<MaterialSamplingFootprint>
resolveMaterialSampling(
    const Material& material,
    double logicalWidth,
    double logicalHeight,
    double outputScale);

} // namespace hfg::v2
