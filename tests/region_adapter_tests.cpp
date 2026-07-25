#include "TestHarness.hpp"

#include "v2/targets/RegionAdapter.hpp"

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

OutputGeneration output() {
    return {
        .snapshot = OutputSnapshot{
            .name = "DP-1",
            .objectToken = 44,
            .modeToken = 2,
            .bufferWidth = 2400,
            .bufferHeight = 1350,
            .logicalX = -1920.0,
            .logicalY = 100.0,
            .logicalWidth = 1920.0,
            .logicalHeight = 1080.0,
            .scale = 1.25,
            .transform = OutputTransform::Normal,
            .renderFormat = 0x34325241U,
            .colorStateToken = 7,
        },
        .generation = 5,
    };
}

Target target(bool enabled = true) {
    return {
        .id = "preview",
        .kind = TargetKind::Region,
        .material = MaterialReference{
            .source = MaterialSource::Session,
            .name = "fluid",
        },
        .shape = RoundedRectShape{.radius = 20.0},
        .selector = RegionSelector{.output = "DP-1"},
        .geometry = Rect{.x = 100.0, .y = 50.0, .width = 400.0, .height = 300.0},
        .stage = RenderStage::PostLayer,
        .transition = std::nullopt,
        .enabled = enabled,
    };
}

TargetIdentity identity() {
    return {"preview:lab:session", "preview"};
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"region geometry becomes global logical attachment geometry", [] {
            const auto result = resolveRegionAttachment(
                identity(),
                target(),
                output());
            require(result.hasValue() && result.value(), "region did not resolve");
            require(
                result.value()->globalGeometry ==
                    Rect{.x = -1820.0, .y = 150.0, .width = 400.0, .height = 300.0},
                "output-local geometry translation changed");
            require(result.value()->objectToken == 44U, "output object identity changed");
            require(result.value()->outputFilter == "DP-1", "region output filter changed");
            require(result.value()->stage == RenderStage::PostLayer, "region stage changed");
        }},
        Case{"resolved region uses common presentation mapping", [] {
            const auto generation = output();
            const auto attachment = resolveRegionAttachment(
                identity(),
                target(),
                generation);
            require(attachment.hasValue() && attachment.value(), "region did not resolve");
            const auto presentations = resolvePresentations(
                *attachment.value(),
                std::array{generation});
            require(
                presentations.hasValue() && presentations.value().size() == 1U,
                "region presentation did not map");
            require(
                presentations.value().front().geometry.coverage ==
                    PixelRect{.x = 125, .y = 62, .width = 500, .height = 376},
                "fractional region coverage changed");
        }},
        Case{"disabled region has no attachment", [] {
            const auto result = resolveRegionAttachment(
                identity(),
                target(false),
                output());
            require(result.hasValue() && !result.value(), "disabled region resolved");
        }},
        Case{"missing selected output remains unresolved", [] {
            auto generation = output();
            generation.snapshot.name = "HDMI-A-1";
            const auto result = resolveRegionAttachment(
                identity(),
                target(),
                generation);
            require(!result, "wrong output resolved");
            require(result.error().code == ErrorCode::UnresolvedTarget, "wrong unresolved-output code");
        }},
        Case{"adapter rejects wrong target kind and identity", [] {
            auto wrongKind = target();
            wrongKind.kind = TargetKind::Layer;
            require(
                !resolveRegionAttachment(identity(), wrongKind, output()),
                "wrong target kind was accepted");

            auto wrongIdentity = identity();
            wrongIdentity.targetId = "other";
            require(
                !resolveRegionAttachment(wrongIdentity, target(), output()),
                "mismatched target identity was accepted");
        }},
        Case{"adapter rejects stale output generation", [] {
            auto stale = output();
            stale.generation = 0;
            const auto result = resolveRegionAttachment(
                identity(),
                target(),
                stale);
            require(!result, "zero output generation was accepted");
            require(result.error().code == ErrorCode::StaleGeneration, "wrong stale-output code");
        }},
    });
}
