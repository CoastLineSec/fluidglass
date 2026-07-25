#include "TestHarness.hpp"

#include "v2/targets/TargetScene.hpp"

#include <algorithm>
#include <array>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

ConfigSnapshot config() {
    auto validated = validateConfig({
        .version = 2,
        .enabled = true,
        .defaultMaterial = "fluid",
        .materials = {{"fluid", MaterialInput{}}},
        .windowRules = {
            WindowRuleInput{
                .id = "app",
                .initialClass = MatchInput{
                    .mode = MatchMode::Exact,
                    .value = "org.example.App",
                },
                .currentClass = std::nullopt,
                .initialTitle = std::nullopt,
                .currentTitle = std::nullopt,
                .material = "fluid",
                .enabled = true,
            },
        },
        .layerRules = {
            LayerRuleInput{
                .id = "app",
                .namespaceMatch = MatchInput{
                    .mode = MatchMode::Exact,
                    .value = "example:bar",
                },
                .material = "fluid",
                .enabled = true,
            },
        },
    });
    require(validated.hasValue(), "test config was invalid");
    return std::move(validated.value());
}

WindowSnapshot window() {
    return {
        .address = "0xabc",
        .objectToken = 1,
        .pid = 42,
        .initialClass = "org.example.App",
        .currentClass = "org.example.App",
        .initialTitle = "Example",
        .currentTitle = "Example",
        .globalGeometry = {
            .x = 100.0,
            .y = 100.0,
            .width = 800.0,
            .height = 600.0,
        },
        .rounding = 12.0,
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
        .objectToken = 1,
        .output = "DP-1",
        .globalGeometry = {
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

SessionSnapshot clientOverride() {
    auto material = validateMaterial(
        "override",
        MaterialInput{});
    require(material.hasValue(), "test material was invalid");
    Target target{
        .id = "window",
        .kind = TargetKind::Window,
        .material = {
            .source = MaterialSource::Session,
            .name = "override",
        },
        .shape = RoundedRectShape{.radius = 20.0},
        .selector = WindowSelector{
            .address = "0xabc",
            .pid = 42,
            .initialClass = std::nullopt,
        },
        .geometry = std::nullopt,
        .stage = std::nullopt,
        .transition = std::nullopt,
        .enabled = true,
    };
    return {
        .owner = "client:example:s1",
        .clientId = "example",
        .mode = SessionMode::Client,
        .generation = 1,
        .expiresAtMs = 1000,
        .materials = {
            {"override", std::move(material.value())},
        },
        .targets = {std::move(target)},
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"durable window and layer identities remain distinct", [] {
            const auto active = config();
            const std::array windows{window()};
            const std::array layers{layer()};
            const auto result = buildTargetScene(
                &active,
                std::span<const SessionSnapshot>{},
                windows,
                layers,
                std::span<const OutputGeneration>{});
            require(result.hasValue(), "durable scene failed");
            require(result.value().effective.size() == 2U, "durable targets collided");
            require(result.value().effective[0].attachment.identity.targetId !=
                    result.value().effective[1].attachment.identity.targetId,
                    "cross-kind durable identities collided");
        }},
        Case{"leased target overrides only its exact durable attachment", [] {
            const auto active = config();
            const std::array sessions{clientOverride()};
            const std::array windows{window()};
            const std::array layers{layer()};
            const auto result = buildTargetScene(
                &active,
                sessions,
                windows,
                layers,
                std::span<const OutputGeneration>{});
            require(result.hasValue(), "combined scene failed");
            require(result.value().effective.size() == 2U, "unrelated durable layer was lost");
            require(result.value().suppressed.size() == 1U, "durable window was not suppressed");
            require(result.value().suppressed.front().targetId == "window.app.1", "wrong durable target suppressed");
            require(std::ranges::any_of(
                result.value().effective,
                [](const ResolvedTarget& target) {
                    return target.attachment.identity.owner ==
                        "client:example:s1";
                }), "client override was not effective");
        }},
        Case{"unresolved leased target is retained beside durable scene", [] {
            auto missing = clientOverride();
            std::get<WindowSelector>(
                missing.targets.front().selector).address =
                    "0xdef";
            const auto active = config();
            const std::array sessions{missing};
            const std::array windows{window()};
            const std::array layers{layer()};
            const auto result = buildTargetScene(
                &active,
                sessions,
                windows,
                layers,
                std::span<const OutputGeneration>{});
            require(result.hasValue(), "partial scene failed globally");
            require(result.value().effective.size() == 2U, "durable scene was discarded");
            require(result.value().failures.size() == 1U, "leased failure was lost");
            require(result.value().failures.front().identity.targetId == "window", "wrong failure retained");
        }},
        Case{"null config still resolves leased targets", [] {
            const std::array sessions{clientOverride()};
            const std::array windows{window()};
            const auto result = buildTargetScene(
                nullptr,
                sessions,
                windows,
                std::span<const LayerSurfaceSnapshot>{},
                std::span<const OutputGeneration>{});
            require(result.hasValue(), "config-free scene failed");
            require(result.value().effective.size() == 1U, "leased target was lost without config");
        }},
    });
}
