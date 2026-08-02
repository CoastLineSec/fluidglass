#include "v2/targets/LayerRuleResolver.hpp"

#include "v2/core/Limits.hpp"

#include <array>
#include <ranges>
#include <utility>

namespace hfg::v2 {
namespace {

Result<RuleResolution> invalid(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<RuleResolution>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

} // namespace

Result<RuleResolution>
resolveLayerRules(
    const ConfigSnapshot& config,
    std::span<const LayerSurfaceSnapshot> surfaces) {
    if (!config.enabled)
        return Result<RuleResolution>::success({});
    if (surfaces.size() > Limits::MAX_COMPOSITOR_OBJECTS)
        return invalid(
            ErrorCode::ResourceLimited,
            "layers",
            "compositor layer count exceeds the supported limit");

    RuleResolution result;
    for (const auto& surface : surfaces) {
        if (!surface.mapped ||
            surface.fadingOut ||
            surface.readyToDelete)
            continue;
        const auto rule = std::ranges::find_if(
            config.layerRules,
            [&](const LayerRule& candidate) {
                return candidate.matches(surface.namespaceName);
            });
        if (rule == config.layerRules.end())
            continue;

        Target definition{
            .id = "layer." + rule->id + "." +
                std::to_string(surface.objectToken),
            .kind = TargetKind::Layer,
            .material = MaterialReference{
                .source = MaterialSource::Config,
                .name = rule->material,
            },
            .shape = RoundedRectShape{.radius = 0.0},
            .selector = LayerSelector{
                .namespaceName = surface.namespaceName,
            },
            .geometry = std::nullopt,
            .stage = std::nullopt,
            .transition = std::nullopt,
            .enabled = true,
        };
        TargetIdentity identity{
            .owner = std::string(CONFIG_TARGET_OWNER),
            .targetId = definition.id,
        };
        if (!config.materials.contains(rule->material)) {
            result.failures.push_back({
                .identity = std::move(identity),
                .error = {
                    .code = ErrorCode::InvalidMaterial,
                    .path = "layer_rules." + rule->id + ".material",
                    .message =
                        "matched layer rule references a missing material",
                },
            });
            continue;
        }
        const std::array selected{surface};
        auto attachment = resolveLayerAttachment(
            identity,
            definition,
            selected);
        if (!attachment) {
            // A surface the adapter cannot place right now — mid-map, mid-
            // resize — costs this target alone, never the scene.
            result.failures.push_back({
                .identity = std::move(identity),
                .error = attachment.error(),
            });
            continue;
        }
        if (!attachment.value())
            continue;
        result.resolved.push_back({
            .definition = std::move(definition),
            .attachment = std::move(*attachment.value()),
            .roundingPower = 2.0,
        });
    }
    return Result<RuleResolution>::success(std::move(result));
}

} // namespace hfg::v2
