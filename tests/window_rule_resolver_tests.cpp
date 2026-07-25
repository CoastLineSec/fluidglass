#include "TestHarness.hpp"

#include "v2/targets/WindowRuleResolver.hpp"

#include <array>
#include <limits>

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
        .windowRules = {
            WindowRuleInput{
                .id = "files",
                .initialClass = MatchInput{
                    .mode = MatchMode::Exact,
                    .value = "org.gnome.Nautilus",
                },
                .currentClass = std::nullopt,
                .initialTitle = std::nullopt,
                .currentTitle = std::nullopt,
                .material = "fluid",
                .enabled = true,
            },
            WindowRuleInput{
                .id = "gnome",
                .initialClass = MatchInput{
                    .mode = MatchMode::Regex,
                    .value = "^org\\.gnome\\.",
                },
                .currentClass = std::nullopt,
                .initialTitle = std::nullopt,
                .currentTitle = std::nullopt,
                .material = "clear",
                .enabled = true,
            },
        },
        .layerRules = {},
    });
    require(validated.hasValue(), "test config was invalid");
    return std::move(validated.value());
}

WindowSnapshot window(
    std::uint64_t token = 91,
    std::string initialClass = "org.gnome.Nautilus") {
    return {
        .address = "0xabc123",
        .objectToken = token,
        .pid = 7301,
        .initialClass = std::move(initialClass),
        .currentClass = "org.gnome.Nautilus",
        .initialTitle = "Files",
        .currentTitle = "Home",
        .globalGeometry = Rect{
            .x = 100.0,
            .y = 80.0,
            .width = 900.0,
            .height = 700.0,
        },
        .rounding = 14.0,
        .roundingPower = 2.0,
        .opacity = 0.9,
        .mapped = true,
        .fadingOut = false,
        .readyToDelete = false,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"first matching rule creates one config-owned target", [] {
            const std::array windows{window()};
            const auto result =
                resolveWindowRules(config(), windows);
            require(result.hasValue(), "window rules did not resolve");
            require(result.value().size() == 1U, "wrong resolved rule count");
            const auto& target = result.value().front();
            require(target.attachment.identity.owner == "config", "wrong durable owner");
            require(target.attachment.identity.targetId == "files.91", "unstable durable target id");
            require(target.definition.material == MaterialReference{
                .source = MaterialSource::Config,
                .name = "fluid",
            }, "first-match material precedence changed");
            require(target.attachment.objectToken == 91U, "window token changed");
            require(target.attachment.stage == RenderStage::PreWindow, "window stage changed");
            require(std::get<RoundedRectShape>(target.definition.shape).radius == 14.0, "window rounding changed");
            require(target.roundingPower == 2.0, "window rounding power changed");
        }},
        Case{"one rule can create stable targets for multiple windows", [] {
            auto second = window(92);
            second.address = "0xabc124";
            second.pid += 1;
            const std::array windows{window(), second};
            const auto result =
                resolveWindowRules(config(), windows);
            require(result.hasValue() && result.value().size() == 2U, "multiple windows did not resolve");
            require(result.value()[0].definition.id == "files.91", "first target id changed");
            require(result.value()[1].definition.id == "files.92", "second target id changed");
        }},
        Case{"unmatched and unavailable windows are omitted", [] {
            auto unmatched = window();
            unmatched.initialClass = "org.example.Editor";
            unmatched.currentClass = "org.example.Editor";
            const std::array unmatchedWindows{unmatched};
            const auto noMatch =
                resolveWindowRules(config(), unmatchedWindows);
            require(noMatch.hasValue() && noMatch.value().empty(), "unmatched window resolved");

            for (int state = 0; state < 3; ++state) {
                auto unavailable = window();
                if (state == 0)
                    unavailable.mapped = false;
                else if (state == 1)
                    unavailable.fadingOut = true;
                else
                    unavailable.readyToDelete = true;
                const std::array unavailableWindows{unavailable};
                const auto result =
                    resolveWindowRules(config(), unavailableWindows);
                require(result.hasValue() && result.value().empty(), "unavailable window resolved");
            }
        }},
        Case{"disabled durable config creates no targets", [] {
            auto disabled = config();
            disabled.enabled = false;
            const std::array windows{window()};
            const auto result =
                resolveWindowRules(disabled, windows);
            require(result.hasValue() && result.value().empty(), "disabled config resolved");
        }},
        Case{"pid alone can guard a matched window", [] {
            auto pidOnly = window();
            pidOnly.initialClass.clear();
            pidOnly.currentClass = "org.gnome.Nautilus";
            auto currentClassConfig = config();
            currentClassConfig.windowRules.front().initialClass.reset();
            currentClassConfig.windowRules.front().currentClass =
                MatchExpression{
                    .mode = MatchMode::Exact,
                    .value = "org.gnome.Nautilus",
                    .compiled = nullptr,
                };
            const std::array windows{pidOnly};
            const auto result =
                resolveWindowRules(currentClassConfig, windows);
            require(result.hasValue() && result.value().size() == 1U, "pid-only window did not resolve");
            const auto& selector =
                std::get<WindowSelector>(result.value().front().definition.selector);
            require(selector.pid == 7301, "pid guard was not retained");
            require(!selector.initialClass, "empty initial class became a guard");
        }},
        Case{"matched malformed window fails closed", [] {
            auto malformed = window();
            malformed.rounding =
                std::numeric_limits<double>::quiet_NaN();
            const std::array windows{malformed};
            const auto result =
                resolveWindowRules(config(), windows);
            require(!result, "malformed matched window resolved");
            require(result.error().path == "windows[0].rounding", "wrong malformed-window path");

            malformed = window();
            malformed.roundingPower =
                std::numeric_limits<double>::infinity();
            const std::array invalidPower{malformed};
            const auto powerResult =
                resolveWindowRules(config(), invalidPower);
            require(!powerResult, "invalid rounding power resolved");
            require(
                powerResult.error().path == "windows[0].rounding_power",
                "wrong rounding-power path");
        }},
    });
}
