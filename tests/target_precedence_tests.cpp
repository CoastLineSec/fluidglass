#include "TestHarness.hpp"

#include "v2/targets/TargetPrecedence.hpp"

#include <array>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

ResolvedTarget target(
    TargetIdentity identity,
    TargetKind kind,
    std::uint64_t objectToken,
    Rect geometry = {
        .x = 10.0,
        .y = 20.0,
        .width = 300.0,
        .height = 200.0,
    },
    RenderStage stage = RenderStage::PostWindows) {
    Target definition{
        .id = identity.targetId,
        .kind = kind,
        .material = {
            .source = identity.owner == "config" ?
                MaterialSource::Config :
                MaterialSource::Session,
            .name = "fluid",
        },
        .shape = RoundedRectShape{.radius = 12.0},
        .selector = kind == TargetKind::Window ?
            TargetSelector{WindowSelector{
                .address = "0xabc",
                .pid = 42,
                .initialClass = std::nullopt,
            }} :
            kind == TargetKind::Layer ?
                TargetSelector{LayerSelector{
                    .namespaceName = "example:bar",
                }} :
                TargetSelector{RegionSelector{
                    .output = "DP-1",
                }},
        .geometry = kind == TargetKind::Region ?
            std::optional<Rect>{geometry} :
            std::nullopt,
        .stage = kind == TargetKind::Region ?
            std::optional<RenderStage>{stage} :
            std::nullopt,
        .transition = std::nullopt,
        .enabled = true,
    };
    return {
        .definition = std::move(definition),
        .attachment = {
            .identity = std::move(identity),
            .kind = kind,
            .objectToken = objectToken,
            .globalGeometry = geometry,
            .stage = kind == TargetKind::Window ?
                RenderStage::PreWindow :
                stage,
            .outputFilter = kind == TargetKind::Window ?
                std::nullopt :
                std::optional<std::string>{"DP-1"},
            .opacity = 1.0,
        },
        .roundingPower = 2.0,
    };
}

SessionSnapshot session(
    std::string owner,
    SessionMode mode) {
    return {
        .owner = std::move(owner),
        .clientId = "example",
        .mode = mode,
        .generation = 1,
        .expiresAtMs = 1000,
        .materials = {},
        .targets = {},
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"client target overrides durable exact attachment", [] {
            const std::array durable{
                target({"config", "files.1"}, TargetKind::Window, 1),
            };
            const std::array leased{
                target({"client:example:s1", "files"}, TargetKind::Window, 1),
            };
            const std::array sessions{
                session("client:example:s1", SessionMode::Client),
            };
            const auto result =
                selectEffectiveTargets(durable, leased, sessions);
            require(result.hasValue(), "client precedence failed");
            require(result.value().targets.size() == 1U, "wrong effective target count");
            require(result.value().targets.front().attachment.identity == leased.front().attachment.identity, "client did not win");
            require(result.value().suppressed == std::vector{durable.front().attachment.identity}, "durable target was not suppressed");
            require(result.value().conflicts.empty(), "valid precedence became a conflict");
        }},
        Case{"preview overrides client and durable", [] {
            const std::array durable{
                target({"config", "files.1"}, TargetKind::Window, 1),
            };
            const std::array leased{
                target({"client:example:s1", "files"}, TargetKind::Window, 1),
                target({"preview:lab:s2", "preview"}, TargetKind::Window, 1),
            };
            const std::array sessions{
                session("client:example:s1", SessionMode::Client),
                session("preview:lab:s2", SessionMode::Preview),
            };
            const auto result =
                selectEffectiveTargets(durable, leased, sessions);
            require(result.hasValue(), "preview precedence failed");
            require(result.value().targets.size() == 1U, "wrong preview target count");
            require(result.value().targets.front().attachment.identity == leased[1].attachment.identity, "preview did not win");
            require(result.value().suppressed.size() == 2U, "lower authorities were not suppressed");
        }},
        Case{"equal authority collision fails closed", [] {
            const std::array leased{
                target({"client:first:s1", "glass"}, TargetKind::Window, 1),
                target({"client:second:s2", "glass"}, TargetKind::Window, 1),
            };
            const std::array sessions{
                session("client:first:s1", SessionMode::Client),
                session("client:second:s2", SessionMode::Client),
            };
            const auto result =
                selectEffectiveTargets({}, leased, sessions);
            require(result.hasValue(), "collision failed globally");
            require(result.value().targets.empty(), "equal-precedence collision selected a winner");
            require(result.value().conflicts.size() == 2U, "both conflicting owners were not reported");
        }},
        Case{"different layer subregions can coexist", [] {
            const std::array leased{
                target(
                    {"client:example:s1", "left"},
                    TargetKind::Layer,
                    2,
                    {.x = 0.0, .y = 0.0, .width = 100.0, .height = 48.0}),
                target(
                    {"client:example:s1", "right"},
                    TargetKind::Layer,
                    2,
                    {.x = 100.0, .y = 0.0, .width = 100.0, .height = 48.0}),
            };
            const std::array sessions{
                session("client:example:s1", SessionMode::Client),
            };
            const auto result =
                selectEffectiveTargets({}, leased, sessions);
            require(result.hasValue(), "layer subregion selection failed");
            require(result.value().targets.size() == 2U, "distinct layer subregions collided");
        }},
        Case{"region stage participates in exact attachment identity", [] {
            const std::array leased{
                target(
                    {"client:example:s1", "early"},
                    TargetKind::Region,
                    3,
                    {.x = 10.0, .y = 20.0, .width = 300.0, .height = 200.0},
                    RenderStage::PostWallpaper),
                target(
                    {"client:example:s1", "late"},
                    TargetKind::Region,
                    3,
                    {.x = 10.0, .y = 20.0, .width = 300.0, .height = 200.0},
                    RenderStage::PostWindows),
            };
            const std::array sessions{
                session("client:example:s1", SessionMode::Client),
            };
            const auto result =
                selectEffectiveTargets({}, leased, sessions);
            require(result.hasValue() && result.value().targets.size() == 2U, "different region stages collided");
        }},
        Case{"unknown leased owner fails closed", [] {
            const std::array leased{
                target({"client:missing:s1", "glass"}, TargetKind::Window, 1),
            };
            const auto result =
                selectEffectiveTargets(
                    std::span<const ResolvedTarget>{},
                    leased,
                    std::span<const SessionSnapshot>{});
            require(!result, "unknown leased owner was accepted");
            require(result.error().path == "leased.identity.owner", "wrong unknown-owner path");
        }},
    });
}
