#include "TestHarness.hpp"

#include "v2/render/PresentationScene.hpp"

#include <array>
#include <utility>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

Material material(std::string name, double glassLevel) {
    MaterialInput input;
    input.glassLevel = glassLevel;
    auto result = validateMaterial(std::move(name), input);
    require(result.hasValue(), "test material was invalid");
    return std::move(result.value());
}

OutputGeneration output(
    std::string name,
    double logicalX,
    std::uint64_t objectToken) {
    return {
        .snapshot = OutputSnapshot{
            .name = std::move(name),
            .objectToken = objectToken,
            .modeToken = objectToken,
            .bufferWidth = 100,
            .bufferHeight = 100,
            .logicalX = logicalX,
            .logicalY = 0.0,
            .logicalWidth = 100.0,
            .logicalHeight = 100.0,
            .scale = 1.0,
            .transform = OutputTransform::Normal,
            .renderFormat = 0x34325241U,
            .colorStateToken = 1,
        },
        .generation = 1,
    };
}

ResolvedTarget target(
    std::string owner,
    std::string id,
    MaterialSource source,
    std::string materialName,
    Rect geometry) {
    return {
        .definition = Target{
            .id = id,
            .kind = TargetKind::Region,
            .material = {
                .source = source,
                .name = std::move(materialName),
            },
            .shape = RoundedRectShape{},
            .selector = RegionSelector{.output = "DP-1"},
            .geometry = geometry,
            .stage = RenderStage::PostWindows,
            .transition = std::nullopt,
            .enabled = true,
        },
        .attachment = ResolvedAttachment{
            .identity = {
                .owner = std::move(owner),
                .targetId = std::move(id),
            },
            .kind = TargetKind::Region,
            .objectToken = 1,
            .globalGeometry = geometry,
            .stage = RenderStage::PostWindows,
            .outputFilter = std::nullopt,
            .opacity = 1.0,
        },
        .roundingPower = 2.0,
    };
}

ConfigSnapshot config() {
    return {
        .version = 2,
        .enabled = true,
        .defaultMaterial = "shared",
        .materials = {
            {"shared", material("shared", 0.25)},
        },
        .windowRules = {},
        .layerRules = {},
    };
}

