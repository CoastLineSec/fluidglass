#include "v2/targets/LayerRuleResolver.hpp"

#include "v2/core/Limits.hpp"

#include <array>
#include <ranges>
#include <utility>

namespace hfg::v2 {
namespace {

Result<std::vector<ResolvedTarget>> invalid(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<std::vector<ResolvedTarget>>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

} // namespace

Result<std::vector<ResolvedTarget>>
resolveLayerRules(
    const ConfigSnapshot& config,
    std::span<const LayerSurfaceSnapshot> surfaces) {
    if (!config.enabled)
        return Result<std::vector<ResolvedTarget>>::success({});
    if (surfaces.size() > Limits::MAX_COMPOSITOR_OBJECTS)
        return invalid(
            ErrorCode::ResourceLimited,
            "layers",
            "compositor layer count exceeds the supported limit");

    std::vector<ResolvedTarget> result;
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
        if (!config.materials.contains(rule->material))
            return invalid(
                ErrorCode::InvalidMaterial,
                "layer_rules." + rule->id + ".material",
                "matched layer rule references a missing material");

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
            .owner = "config",
            .targetId = definition.id,
        };
        const std::array selected{surface};
        auto attachment = resolveLayerAttachment(
            identity,
            definition,
            selected);
        if (!attachment)
            return Result<std::vector<ResolvedTarget>>::failure(
                attachment.error());
        if (!attachment.value())
            continue;
        result.push_back({
            .definition = std::move(definition),
            .attachment = std::move(*attachment.value()),
            .roundingPower = 2.0,
        });
    }
    return Result<std::vector<ResolvedTarget>>::success(
        std::move(result));
}

} // namespace hfg::v2
