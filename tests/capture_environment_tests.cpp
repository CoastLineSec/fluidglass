#include "TestHarness.hpp"

#include "v2/core/Limits.hpp"
#include "v2/render/CaptureEnvironment.hpp"

#include <cstdint>
#include <limits>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

CaptureBudget budget() {
    return {
        .maxApronPixels = 2048,
        .maxPixels = 64U * 1024U * 1024U,
        .maxBytes = 256U * 1024U * 1024U,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"renderer and policy limits combine conservatively", [] {
            const auto result = resolveCaptureLimits(
                16'384,
                8,
                budget());
            require(result.hasValue(), "valid capture environment failed");
            require(result.value().maxWidth == 16'384U &&
                        result.value().maxHeight == 16'384U,
                    "renderer texture dimension changed");
            require(result.value().maxApronPixels == 2048U,
                    "apron policy changed");
            require(result.value().maxPixels ==
                        64U * 1024U * 1024U,
                    "pixel budget changed");
            require(result.value().maxBytes ==
                        256U * 1024U * 1024U,
                    "byte budget changed");
        }},
        Case{"plugin dimension ceiling is always enforced", [] {
            auto large = budget();
            large.maxPixels =
                std::numeric_limits<std::uint64_t>::max();
            large.maxBytes =
                std::numeric_limits<std::uint64_t>::max();
            const auto result = resolveCaptureLimits(
                std::numeric_limits<std::uint32_t>::max(),
                4,
                large);
            require(result.hasValue(), "bounded environment failed");
            require(result.value().maxWidth ==
                        Limits::MAX_OUTPUT_BUFFER_DIMENSION,
                    "plugin dimension ceiling was bypassed");
            require(result.value().maxPixels ==
                        static_cast<std::uint64_t>(
                            Limits::MAX_OUTPUT_BUFFER_DIMENSION) *
                            Limits::MAX_OUTPUT_BUFFER_DIMENSION,
                    "dimension-derived pixel ceiling changed");
        }},
        Case{"byte ceiling respects actual format size", [] {
            auto large = budget();
            large.maxPixels = 100;
            large.maxBytes = 10'000;
            const auto result = resolveCaptureLimits(
                1024,
                8,
                large);
            require(result.hasValue(), "format-sized environment failed");
            require(result.value().maxBytes == 800U,
                    "byte ceiling exceeded representable pixels");
        }},
        Case{"apron is bounded by the texture dimension", [] {
            auto small = budget();
            small.maxApronPixels = 2048;
            const auto result = resolveCaptureLimits(
                512,
                4,
                small);
            require(result.hasValue(), "small environment failed");
            require(result.value().maxApronPixels == 512U,
                    "apron exceeded the texture dimension");
        }},
        Case{"invalid adapter and budget values fail closed", [] {
            require(!resolveCaptureLimits(0, 4, budget()),
                    "zero texture dimension was accepted");
            require(!resolveCaptureLimits(1024, 0, budget()),
                    "zero format size was accepted");
            require(!resolveCaptureLimits(1024, 65, budget()),
                    "oversized format was accepted");

            auto invalid = budget();
            invalid.maxPixels = 0;
            require(!resolveCaptureLimits(1024, 4, invalid),
                    "zero pixel budget was accepted");
            invalid = budget();
            invalid.maxBytes = 0;
            require(!resolveCaptureLimits(1024, 4, invalid),
                    "zero byte budget was accepted");
        }},
    });
}
