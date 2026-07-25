#include "TestHarness.hpp"

#include "v2/targets/WindowAdapter.hpp"

#include <array>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

Target target() {
    return {
        .id = "files",
        .kind = TargetKind::Window,
        .material = MaterialReference{
            .source = MaterialSource::Session,
            .name = "fluid",
        },
        .shape = RoundedRectShape{.radius = 18.0},
        .selector = WindowSelector{
            .address = "0xabc123",
            .pid = 4042,
            .initialClass = "org.gnome.Nautilus",
        },
        .geometry = std::nullopt,
        .stage = std::nullopt,
        .transition = std::nullopt,
        .enabled = true,
    };
}

TargetIdentity identity() {
    return {"config", "files"};
}

WindowSnapshot window() {
    return {
        .address = "0xabc123",
        .objectToken = 77,
        .pid = 4042,
        .initialClass = "org.gnome.Nautilus",
        .globalGeometry = Rect{
            .x = -800.0,
            .y = 120.0,
            .width = 900.0,
            .height = 700.0,
        },
        .opacity = 0.8,
        .mapped = true,
        .fadingOut = false,
        .readyToDelete = false,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"exact guarded window resolves before its surface", [] {
            const std::array windows{window()};
            const auto result = resolveWindowAttachment(
                identity(),
                target(),
                windows);
            require(result.hasValue() && result.value(), "window did not resolve");
            require(
                result.value()->globalGeometry ==
                    Rect{.x = -800.0, .y = 120.0, .width = 900.0, .height = 700.0},
                "window geometry changed");
            require(result.value()->objectToken == 77U, "window object identity changed");
            require(!result.value()->outputFilter, "spanning window was output-filtered");
            require(result.value()->opacity == 0.8, "window opacity changed");
            require(result.value()->stage == RenderStage::PreWindow, "window stage changed");
        }},
        Case{"either identity guard may protect the address", [] {
            for (int guard = 0; guard < 2; ++guard) {
                auto guarded = target();
                auto& selector = std::get<WindowSelector>(guarded.selector);
                auto snapshot = window();
                if (guard == 0) {
                    selector.initialClass.reset();
                    snapshot.initialClass.clear();
                } else {
                    selector.pid.reset();
                    snapshot.pid = 0;
                }
                const std::array windows{snapshot};
                const auto result = resolveWindowAttachment(
                    identity(),
                    guarded,
                    windows);
                require(result.hasValue() && result.value(), "single identity guard did not resolve");
            }
        }},
        Case{"reused address with changed identity fails closed", [] {
            for (int guard = 0; guard < 2; ++guard) {
                auto reused = window();
                if (guard == 0)
                    reused.pid += 1;
                else
                    reused.initialClass = "org.example.Impostor";
                const std::array windows{reused};
                const auto result = resolveWindowAttachment(
                    identity(),
                    target(),
                    windows);
                require(!result, "reused address passed its identity guard");
                require(result.error().code == ErrorCode::UnresolvedTarget, "wrong reused-address code");
            }
        }},
        Case{"unmapped fading and deleted windows do not resolve", [] {
            for (int state = 0; state < 3; ++state) {
                auto unavailable = window();
                if (state == 0)
                    unavailable.mapped = false;
                else if (state == 1)
                    unavailable.fadingOut = true;
                else
                    unavailable.readyToDelete = true;
                const std::array windows{unavailable};
                const auto result = resolveWindowAttachment(
                    identity(),
                    target(),
                    windows);
                require(!result, "unavailable window resolved");
                require(result.error().code == ErrorCode::UnresolvedTarget, "wrong unavailable-window code");
            }
        }},
        Case{"duplicate live address is ambiguous", [] {
            auto second = window();
            second.objectToken = 78;
            const std::array windows{window(), second};
            const auto result = resolveWindowAttachment(
                identity(),
                target(),
                windows);
            require(!result, "duplicate address resolved");
            require(result.error().code == ErrorCode::UnresolvedTarget, "wrong ambiguous-window code");
        }},
        Case{"unrelated malformed snapshots are ignored", [] {
            auto unrelated = window();
            unrelated.address = "";
            unrelated.objectToken = 0;
            const std::array windows{unrelated, window()};
            const auto result = resolveWindowAttachment(
                identity(),
                target(),
                windows);
            require(result.hasValue() && result.value(), "exact address was not selected");
            require(result.value()->objectToken == 77U, "unrelated snapshot changed selection");
        }},
        Case{"disabled target has no attachment", [] {
            auto disabled = target();
            disabled.enabled = false;
            const std::array windows{window()};
            const auto result = resolveWindowAttachment(
                identity(),
                disabled,
                windows);
            require(result.hasValue() && !result.value(), "disabled window resolved");
        }},
        Case{"malformed matching snapshot fails closed", [] {
            auto malformed = window();
            malformed.globalGeometry.width = 0.0;
            const std::array windows{malformed};
            const auto result = resolveWindowAttachment(
                identity(),
                target(),
                windows);
            require(!result, "invalid window geometry was accepted");
            require(result.error().path == "window.geometry", "wrong malformed-window path");
        }},
        Case{"adapter rejects wrong kind identity and noncanonical address", [] {
            auto wrongKind = target();
            wrongKind.kind = TargetKind::Layer;
            const std::array windows{window()};
            require(
                !resolveWindowAttachment(identity(), wrongKind, windows),
                "wrong target kind was accepted");

            auto wrongIdentity = identity();
            wrongIdentity.targetId = "other";
            require(
                !resolveWindowAttachment(wrongIdentity, target(), windows),
                "mismatched target identity was accepted");

            auto noncanonical = target();
            std::get<WindowSelector>(noncanonical.selector).address = "0xABC123";
            require(
                !resolveWindowAttachment(identity(), noncanonical, windows),
                "noncanonical address was accepted");
        }},
    });
}
