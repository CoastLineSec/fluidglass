#include "TestHarness.hpp"

#include "v2/render/CaptureResourcePlan.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

CapturePlan capture(
    PixelRect region,
    std::uint64_t generation = 1,
    RenderStage stage = RenderStage::PostWindows,
    std::uint32_t format = 0x34325241U,
    std::uint64_t colorState = 7) {
    const auto pixels =
        static_cast<std::uint64_t>(region.width) *
        static_cast<std::uint64_t>(region.height);
    return {
        .key = {
            .output = "DP-1",
            .outputGeneration = generation,
            .stage = stage,
            .renderFormat = format,
            .colorStateToken = colorState,
        },
        .region = region,
        .bytesPerPixel = 4,
        .pixelCount = pixels,
        .byteCount = pixels * 4U,
    };
}

CaptureResource resource(
    std::uint64_t token,
    PixelRect region,
    std::uint64_t generation = 1,
    RenderStage stage = RenderStage::PostWindows,
    std::uint32_t format = 0x34325241U,
    std::uint64_t colorState = 7) {
    return {
        .token = token,
        .plan = capture(
            region,
            generation,
            stage,
            format,
            colorState),
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"exact resource is retained and bound", [] {
            const std::array current{
                resource(9, {10, 20, 100, 80}),
            };
            const std::array desired{
                capture({10, 20, 100, 80}),
            };
            const auto result = planCaptureResources(
                current,
                desired,
                1'000'000);
            require(result.hasValue(), "valid resource plan failed");
            require(result.value().retain == std::vector{current.front()},
                    "exact resource was not retained");
            require(result.value().allocate.empty(),
                    "exact resource was reallocated");
            require(result.value().retire.empty(),
                    "exact resource was retired");
            require(result.value().bindings.size() == 1U &&
                        result.value().bindings[0].retainedToken ==
                            9U &&
                        !result.value().bindings[0].allocationIndex,
                    "capture was not bound to retained token");
            require(result.value().allocateBeforeRetire,
                    "zero-allocation reconciliation was not safe");
        }},
        Case{"one covering resource can satisfy multiple captures", [] {
            const std::array current{
                resource(4, {0, 0, 200, 100}),
            };
            const std::array desired{
                capture({0, 0, 100, 100}),
                capture({100, 0, 100, 100}),
            };
            const auto result = planCaptureResources(
                current,
                desired,
                80'000);
            require(result.hasValue(), "shared resource plan failed");
            require(result.value().retain.size() == 1U,
                    "shared covering resource was not retained");
            require(result.value().allocate.empty(),
                    "shared resource captures were reallocated");
            require(result.value().bindings[0].retainedToken == 4U &&
                        result.value().bindings[1].retainedToken == 4U,
                    "shared resource was not bound to both captures");
            require(result.value().totalBytes == 80'000U,
                    "shared resource accounting changed");
        }},
        Case{"smallest compatible retained set is preferred", [] {
            const std::array current{
                resource(8, {0, 0, 300, 300}),
                resource(7, {0, 0, 100, 100}),
            };
            const std::array desired{
                capture({10, 10, 20, 20}),
            };
            const auto result = planCaptureResources(
                current,
                desired,
                1'000'000);
            require(result.hasValue(), "covering selection failed");
            require(result.value().retain.size() == 1U &&
                        result.value().retain.front().token == 7U,
                    "smallest covering resource was not retained");
            require(result.value().retire.size() == 1U &&
                        result.value().retire.front().token == 8U,
                    "larger redundant resource was not retired");
        }},
        Case{"budget can force exact allocation over oversized reuse", [] {
            const std::array current{
                resource(2, {0, 0, 200, 200}),
            };
            const std::array desired{
                capture({10, 10, 20, 20}),
            };
            const auto result = planCaptureResources(
                current,
                desired,
                desired.front().byteCount);
            require(result.hasValue(), "bounded replacement failed");
            require(result.value().retain.empty(),
                    "oversized resource bypassed total budget");
            require(result.value().allocate ==
                        std::vector{desired.front()},
                    "exact bounded allocation was not planned");
            require(result.value().retire ==
                        std::vector{current.front()},
                    "oversized resource was not retired");
            require(!result.value().allocateBeforeRetire,
                    "peak memory budget was bypassed");
        }},
        Case{"stale generations and incompatible keys never reuse", [] {
            const std::array current{
                resource(1, {0, 0, 100, 100}, 2),
                resource(
                    2,
                    {0, 0, 100, 100},
                    1,
                    RenderStage::PreWindow),
                resource(
                    3,
                    {0, 0, 100, 100},
                    1,
                    RenderStage::PostWindows,
                    0x30334241U),
                resource(
                    4,
                    {0, 0, 100, 100},
                    1,
                    RenderStage::PostWindows,
                    0x34325241U,
                    8),
            };
            const std::array desired{
                capture({0, 0, 100, 100}),
            };
            const auto result = planCaptureResources(
                current,
                desired,
                1'000'000);
            require(result.hasValue(), "incompatible plan failed");
            require(result.value().retain.empty(),
                    "incompatible resource was retained");
            require(result.value().allocate.size() == 1U,
                    "incompatible resource suppressed allocation");
            require(result.value().retire.size() == current.size(),
                    "incompatible resources were not retired");
        }},
        Case{"new and unused resources separate cleanly", [] {
            const std::array current{
                resource(5, {0, 0, 20, 20}),
            };
            auto newCapture = capture({50, 50, 30, 30});
            newCapture.key.output = "HDMI-A-1";
            const std::array desired{newCapture};
            const auto result = planCaptureResources(
                current,
                desired,
                1'000'000);
            require(result.hasValue(), "replacement plan failed");
            require(result.value().retain.empty(),
                    "unused resource was retained");
            require(result.value().retire ==
                        std::vector{current.front()},
                    "unused resource was not retired");
            require(result.value().allocate ==
                        std::vector{desired.front()},
                    "new capture was not allocated");
            require(result.value().bindings[0].allocationIndex == 0U &&
                        !result.value().bindings[0].retainedToken,
                    "new capture binding is malformed");
        }},
        Case{"allocation precedes retirement only within peak budget", [] {
            const std::array current{
                resource(5, {0, 0, 20, 20}),
            };
            auto next = capture({50, 50, 20, 20});
            next.key.output = "HDMI-A-1";
            const std::array desired{next};

            const auto roomy = planCaptureResources(
                current,
                desired,
                current.front().plan.byteCount +
                    desired.front().byteCount);
            require(roomy.hasValue() &&
                        roomy.value().allocateBeforeRetire,
                    "safe make-before-break was not selected");

            const auto tight = planCaptureResources(
                current,
                desired,
                desired.front().byteCount);
            require(tight.hasValue() &&
                        !tight.value().allocateBeforeRetire,
                    "unsafe make-before-break was selected");
        }},
        Case{"empty desired scene retires every resource", [] {
            const std::array current{
                resource(1, {0, 0, 20, 20}),
                resource(2, {30, 30, 20, 20}),
            };
            const auto result = planCaptureResources(
                current,
                {},
                1'000'000);
            require(result.hasValue(), "empty plan failed");
            require(result.value().retain.empty() &&
                        result.value().allocate.empty() &&
                        result.value().bindings.empty(),
                    "empty scene kept active work");
            require(result.value().retire ==
                        std::vector(
                            current.begin(),
                            current.end()),
                    "empty scene did not retire all resources");
            require(result.value().totalBytes == 0U,
                    "empty scene retained memory accounting");
        }},
        Case{"desired baseline must fit the total budget", [] {
            const std::array desired{
                capture({0, 0, 100, 100}),
                capture({200, 0, 100, 100}),
            };
            const auto result = planCaptureResources(
                {},
                desired,
                desired.front().byteCount);
            require(!result, "over-budget desired scene was accepted");
            require(result.error().code ==
                        ErrorCode::ResourceLimited &&
                        result.error().path == "desired",
                    "wrong total-budget failure");
        }},
        Case{"malformed inputs and zero budget fail closed", [] {
            const std::array desired{
                capture({0, 0, 20, 20}),
            };
            require(!planCaptureResources({}, desired, 0),
                    "zero total budget was accepted");

            auto badCurrent =
                resource(0, {0, 0, 20, 20});
            const std::array current{badCurrent};
            require(!planCaptureResources(
                        current,
                        desired,
                        1'000'000),
                    "zero resource token was accepted");

            auto malformed = desired.front();
            malformed.byteCount = 1;
            const std::array malformedDesired{malformed};
            require(!planCaptureResources(
                        {},
                        malformedDesired,
                        1'000'000),
                    "malformed desired plan was accepted");
        }},
        Case{"resource tokens and desired plans must be unique", [] {
            const std::array duplicateTokens{
                resource(3, {0, 0, 20, 20}),
                resource(3, {30, 30, 20, 20}),
            };
            require(!planCaptureResources(
                        duplicateTokens,
                        {},
                        1'000'000),
                    "duplicate resource token was accepted");

            const auto duplicate =
                capture({0, 0, 20, 20});
            const std::array duplicateDesired{
                duplicate,
                duplicate,
            };
            require(!planCaptureResources(
                        {},
                        duplicateDesired,
                        1'000'000),
                    "duplicate desired plan was accepted");
        }},
        Case{"equal candidates resolve deterministically by token", [] {
            const std::array current{
                resource(10, {0, 0, 100, 100}),
                resource(9, {0, 0, 100, 100}),
            };
            const std::array desired{
                capture({10, 10, 20, 20}),
            };
            const auto result = planCaptureResources(
                current,
                desired,
                1'000'000);
            require(result.hasValue(), "tie-break plan failed");
            require(result.value().retain.size() == 1U &&
                        result.value().retain.front().token == 9U,
                    "candidate token tie-break changed");
        }},
    });
}
