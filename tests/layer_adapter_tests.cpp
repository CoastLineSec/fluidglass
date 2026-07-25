#include "TestHarness.hpp"

#include "v2/targets/LayerAdapter.hpp"

#include <array>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

Target target(std::optional<Rect> geometry = std::nullopt) {
    return {
        .id = "bar",
        .kind = TargetKind::Layer,
        .material = MaterialReference{
            .source = MaterialSource::Session,
            .name = "fluid",
        },
        .shape = RoundedRectShape{.radius = 20.0},
        .selector = LayerSelector{.namespaceName = "example:bar:primary"},
        .geometry = geometry,
        .stage = std::nullopt,
        .transition = std::nullopt,
        .enabled = true,
    };
}

TargetIdentity identity() {
    return {"client:example:session", "bar"};
}

LayerSurfaceSnapshot surface(
    LayerLevel level = LayerLevel::Top) {
    return {
        .namespaceName = "example:bar:primary",
        .objectToken = 55,
        .output = "DP-1",
        .globalGeometry = Rect{
            .x = -1920.0,
            .y = 100.0,
            .width = 1920.0,
            .height = 48.0,
        },
        .level = level,
        .opacity = 0.75,
        .mapped = true,
        .fadingOut = false,
        .readyToDelete = false,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"exact mapped namespace resolves coherent layer geometry", [] {
            const std::array surfaces{surface()};
            const auto result = resolveLayerAttachment(
                identity(),
                target(),
                surfaces);
            require(result.hasValue() && result.value(), "layer did not resolve");
            require(
                result.value()->globalGeometry ==
                    Rect{.x = -1920.0, .y = 100.0, .width = 1920.0, .height = 48.0},
                "layer geometry changed");
            require(result.value()->objectToken == 55U, "layer object identity changed");
            require(result.value()->outputFilter == "DP-1", "layer output changed");
            require(result.value()->opacity == 0.75, "layer fade opacity changed");
            require(result.value()->stage == RenderStage::PostWindows, "top layer stage changed");
        }},
        Case{"background and bottom resolve before windows", [] {
            for (const auto level : {LayerLevel::Background, LayerLevel::Bottom}) {
                const std::array surfaces{surface(level)};
                const auto result = resolveLayerAttachment(
                    identity(),
                    target(),
                    surfaces);
                require(result.hasValue() && result.value(), "lower layer did not resolve");
                require(
                    result.value()->stage == RenderStage::PostWallpaper,
                    "lower layer stage changed");
            }
        }},
        Case{"surface-local geometry is clipped to the live layer", [] {
            const std::array surfaces{surface()};
            const auto result = resolveLayerAttachment(
                identity(),
                target(Rect{.x = -10.0, .y = 8.0, .width = 100.0, .height = 80.0}),
                surfaces);
            require(result.hasValue() && result.value(), "layer subregion did not resolve");
            require(
                result.value()->globalGeometry ==
                    Rect{.x = -1920.0, .y = 108.0, .width = 90.0, .height = 40.0},
                "layer subregion clipping changed");
        }},
        Case{"fully clipped local geometry has no attachment", [] {
            const std::array surfaces{surface()};
            const auto result = resolveLayerAttachment(
                identity(),
                target(Rect{.x = 2000.0, .y = 0.0, .width = 100.0, .height = 20.0}),
                surfaces);
            require(result.hasValue() && !result.value(), "off-surface subregion resolved");
        }},
        Case{"unmapped fading and deleted layers do not resolve", [] {
            for (int state = 0; state < 3; ++state) {
                auto unavailable = surface();
                if (state == 0)
                    unavailable.mapped = false;
                else if (state == 1)
                    unavailable.fadingOut = true;
                else
                    unavailable.readyToDelete = true;
                const std::array surfaces{unavailable};
                const auto result = resolveLayerAttachment(
                    identity(),
                    target(),
                    surfaces);
                require(!result, "unavailable layer surface resolved");
                require(result.error().code == ErrorCode::UnresolvedTarget, "wrong unavailable-layer code");
            }
        }},
        Case{"duplicate live namespace is ambiguous", [] {
            auto second = surface();
            second.objectToken = 56;
            second.output = "DP-2";
            const std::array surfaces{surface(), second};
            const auto result = resolveLayerAttachment(
                identity(),
                target(),
                surfaces);
            require(!result, "duplicate namespace resolved by heuristic");
            require(result.error().code == ErrorCode::UnresolvedTarget, "wrong ambiguous-layer code");
        }},
        Case{"unrelated namespaces are ignored", [] {
            auto unrelated = surface();
            unrelated.namespaceName = "";
            const std::array surfaces{unrelated, surface()};
            const auto result = resolveLayerAttachment(
                identity(),
                target(),
                surfaces);
            require(result.hasValue() && result.value(), "exact namespace was not selected");
            require(result.value()->objectToken == 55U, "unrelated namespace changed selection");
        }},
        Case{"disabled target has no attachment", [] {
            auto disabled = target();
            disabled.enabled = false;
            const std::array surfaces{surface()};
            const auto result = resolveLayerAttachment(
                identity(),
                disabled,
                surfaces);
            require(result.hasValue() && !result.value(), "disabled layer resolved");
        }},
        Case{"malformed surface data fails closed", [] {
            auto malformed = surface();
            malformed.opacity = 2.0;
            const std::array surfaces{malformed};
            const auto result = resolveLayerAttachment(
                identity(),
                target(),
                surfaces);
            require(!result, "invalid layer opacity was accepted");
            require(result.error().path == "surface.opacity", "wrong malformed-layer path");
        }},
    });
}
