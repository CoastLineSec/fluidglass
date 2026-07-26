#include "TestHarness.hpp"

#include "v2/runtime/LiveScenePlan.hpp"

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

PlannedPresentation presentation(TargetKind kind, std::string targetId,
                                 std::string output, std::uint64_t outputToken,
                                 std::uint64_t attachmentToken) {
    PlannedPresentation planned;
    planned.target.attachment = {
        .identity = {.owner = "client:test", .targetId = targetId},
        .kind = kind,
        .objectToken = attachmentToken,
        .globalGeometry = {},
        .stage = RenderStage::PostWindows,
        .outputFilter = std::nullopt,
        .opacity = 1.0,
    };
    planned.presentation.key = {
        .identity = planned.target.attachment.identity,
        .output = output,
        .outputGeneration = 1,
        .stage = kind == TargetKind::Window ? RenderStage::PreWindow
                                            : RenderStage::PostWindows,
    };
    planned.output.snapshot.name = std::move(output);
    planned.output.snapshot.objectToken = outputToken;
    planned.output.generation = 1;
    return planned;
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"bindings deduplicate multi-output window presentations", [] {
            const std::vector presentations{
                presentation(TargetKind::Window, "window", "DP-1", 10, 30),
                presentation(TargetKind::Window, "window", "HDMI-A-1", 11, 30),
            };
            const auto result = planLiveSceneBindings(presentations);
            require(result.hasValue(), "binding plan failed");
            require(result.value().windowAttachments.size() == 1,
                    "window attachment was not deduplicated");
            require(result.value().directScanoutLeases.size() == 2,
                    "output leases were not preserved");
        }},
        Case{"non-window presentations do not create decorations", [] {
            const std::vector presentations{
                presentation(TargetKind::Layer, "bar", "DP-1", 10, 40),
                presentation(TargetKind::Region, "region", "DP-1", 10, 0),
            };
            const auto result = planLiveSceneBindings(presentations);
            require(result.hasValue(), "binding plan failed");
            require(result.value().windowAttachments.empty(),
                    "non-window target created a window decoration");
            require(result.value().directScanoutLeases.size() == 1,
                    "shared output lease was not deduplicated");
        }},
        Case{"conflicting identities and output objects fail closed", [] {
            auto first =
                presentation(TargetKind::Window, "window", "DP-1", 10, 30);
            auto conflictingWindow =
                presentation(TargetKind::Window, "window", "HDMI-A-1", 11, 31);
            require(!planLiveSceneBindings(
                        std::vector{first, conflictingWindow}),
                    "one identity attached to two windows");
            auto conflictingOutput =
                presentation(TargetKind::Region, "other", "DP-1", 12, 0);
            require(!planLiveSceneBindings(
                        std::vector{first, conflictingOutput}),
                    "one output name accepted two compositor objects");
        }},
        Case{"incomplete output and window identities fail closed", [] {
            auto missingOutput =
                presentation(TargetKind::Region, "region", "", 10, 0);
            require(!planLiveSceneBindings(std::vector{missingOutput}),
                    "empty output name was accepted");
            auto missingWindow =
                presentation(TargetKind::Window, "window", "DP-1", 10, 0);
            require(!planLiveSceneBindings(std::vector{missingWindow}),
                    "zero window token was accepted");
        }},
    });
}
