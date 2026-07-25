#include "TestHarness.hpp"

#include "v2/render/CaptureCache.hpp"

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

CapturePlan plan(
    PixelRect region,
    std::uint64_t generation = 1,
    RenderStage stage = RenderStage::PostWindows,
    std::uint32_t format = 0x34325241U,
    std::uint64_t colorState = 7) {
    const auto pixels = static_cast<std::uint64_t>(region.width) *
        static_cast<std::uint64_t>(region.height);
    return {
        .key = CaptureKey{
            .output = "DP-1",
            .outputGeneration = generation,
            .stage = stage,
            .renderFormat = format,
            .colorStateToken = colorState,
            .stageObjectToken =
                stage == RenderStage::PreWindow ? 99U : 0U,
        },
        .region = region,
        .bytesPerPixel = 4,
        .pixelCount = pixels,
        .byteCount = pixels * 4U,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"covering lookup chooses the smallest exact-compatible resource", [] {
            CaptureResourceIndex index;
            require(index.add({1, plan(PixelRect{0, 0, 400, 400})}).hasValue(), "large resource add failed");
            require(index.add({2, plan(PixelRect{50, 50, 100, 100})}).hasValue(), "small resource add failed");
            const auto found = index.findCovering(plan(PixelRect{60, 60, 20, 20}));
            require(found && found->token == 2U, "lookup did not select the smallest covering capture");
        }},
        Case{"lookup never crosses generation stage format or color", [] {
            CaptureResourceIndex index;
            require(index.add({1, plan(PixelRect{0, 0, 100, 100})}).hasValue(), "resource add failed");
            require(!index.findCovering(plan(PixelRect{10, 10, 10, 10}, 2)), "stale generation was reused");
            require(
                !index.findCovering(plan(PixelRect{10, 10, 10, 10}, 1, RenderStage::PreWindow)),
                "wrong render stage was reused");
            require(
                !index.findCovering(plan(PixelRect{10, 10, 10, 10}, 1, RenderStage::PostWindows, 0x30334241U)),
                "wrong format was reused");
            require(
                !index.findCovering(plan(PixelRect{10, 10, 10, 10}, 1, RenderStage::PostWindows, 0x34325241U, 8)),
                "wrong color state was reused");
        }},
        Case{"uncontained resource is not reused", [] {
            CaptureResourceIndex index;
            require(index.add({1, plan(PixelRect{10, 10, 20, 20})}).hasValue(), "resource add failed");
            require(!index.findCovering(plan(PixelRect{0, 10, 20, 20})), "uncontained resource was reused");
        }},
        Case{"resource tokens are unique and removable", [] {
            CaptureResourceIndex index;
            require(index.add({5, plan(PixelRect{0, 0, 20, 20})}).hasValue(), "resource add failed");
            require(!index.add({5, plan(PixelRect{30, 30, 20, 20})}), "duplicate token was accepted");
            const auto removed = index.remove(5);
            require(removed && removed->token == 5U, "resource removal failed");
            require(index.size() == 0U, "removed resource remained indexed");
            require(!index.remove(5), "resource was removed twice");
        }},
        Case{"generation retirement is exact", [] {
            CaptureResourceIndex index;
            require(index.add({1, plan(PixelRect{0, 0, 20, 20}, 1)}).hasValue(), "generation one add failed");
            require(index.add({2, plan(PixelRect{0, 0, 20, 20}, 2)}).hasValue(), "generation two add failed");
            auto otherOutput = plan(PixelRect{0, 0, 20, 20}, 1);
            otherOutput.key.output = "HDMI-A-1";
            require(index.add({3, otherOutput}).hasValue(), "other output add failed");

            const auto retired = index.retireGeneration("DP-1", 1);
            require(retired.size() == 1U && retired.front().token == 1U, "wrong generation retired");
            require(index.size() == 2U, "unrelated resources were retired");
        }},
        Case{"output retirement and clear return ownership", [] {
            CaptureResourceIndex index;
            require(index.add({1, plan(PixelRect{0, 0, 20, 20}, 1)}).hasValue(), "first add failed");
            require(index.add({2, plan(PixelRect{0, 0, 20, 20}, 2)}).hasValue(), "second add failed");
            auto otherOutput = plan(PixelRect{0, 0, 20, 20}, 1);
            otherOutput.key.output = "HDMI-A-1";
            require(index.add({3, otherOutput}).hasValue(), "other output add failed");

            const auto outputResources = index.retireOutput("DP-1");
            require(outputResources.size() == 2U, "output retirement did not return all generations");
            require(index.size() == 1U, "output retirement removed another output");
            const auto remaining = index.clear();
            require(remaining.size() == 1U && remaining.front().token == 3U, "clear lost resource ownership");
            require(index.size() == 0U, "clear left resources indexed");
        }},
        Case{"malformed resources fail closed", [] {
            CaptureResourceIndex index;
            require(!index.add({0, plan(PixelRect{0, 0, 20, 20})}), "zero token was accepted");
            auto malformed = plan(PixelRect{0, 0, 20, 20});
            malformed.byteCount = 1;
            require(!index.add({1, malformed}), "inconsistent allocation accounting was accepted");
            require(index.size() == 0U, "malformed resource was indexed");
        }},
    });
}
