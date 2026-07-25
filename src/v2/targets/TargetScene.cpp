#include "v2/targets/TargetScene.hpp"

#include "v2/targets/LayerRuleResolver.hpp"
#include "v2/targets/TargetPrecedence.hpp"
#include "v2/targets/WindowRuleResolver.hpp"

#include <iterator>
#include <ranges>
#include <utility>

namespace hfg::v2 {

Result<TargetScene>
buildTargetScene(
    const ConfigSnapshot* config,
    std::span<const SessionSnapshot> sessions,
    std::span<const WindowSnapshot> windows,
    std::span<const LayerSurfaceSnapshot> layers,
    std::span<const OutputGeneration> outputs) {
    std::vector<ResolvedTarget> durable;
    if (config) {
        auto windowTargets =
            resolveWindowRules(*config, windows);
        if (!windowTargets)
            return Result<TargetScene>::failure(
                windowTargets.error());
        auto layerTargets =
            resolveLayerRules(*config, layers);
        if (!layerTargets)
            return Result<TargetScene>::failure(
                layerTargets.error());
        durable.reserve(
            windowTargets.value().size() +
            layerTargets.value().size());
        std::ranges::move(
            windowTargets.value(),
            std::back_inserter(durable));
        std::ranges::move(
            layerTargets.value(),
            std::back_inserter(durable));
    }

    auto leased = resolveSessionTargets(
        sessions,
        windows,
        layers,
        outputs);
    if (!leased)
        return Result<TargetScene>::failure(
            leased.error());
    auto selected = selectEffectiveTargets(
        durable,
        leased.value().resolved,
        sessions);
    if (!selected)
        return Result<TargetScene>::failure(
            selected.error());

    TargetScene scene{
        .effective = std::move(selected.value().targets),
        .inactive = std::move(leased.value().inactive),
        .suppressed =
            std::move(selected.value().suppressed),
        .failures = std::move(leased.value().failures),
    };
    std::ranges::move(
        selected.value().conflicts,
        std::back_inserter(scene.failures));
    return Result<TargetScene>::success(
        std::move(scene));
}

} // namespace hfg::v2
