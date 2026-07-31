#include "TestHarness.hpp"

#include "v2/targets/TargetResolver.hpp"

#include <array>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

Target windowTarget(bool enabled = true) {
    return {
        .id = "window",
        .kind = TargetKind::Window,
        .material = {
            .source = MaterialSource::Session,
            .name = "fluid",
        },
        .shape = RoundedRectShape{.radius = 12.0},
        .selector = WindowSelector{
            .address = "0xabc",
            .pid = 42,
            .initialClass = std::nullopt,
        },
        .geometry = std::nullopt,
        .stage = std::nullopt,
        .transition = std::nullopt,
        .enabled = enabled,
    };
}

Target layerTarget() {
    return {
        .id = "layer",
        .kind = TargetKind::Layer,
        .material = {
            .source = MaterialSource::Session,
            .name = "fluid",
        },
        .shape = RoundedRectShape{.radius = 18.0},
        .selector = LayerSelector{
            .namespaceName = "example:bar",
        },
        .geometry = std::nullopt,
        .stage = std::nullopt,
        .transition = std::nullopt,
        .enabled = true,
    };
}

Target regionTarget() {
    return {
        .id = "region",
        .kind = TargetKind::Region,
        .material = {
            .source = MaterialSource::Session,
            .name = "fluid",
        },
        .shape = RoundedRectShape{.radius = 20.0},
        .selector = RegionSelector{.output = "DP-1"},
        .geometry = Rect{
            .x = 100.0,
            .y = 80.0,
            .width = 400.0,
            .height = 300.0,
        },
        .stage = RenderStage::PostWindows,
        .transition = std::nullopt,
        .enabled = true,
    };
}

SessionSnapshot session(std::vector<Target> targets) {
    return {
        .owner = "client:example:s1",
        .clientId = "example",
        .mode = SessionMode::Client,
        .generation = 1,
        .expiresAtMs = 10'000,
        .materials = {},
        .targets = std::move(targets),
    };
}

WindowSnapshot window() {
    return {
        .address = "0xabc",
        .objectToken = 11,
        .pid = 42,
        .initialClass = "org.example.App",
        .currentClass = "org.example.App",
        .initialTitle = "Example",
        .currentTitle = "Example",
        .globalGeometry = Rect{
            .x = 20.0,
            .y = 30.0,
            .width = 800.0,
            .height = 600.0,
        },
        .rounding = 10.0,
        .roundingPower = 2.0,
        .opacity = 1.0,
        .mapped = true,
        .fadingOut = false,
        .readyToDelete = false,
    };
}

LayerSurfaceSnapshot layer() {
    return {
        .namespaceName = "example:bar",
        .objectToken = 12,
        .output = "DP-1",
        .globalGeometry = Rect{
            .x = 0.0,
            .y = 0.0,
            .width = 1920.0,
            .height = 48.0,
        },
        .level = LayerLevel::Top,
        .opacity = 1.0,
        .mapped = true,
        .fadingOut = false,
        .readyToDelete = false,
    };
}

