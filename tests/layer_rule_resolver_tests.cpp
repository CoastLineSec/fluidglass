#include "TestHarness.hpp"

#include "v2/targets/LayerRuleResolver.hpp"

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
        .materials = {
            {"fluid", MaterialInput{}},
            {"clear", MaterialInput{}},
        },
        .windowRules = {},
        .layerRules = {
            LayerRuleInput{
                .id = "shell-primary",
                .namespaceMatch = MatchInput{
                    .mode = MatchMode::Exact,
                    .value = "example:bar:primary",
                },
                .material = "fluid",
                .enabled = true,
            },
            LayerRuleInput{
                .id = "shell",
                .namespaceMatch = MatchInput{
                    .mode = MatchMode::Regex,
                    .value = "^example:",
                },
                .material = "clear",
                .enabled = true,
            },
        },
    });
    require(validated.hasValue(), "test config was invalid");
    return std::move(validated.value());
}

LayerSurfaceSnapshot layer(
    std::uint64_t token = 51,
    std::string namespaceName = "example:bar:primary") {
    return {
        .namespaceName = std::move(namespaceName),
        .objectToken = token,
        .output = "DP-1",
        .globalGeometry = Rect{
            .x = 0.0,
            .y = 0.0,
            .width = 1920.0,
            .height = 48.0,
        },
        .level = LayerLevel::Top,
        .opacity = 0.85,
        .mapped = true,
        .fadingOut = false,
        .readyToDelete = false,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"first matching rule creates one config-owned layer target", [] {
            const std::array surfaces{layer()};
            const auto result =
                resolveLayerRules(config(), surfaces);
            require(result.hasValue() && result.value().resolved.size() == 1U, "layer rule did not resolve");
            const auto& target = result.value().resolved.front();
            require(target.attachment.identity.owner == "config", "wrong durable owner");
            require(target.definition.id == "layer.shell-primary.51", "unstable durable target id");
            require(target.definition.material == MaterialReference{
                .source = MaterialSource::Config,
                .name = "fluid",
            }, "first-match layer material changed");
            require(target.attachment.stage == RenderStage::PostWindows, "layer stage changed");
            require(target.attachment.outputFilter == "DP-1", "layer output changed");
            require(target.attachment.opacity == 0.85, "layer opacity changed");
        }},
        Case{"regex rule handles other matching namespaces", [] {
            const std::array surfaces{
                layer(52, "example:dock:primary"),
            };
            const auto result =
                resolveLayerRules(config(), surfaces);
            require(result.hasValue() && result.value().resolved.size() == 1U, "regex layer rule did not resolve");
            require(result.value().resolved.front().definition.id == "layer.shell.52", "regex rule id changed");
            require(result.value().resolved.front().definition.material.name == "clear", "regex material changed");
        }},
        Case{"unmatched and unavailable layers are omitted", [] {
            const std::array unrelated{
                layer(52, "other:bar"),
            };
            const auto noMatch =
                resolveLayerRules(config(), unrelated);
            require(noMatch.hasValue() && noMatch.value().resolved.empty(), "unmatched layer resolved");

            for (int state = 0; state < 3; ++state) {
                auto unavailable = layer();
                if (state == 0)
                    unavailable.mapped = false;
                else if (state == 1)
                    unavailable.fadingOut = true;
                else
                    unavailable.readyToDelete = true;
                const std::array surfaces{unavailable};
                const auto result =
                    resolveLayerRules(config(), surfaces);
                require(result.hasValue() && result.value().resolved.empty(), "unavailable layer resolved");
            }
        }},
        Case{"disabled durable config creates no layer targets", [] {
            auto disabled = config();
            disabled.enabled = false;
            const std::array surfaces{layer()};
            const auto result =
                resolveLayerRules(disabled, surfaces);
            require(result.hasValue() && result.value().resolved.empty(), "disabled layer config resolved");
        }},
        Case{"a malformed matched layer costs its target alone", [] {
            // One surface the adapter cannot place files a failure for that
            // target; it must not fail the resolve and strip glass from
            // every other surface.
            auto malformed = layer();
            malformed.opacity = 2.0;
            auto healthy = layer();
            healthy.namespaceName = "example:bar:secondary";
            healthy.objectToken = healthy.objectToken + 1;
            const std::array surfaces{malformed, healthy};
            const auto result =
                resolveLayerRules(config(), surfaces);
            require(result.hasValue(),
                    "a malformed surface failed the whole resolve");
            require(result.value().resolved.size() == 1U,
                    "the healthy surface was lost");
            require(result.value().failures.size() == 1U &&
                        result.value().failures[0].error.path ==
                            "surface.opacity",
                    "the malformed surface did not file a failure");
        }},
    });
}
