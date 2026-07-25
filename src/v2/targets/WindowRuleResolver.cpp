#include "v2/targets/WindowRuleResolver.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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

bool usableInitialClass(std::string_view value) {
    return !value.empty() &&
        value.size() <= Limits::MAX_IDENTIFIER_BYTES &&
        std::ranges::none_of(value, [](const unsigned char character) {
            return std::iscntrl(character);
        });
}

} // namespace

Result<std::vector<ResolvedTarget>>
resolveWindowRules(
    const ConfigSnapshot& config,
    std::span<const WindowSnapshot> windows) {
    if (!config.enabled)
        return Result<std::vector<ResolvedTarget>>::success({});
    if (windows.size() > Limits::MAX_COMPOSITOR_OBJECTS)
        return invalid(
            ErrorCode::ResourceLimited,
            "windows",
            "compositor window count exceeds the supported limit");

    std::vector<ResolvedTarget> result;
    for (std::size_t index = 0; index < windows.size(); ++index) {
        const auto& window = windows[index];
        if (!window.mapped ||
            window.fadingOut ||
            window.readyToDelete)
            continue;

        const auto rule = std::ranges::find_if(
            config.windowRules,
            [&](const WindowRule& candidate) {
                return candidate.matches({
                    .initialClass = window.initialClass,
                    .currentClass = window.currentClass,
                    .initialTitle = window.initialTitle,
                    .currentTitle = window.currentTitle,
                });
            });
        if (rule == config.windowRules.end())
            continue;
        if (!config.materials.contains(rule->material))
            return invalid(
                ErrorCode::InvalidMaterial,
                "window_rules." + rule->id + ".material",
                "matched window rule references a missing material");
        if (!std::isfinite(window.rounding) ||
            window.rounding < 0.0)
            return invalid(
                ErrorCode::InvalidRequest,
                "windows[" + std::to_string(index) + "].rounding",
                "window rounding must be finite and non-negative");
        if (!std::isfinite(window.roundingPower) ||
            window.roundingPower <= 0.0 ||
            window.roundingPower > 16.0)
            return invalid(
                ErrorCode::InvalidRequest,
                "windows[" + std::to_string(index) + "].rounding_power",
                "window rounding power must be finite and in (0, 16]");
        if (window.pid <= 0 &&
            !usableInitialClass(window.initialClass))
            return invalid(
                ErrorCode::UnresolvedTarget,
                "windows[" + std::to_string(index) + "].identity",
                "matched window lacks stable pid or initial-class evidence");

        WindowSelector selector{
            .address = window.address,
            .pid = std::nullopt,
            .initialClass = std::nullopt,
        };
        if (window.pid > 0)
            selector.pid = window.pid;
        if (usableInitialClass(window.initialClass))
            selector.initialClass = window.initialClass;

        Target definition{
            .id = rule->id + "." +
                std::to_string(window.objectToken),
            .kind = TargetKind::Window,
            .material = MaterialReference{
                .source = MaterialSource::Config,
                .name = rule->material,
            },
            .shape = RoundedRectShape{
                .radius = window.rounding,
            },
            .selector = std::move(selector),
            .geometry = std::nullopt,
            .stage = std::nullopt,
            .transition = std::nullopt,
            .enabled = true,
        };
        TargetIdentity identity{
            .owner = "config",
            .targetId = definition.id,
        };
        const std::array selected{window};
        auto attachment = resolveWindowAttachment(
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
            .roundingPower = window.roundingPower,
        });
    }
    return Result<std::vector<ResolvedTarget>>::success(
        std::move(result));
}

} // namespace hfg::v2