OutputGeneration output() {
    return {
        .snapshot = {
            .name = "DP-1",
            .objectToken = 13,
            .modeToken = 1,
            .bufferWidth = 1920,
            .bufferHeight = 1080,
            .logicalX = 0.0,
            .logicalY = 0.0,
            .logicalWidth = 1920.0,
            .logicalHeight = 1080.0,
            .scale = 1.0,
            .transform = OutputTransform::Normal,
            .renderFormat = 0x34325241U,
            .colorStateToken = 1,
        },
        .generation = 1,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"all runtime target kinds resolve through one batch", [] {
            const std::array sessions{
                session({
                    windowTarget(),
                    layerTarget(),
                    regionTarget(),
                }),
            };
            const std::array windows{window()};
            const std::array layers{layer()};
            const std::array outputs{output()};
            const auto result = resolveSessionTargets(
                sessions,
                windows,
                layers,
                outputs);
            require(result.hasValue(), "runtime batch failed");
            require(result.value().resolved.size() == 3U, "not all target kinds resolved");
            require(result.value().inactive.empty(), "active target became inactive");
            require(result.value().failures.empty(), "valid target failed");
            require(result.value().resolved[0].attachment.kind == TargetKind::Window, "window order changed");
            require(result.value().resolved[1].attachment.kind == TargetKind::Layer, "layer order changed");
            require(result.value().resolved[2].attachment.kind == TargetKind::Region, "region order changed");
        }},
        Case{"one unresolved target does not discard resolved siblings", [] {
            auto missing = layerTarget();
            std::get<LayerSelector>(missing.selector).namespaceName =
                "missing";
            const std::array sessions{
                session({windowTarget(), missing}),
            };
            const std::array windows{window()};
            const std::array layers{layer()};
            const auto result = resolveSessionTargets(
                sessions,
                windows,
                layers,
                std::span<const OutputGeneration>{});
            require(result.hasValue(), "partial batch failed globally");
            require(result.value().resolved.size() == 1U, "resolved sibling was discarded");
            require(result.value().failures.size() == 1U, "unresolved target was not reported");
            require(result.value().failures.front().identity.targetId == "layer", "wrong failed target");
            require(result.value().failures.front().error.code == ErrorCode::UnresolvedTarget, "wrong target failure");
        }},
        Case{"disabled target is inactive rather than failed", [] {
            const std::array sessions{
                session({windowTarget(false)}),
            };
            const auto result = resolveSessionTargets(
                sessions,
                std::span<const WindowSnapshot>{},
                std::span<const LayerSurfaceSnapshot>{},
                std::span<const OutputGeneration>{});
            require(result.hasValue(), "disabled batch failed");
            require(result.value().resolved.empty(), "disabled target resolved");
            require(result.value().failures.empty(), "disabled target failed");
            require(result.value().inactive.size() == 1U, "disabled target was not inactive");
            require(result.value().inactive.front().reason == TargetInactiveReason::Disabled,
                    "disabled target did not report why it is inactive");
        }},
        Case{"duplicate current output generations fail the region only", [] {
            auto duplicate = output();
            duplicate.generation = 2;
            const std::array outputs{output(), duplicate};
            const std::array sessions{
                session({regionTarget()}),
            };
            const auto result = resolveSessionTargets(
                sessions,
                std::span<const WindowSnapshot>{},
                std::span<const LayerSurfaceSnapshot>{},
                outputs);
            require(result.hasValue(), "ambiguous output failed globally");
            require(result.value().resolved.empty(), "ambiguous output resolved");
            require(result.value().failures.size() == 1U, "ambiguous output was not reported");
            require(result.value().failures.front().error.code == ErrorCode::StaleGeneration, "wrong output ambiguity code");
        }},
        Case{"session owner must be present", [] {
            auto malformed = session({windowTarget()});
            malformed.owner.clear();
            const std::array sessions{malformed};
            const auto result = resolveSessionTargets(
                sessions,
                std::span<const WindowSnapshot>{},
                std::span<const LayerSurfaceSnapshot>{},
                std::span<const OutputGeneration>{});
            require(!result, "empty session owner was accepted");
            require(result.error().path == "sessions.owner", "wrong owner error path");
        }},
        Case{"duplicate session owners and target ids fail closed", [] {
            const std::array duplicateOwners{
                session({windowTarget()}),
                session({layerTarget()}),
            };
            const auto ownerResult = resolveSessionTargets(
                duplicateOwners,
                std::span<const WindowSnapshot>{},
                std::span<const LayerSurfaceSnapshot>{},
                std::span<const OutputGeneration>{});
            require(!ownerResult, "duplicate session owner was accepted");
            require(ownerResult.error().path == "sessions.owner", "wrong duplicate-owner path");

            auto duplicateTarget = windowTarget();
            duplicateTarget.kind = TargetKind::Layer;
            duplicateTarget.selector = LayerSelector{
                .namespaceName = "example:bar",
            };
            const std::array duplicateIds{
                session({windowTarget(), duplicateTarget}),
            };
            const auto targetResult = resolveSessionTargets(
                duplicateIds,
                std::span<const WindowSnapshot>{},
                std::span<const LayerSurfaceSnapshot>{},
                std::span<const OutputGeneration>{});
            require(!targetResult, "duplicate target id was accepted");
            require(targetResult.error().path == "sessions.targets.id", "wrong duplicate-target path");
        }},
    });
}
