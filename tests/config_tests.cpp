#include "TestHarness.hpp"

#include "v2/model/Config.hpp"

#include <string>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

ConfigSnapshotInput validConfig() {
    return {
        .version = 2,
        .enabled = true,
        .defaultMaterial = "fluid",
        .materials = {{"fluid", MaterialInput{}}},
        .windowRules = {},
        .layerRules = {},
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"minimal configuration", [] {
            const auto result = validateConfig(validConfig());
            require(result.hasValue(), "minimal configuration must be valid");
            require(result.value().materials.contains("fluid"), "material was not normalized");
        }},
        Case{"version and default material", [] {
            auto input = validConfig();
            input.version = 1;
            require(!validateConfig(input), "wrong version must fail");
            input = validConfig();
            input.defaultMaterial = "missing";
            require(!validateConfig(input), "missing default material must fail");
        }},
        Case{"window rule matching", [] {
            auto input = validConfig();
            input.windowRules.push_back({
                .id = "files",
                .initialClass = MatchInput{.mode = MatchMode::Exact, .value = "org.gnome.Nautilus"},
                .currentClass = std::nullopt,
                .initialTitle = std::nullopt,
                .currentTitle = MatchInput{.mode = MatchMode::Regex, .value = "^Files"},
                .material = "fluid",
            });
            const auto result = validateConfig(std::move(input));
            require(result.hasValue(), "valid window rule was rejected");
            const WindowMetadata matching{
                .initialClass = "org.gnome.Nautilus",
                .currentClass = "org.gnome.Nautilus",
                .initialTitle = "Home",
                .currentTitle = "Files — Home",
            };
            require(result.value().windowRules[0].matches(matching), "matching window did not match");
            auto changed = matching;
            changed.currentTitle = "Downloads";
            require(!result.value().windowRules[0].matches(changed), "non-matching title matched");
        }},
        Case{"invalid regex rejected at validation", [] {
            auto input = validConfig();
            input.layerRules.push_back({
                .id = "shell",
                .namespaceMatch = {.mode = MatchMode::Regex, .value = "["},
                .material = "fluid",
            });
            const auto result = validateConfig(std::move(input));
            require(!result, "invalid regular expression must fail");
            require(result.error().path == "layer_rules[0].match.namespace", "regex error path changed");
        }},
        Case{"layer exact and regex matching", [] {
            auto input = validConfig();
            input.layerRules = {
                {
                    .id = "exact",
                    .namespaceMatch = {.mode = MatchMode::Exact, .value = "shell:bar"},
                    .material = "fluid",
                },
                {
                    .id = "family",
                    .namespaceMatch = {.mode = MatchMode::Regex, .value = "^shell:"},
                    .material = "fluid",
                },
            };
            const auto result = validateConfig(std::move(input));
            require(result.hasValue(), "valid layer rules were rejected");
            require(result.value().layerRules[0].matches("shell:bar"), "exact namespace did not match");
            require(!result.value().layerRules[0].matches("shell:dock"), "exact namespace overmatched");
            require(result.value().layerRules[1].matches("shell:dock"), "namespace regex did not match");
        }},
        Case{"window rule requires match", [] {
            auto input = validConfig();
            input.windowRules.push_back({
                .id = "everything",
                .initialClass = std::nullopt,
                .currentClass = std::nullopt,
                .initialTitle = std::nullopt,
                .currentTitle = std::nullopt,
                .material = "fluid",
            });
            require(!validateConfig(std::move(input)), "selector-free window rule must fail");
        }},
        Case{"rule ids and materials validated", [] {
            auto input = validConfig();
            input.layerRules = {
                {.id = "same", .namespaceMatch = {.value = "one"}, .material = "fluid"},
                {.id = "same", .namespaceMatch = {.value = "two"}, .material = "fluid"},
            };
            require(!validateConfig(input), "duplicate rule id must fail");
            input = validConfig();
            input.layerRules = {
                {.id = "missing", .namespaceMatch = {.value = "one"}, .material = "unknown"},
            };
            require(!validateConfig(input), "unknown rule material must fail");
        }},
        Case{"reload commits atomically", [] {
            ConfigStore store;
            store.beginReload();
            require(store.stage(validConfig()).hasValue(), "valid configuration did not stage");
            const auto committed = store.commitReload();
            require(committed.hasValue() && committed.value() == 1, "first generation did not commit");
            require(store.active() && store.active()->enabled, "active configuration missing");

            auto invalid = validConfig();
            invalid.defaultMaterial = "missing";
            store.beginReload();
            require(!store.stage(std::move(invalid)), "invalid configuration staged");
            require(!store.commitReload(), "invalid reload committed");
            require(store.generation() == 1, "invalid reload changed generation");
            require(store.active() && store.active()->defaultMaterial == "fluid",
                    "invalid reload replaced last known-good state");
        }},
        Case{"missing configure call preserves active state", [] {
            ConfigStore store;
            store.beginReload();
            require(store.stage(validConfig()).hasValue(), "valid configuration did not stage");
            require(store.commitReload().hasValue(), "valid configuration did not commit");
            store.beginReload();
            require(!store.commitReload(), "empty reload unexpectedly committed");
            require(store.active() && store.active()->defaultMaterial == "fluid",
                    "empty reload cleared active state");
            require(store.pendingError().has_value(), "empty reload failure was not retained");
            require(store.pendingError()->path == "config", "empty reload error path changed");

            store.beginReload();
            require(store.stage(validConfig()).hasValue(), "recovery configuration did not stage");
            require(store.commitReload().hasValue(), "recovery configuration did not commit");
            require(!store.pendingError().has_value(), "successful reload did not clear the error");
        }},
        Case{"later configure call replaces staging snapshot", [] {
            ConfigStore store;
            auto first = validConfig();
            first.enabled = false;
            auto second = validConfig();
            second.enabled = true;
            store.beginReload();
            require(store.stage(std::move(first)).hasValue(), "first snapshot failed");
            require(store.stage(std::move(second)).hasValue(), "second snapshot failed");
            require(store.commitReload().hasValue(), "staging snapshot did not commit");
            require(store.active() && store.active()->enabled, "last configure call did not win");
        }},
    });
}
