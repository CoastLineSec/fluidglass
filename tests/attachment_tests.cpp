#include "TestHarness.hpp"

#include "v2/targets/Attachment.hpp"

#include <array>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

OutputGeneration output(
    std::string name,
    double logicalX,
    std::uint64_t generation = 1) {
    return {
        .snapshot = OutputSnapshot{
            .name = std::move(name),
            .objectToken = generation,
            .modeToken = 1,
            .bufferWidth = 1920,
            .bufferHeight = 1080,
            .logicalX = logicalX,
            .logicalY = 0.0,
            .logicalWidth = 1920.0,
            .logicalHeight = 1080.0,
            .scale = 1.0,
            .transform = OutputTransform::Normal,
            .renderFormat = 0x34325241U,
            .colorStateToken = 7,
        },
        .generation = generation,
    };
}

ResolvedAttachment attachment(Rect geometry) {
    return {
        .identity = TargetIdentity{"client:test:session", "glass"},
        .kind = TargetKind::Window,
        .objectToken = 44,
        .globalGeometry = geometry,
        .stage = RenderStage::PreWindow,
        .outputFilter = std::nullopt,
        .opacity = 1.0,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"spanning attachment creates independent presentations", [] {
            const std::array outputs{
                output("DP-1", 0.0, 3),
                output("DP-2", 1920.0, 8),
            };
            const auto result = resolvePresentations(
                attachment(Rect{.x = 1900.0, .y = 100.0, .width = 100.0, .height = 200.0}),
                outputs);
            require(result.hasValue() && result.value().size() == 2U, "spanning target did not split");
            require(result.value()[0].key.output == "DP-1", "first output identity changed");
            require(result.value()[0].key.outputGeneration == 3U, "first generation changed");
            require(
                result.value()[0].geometry.coverage == PixelRect{1900, 100, 20, 200},
                "first output clipping changed");
            require(result.value()[1].key.output == "DP-2", "second output identity changed");
            require(result.value()[1].key.outputGeneration == 8U, "second generation changed");
            require(
                result.value()[1].geometry.coverage == PixelRect{0, 100, 80, 200},
                "second output clipping changed");
        }},
        Case{"output filter restricts presentation", [] {
            const std::array outputs{
                output("DP-1", 0.0),
                output("DP-2", 1920.0),
            };
            auto filtered = attachment(
                Rect{.x = 1900.0, .y = 100.0, .width = 100.0, .height = 200.0});
            filtered.outputFilter = "DP-2";
            const auto result = resolvePresentations(filtered, outputs);
            require(result.hasValue() && result.value().size() == 1U, "output filter was ignored");
            require(result.value().front().key.output == "DP-2", "wrong filtered output resolved");
        }},
        Case{"fully clipped attachment resolves without presentation", [] {
            const std::array outputs{output("DP-1", 0.0)};
            const auto result = resolvePresentations(
                attachment(Rect{.x = 3000.0, .y = 100.0, .width = 100.0, .height = 100.0}),
                outputs);
            require(result.hasValue() && result.value().empty(), "off-output target produced a presentation");
        }},
        Case{"presentation retains attachment and readiness identity", [] {
            const std::array outputs{output("DP-1", 0.0, 6)};
            const auto source = attachment(Rect{.x = 10.0, .y = 20.0, .width = 30.0, .height = 40.0});
            const auto result = resolvePresentations(source, outputs);
            require(result.hasValue() && result.value().size() == 1U, "presentation did not resolve");
            require(result.value().front().attachmentToken == 44U, "attachment token changed");
            require(result.value().front().key.identity == source.identity, "target identity changed");
            require(result.value().front().key.stage == RenderStage::PreWindow, "render stage changed");
            require(result.value().front().opacity == 1.0, "attachment opacity changed");
        }},
        Case{"invalid attachment fails before output mapping", [] {
            const std::array outputs{output("DP-1", 0.0)};
            auto malformed = attachment(Rect{.x = 10.0, .y = 20.0, .width = 30.0, .height = 40.0});
            malformed.objectToken = 0;
            const auto result = resolvePresentations(malformed, outputs);
            require(!result, "zero attachment token was accepted");
            require(result.error().path == "attachment.object_token", "wrong attachment failure path");
        }},
        Case{"attachment opacity is validated and preserved", [] {
            const std::array outputs{output("DP-1", 0.0)};
            auto faded = attachment(Rect{.x = 10.0, .y = 20.0, .width = 30.0, .height = 40.0});
            faded.opacity = 0.35;
            const auto result = resolvePresentations(faded, outputs);
            require(result.hasValue() && result.value().size() == 1U, "faded attachment did not resolve");
            require(result.value().front().opacity == 0.35, "faded attachment opacity changed");

            faded.opacity = 1.1;
            const auto invalid = resolvePresentations(faded, outputs);
            require(!invalid, "invalid attachment opacity was accepted");
            require(invalid.error().path == "attachment.opacity", "wrong opacity failure path");
        }},
        Case{"duplicate output generation fails closed", [] {
            const std::array outputs{
                output("DP-1", 0.0, 1),
                output("DP-1", 0.0, 2),
            };
            const auto result = resolvePresentations(
                attachment(Rect{.x = 10.0, .y = 20.0, .width = 30.0, .height = 40.0}),
                outputs);
            require(!result, "two active generations for one output were accepted");
            require(result.error().path == "outputs[1]", "wrong duplicate-output failure path");
        }},
    });
}