SessionSnapshot session() {
    return {
        .owner = "client:demo:s1",
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
        Case{"target spanning outputs creates independent presentations", [] {
            const auto active = config();
            const std::array outputs{
                output("DP-1", 0.0, 1),
                output("DP-2", 100.0, 2),
            };
            TargetScene targets{
                .effective = {
                    target(
                        "config",
                        "wide",
                        MaterialSource::Config,
                        "shared",
                        Rect{50.0, 10.0, 100.0, 40.0}),
                },
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            const auto result = buildPresentationScene(
                targets,
                &active,
                std::span<const SessionSnapshot>{},
                outputs);
            require(result.hasValue(), "presentation scene failed");
            require(result.value().presentations.size() == 2U,
                    "spanning target did not create two presentations");
            require(result.value().presentations[0].output.snapshot.name !=
                        result.value().presentations[1].output.snapshot.name,
                    "presentations reused one output generation");
            require(result.value().presentations[0].material.name == "shared",
                    "configuration material was not attached");
        }},
        Case{"owner material and sampling reach follow each presentation", [] {
            const std::array sessions{session()};
            const std::array outputs{
                output("DP-1", 0.0, 1),
            };
            TargetScene targets{
                .effective = {
                    target(
                        "client:demo:s1",
                        "local",
                        MaterialSource::Session,
                        "local",
                        Rect{10.0, 10.0, 40.0, 40.0}),
                },
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            const auto result = buildPresentationScene(
                targets,
                nullptr,
                sessions,
                outputs);
            require(result.hasValue() &&
                        result.value().presentations.size() == 1U,
                    "session presentation did not plan");
            const auto& planned =
                result.value().presentations.front();
            require(planned.material.glassLevel == 0.75,
                    "owner-session material changed");
            require(planned.sampling.apronPixels > 0U,
                    "material sampling footprint was omitted");
        }},
        Case{"missing material fails only its target", [] {
            const auto active = config();
            const std::array outputs{
                output("DP-1", 0.0, 1),
            };
            TargetScene targets{
                .effective = {
                    target(
                        "config",
                        "missing",
                        MaterialSource::Config,
                        "missing",
                        Rect{10.0, 10.0, 20.0, 20.0}),
                    target(
                        "config",
                        "valid",
                        MaterialSource::Config,
                        "shared",
                        Rect{40.0, 10.0, 20.0, 20.0}),
                },
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            const auto result = buildPresentationScene(
                targets,
                &active,
                std::span<const SessionSnapshot>{},
                outputs);
            require(result.hasValue(), "one material failure discarded the scene");
            require(result.value().presentations.size() == 1U,
                    "valid sibling presentation was lost");
            require(result.value().failures.size() == 1U &&
                        result.value().failures.front().identity.targetId ==
                            "missing",
                    "material failure was not retained by target");
        }},
        Case{"fully clipped target becomes inactive", [] {
            const auto active = config();
            const std::array outputs{
                output("DP-1", 0.0, 1),
            };
            TargetScene targets{
                .effective = {
                    target(
                        "config",
                        "offscreen",
                        MaterialSource::Config,
                        "shared",
                        Rect{200.0, 10.0, 20.0, 20.0}),
                },
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            const auto result = buildPresentationScene(
                targets,
                &active,
                std::span<const SessionSnapshot>{},
                outputs);
            require(result.hasValue(), "clipped scene failed");
            require(result.value().presentations.empty(),
                    "clipped target produced a presentation");
            require(result.value().inactive.size() == 1U &&
                        result.value().inactive.front().targetId ==
                            "offscreen",
                    "clipped target was not marked inactive");
        }},
        Case{"duplicate current output names fail globally", [] {
            const auto active = config();
            const std::array outputs{
                output("DP-1", 0.0, 1),
                output("DP-1", 0.0, 2),
            };
            const auto result = buildPresentationScene(
                TargetScene{},
                &active,
                std::span<const SessionSnapshot>{},
                outputs);
            require(!result &&
                        result.error().code ==
                            ErrorCode::StaleGeneration,
                    "ambiguous current output set was accepted");
        }},
        Case{"definition and attachment identity cannot diverge", [] {
            const auto active = config();
            auto malformed = target(
                "config",
                "original",
                MaterialSource::Config,
                "shared",
                Rect{10.0, 10.0, 20.0, 20.0});
            malformed.attachment.identity.targetId = "different";
            TargetScene targets{
                .effective = {std::move(malformed)},
                .inactive = {},
                .suppressed = {},
                .failures = {},
            };
            const auto result = buildPresentationScene(
                targets,
                &active,
                std::span<const SessionSnapshot>{},
                std::span<const OutputGeneration>{});
            require(!result &&
                        result.error().code == ErrorCode::InvalidTarget,
                    "divergent resolved identity was accepted");
        }},
        Case{"target scene state is carried into presentation state", [] {
            TargetScene targets{
                .effective = {},
                .inactive = {{"client:a:s1", "inactive"}},
                .suppressed = {{"config", "suppressed"}},
                .failures = {{
                    .identity = {"client:b:s2", "failed"},
                    .error = {
                        .code = ErrorCode::UnresolvedTarget,
                        .path = "target",
                        .message = "missing",
                    },
                }},
            };
            const auto result = buildPresentationScene(
                targets,
                nullptr,
                std::span<const SessionSnapshot>{},
                std::span<const OutputGeneration>{});
            require(result.hasValue(), "empty presentation scene failed");
            require(result.value().inactive == targets.inactive,
                    "inactive state was lost");
            require(result.value().suppressed == targets.suppressed,
                    "suppressed state was lost");
            require(result.value().failures == targets.failures,
                    "resolution failures were lost");
        }},
    });
}
