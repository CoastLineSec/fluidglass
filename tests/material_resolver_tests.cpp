#include "TestHarness.hpp"

#include "v2/targets/MaterialResolver.hpp"

#include <array>
#include <limits>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

Material material(std::string name, double glassLevel) {
    MaterialInput input;
    input.glassLevel = glassLevel;
    auto result = validateMaterial(
        std::move(name),
        input);
    require(result.hasValue(), "test material was invalid");
    return std::move(result.value());
}

ResolvedTarget target(
    std::string owner,
    MaterialSource source,
    std::string name) {
    return {
        .definition = Target{
            .id = "surface",
            .kind = TargetKind::Region,
            .material = {
                .source = source,
                .name = std::move(name),
            },
            .shape = RoundedRectShape{},
            .selector = RegionSelector{.output = "DP-1"},
            .geometry = Rect{0.0, 0.0, 10.0, 10.0},
            .stage = RenderStage::PostWindows,
            .transition = std::nullopt,
            .enabled = true,
        },
        .attachment = ResolvedAttachment{
            .identity = {
                .owner = std::move(owner),
                .targetId = "surface",
            },
            .kind = TargetKind::Region,
            .objectToken = 1,
            .globalGeometry = {0.0, 0.0, 10.0, 10.0},
            .stage = RenderStage::PostWindows,
            .outputFilter = "DP-1",
            .opacity = 1.0,
        },
        .roundingPower = 2.0,
    };
}

ConfigSnapshot config() {
    return {
        .version = 2,
        .enabled = false,
        .defaultMaterial = "shared",
        .materials = {
            {"shared", material("shared", 0.25)},
        },
        .windowRules = {},
        .layerRules = {},
    };
}

SessionSnapshot session(
    std::string owner = "client:demo:s1") {
    return {
        .owner = std::move(owner),
        .clientId = "demo",
        .mode = SessionMode::Client,
        .generation = 1,
        .expiresAtMs = 1000,
        .materials = {
            {"local", material("local", 0.75)},
        },
        .targets = {},
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"configuration material resolves independently of durable rules", [] {
            const auto active = config();
            const auto result = resolveTargetMaterial(
                target("client:demo:s1", MaterialSource::Config, "shared"),
                &active,
                std::span<const SessionSnapshot>{});
            require(result.hasValue(), "configuration material did not resolve");
            require(result.value().name == "shared" &&
                        result.value().glassLevel == 0.25,
                    "wrong configuration material resolved");
        }},
        Case{"session material resolves only from the target owner", [] {
            const std::array sessions{
                session("client:other:s2"),
                session(),
            };
            const auto result = resolveTargetMaterial(
                target("client:demo:s1", MaterialSource::Session, "local"),
                nullptr,
                sessions);
            require(result.hasValue(), "owner-session material did not resolve");
            require(result.value().glassLevel == 0.75,
                    "wrong owner-session material resolved");
        }},
        Case{"missing material authorities fail explicitly", [] {
            auto result = resolveTargetMaterial(
                target("client:demo:s1", MaterialSource::Config, "shared"),
                nullptr,
                std::span<const SessionSnapshot>{});
            require(!result &&
                        result.error().code == ErrorCode::UnresolvedTarget,
                    "missing configuration authority was accepted");

            result = resolveTargetMaterial(
                target("client:demo:s1", MaterialSource::Session, "local"),
                nullptr,
                std::span<const SessionSnapshot>{});
            require(!result &&
                        result.error().code == ErrorCode::UnresolvedTarget,
                    "missing session authority was accepted");
        }},
        Case{"material lookup never crosses owner sessions", [] {
            const std::array sessions{
                session("client:other:s2"),
            };
            const auto result = resolveTargetMaterial(
                target("client:demo:s1", MaterialSource::Session, "local"),
                nullptr,
                sessions);
            require(!result, "material leaked across session owners");
        }},
        Case{"duplicate session ownership fails closed", [] {
            const std::array sessions{session(), session()};
            const auto result = resolveTargetMaterial(
                target("client:demo:s1", MaterialSource::Session, "local"),
                nullptr,
                sessions);
            require(!result &&
                        result.error().path == "sessions.owner",
                    "ambiguous owner session was selected");
        }},
        Case{"missing and unsupported material values fail", [] {
            const auto active = config();
            auto missing = resolveTargetMaterial(
                target("config", MaterialSource::Config, "missing"),
                &active,
                std::span<const SessionSnapshot>{});
            require(!missing &&
                        missing.error().code == ErrorCode::InvalidMaterial,
                    "missing named material was accepted");

            auto malformed = target(
                "config",
                static_cast<MaterialSource>(
                    std::numeric_limits<int>::max()),
                "shared");
            const auto unsupported = resolveTargetMaterial(
                malformed,
                &active,
                std::span<const SessionSnapshot>{});
            require(!unsupported &&
                        unsupported.error().path ==
                            "target.material.source",
                    "unsupported material authority was accepted");
        }},
    });
}
