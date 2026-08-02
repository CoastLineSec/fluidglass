#include "v2/targets/WindowRuleResolver.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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

bool usableInitialClass(std::string_view value) {
    return !value.empty() &&
        value.size() <= Limits::MAX_IDENTIFIER_BYTES &&
        std::ranges::none_of(value, [](const unsigned char character) {
            return std::iscntrl(character);
        });
}

} // namespace

Result<RuleResolution>
resolveWindowRules(
    const ConfigSnapshot& config,
    std::span<const WindowSnapshot> windows) {
    if (!config.enabled)
        return Result<RuleResolution>::success({});
    if (windows.size() > Limits::MAX_COMPOSITOR_OBJECTS)
        return invalid(
            ErrorCode::ResourceLimited,
            "windows",
            "compositor window count exceeds the supported limit");

    RuleResolution result;
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
        // Per-window problems cost this target alone: an XWayland window
        // whose class has not arrived yet, or a momentarily odd rounding
        // value, must not strip glass from every unrelated surface.
        const auto fileFailure = [&](ErrorCode code,
                                     std::string path,
                                     std::string message) {
            result.failures.push_back({
                .identity = {
                    .owner = std::string(CONFIG_TARGET_OWNER),
                    .targetId = "window." + rule->id + "." +
                        std::to_string(window.objectToken),
                },
                .error = {
                    .code = code,
                    .path = std::move(path),
                    .message = std::move(message),
                },
            });
        };
        if (!config.materials.contains(rule->material)) {
            fileFailure(
                ErrorCode::InvalidMaterial,
                "window_rules." + rule->id + ".material",
                "matched window rule references a missing material");
            continue;
        }
        if (!std::isfinite(window.rounding) ||
            window.rounding < 0.0) {
            fileFailure(
                ErrorCode::InvalidRequest,
                "windows[" + std::to_string(index) + "].rounding",
                "window rounding must be finite and non-negative");
            continue;
        }
        if (!std::isfinite(window.roundingPower) ||
            window.roundingPower <= 0.0 ||
            window.roundingPower > 16.0) {
            fileFailure(
                ErrorCode::InvalidRequest,
                "windows[" + std::to_string(index) + "].rounding_power",
                "window rounding power must be finite and in (0, 16]");
            continue;
        }
        if (window.pid <= 0 &&
            !usableInitialClass(window.initialClass)) {
            fileFailure(
                ErrorCode::UnresolvedTarget,
                "windows[" + std::to_string(index) + "].identity",
                "matched window lacks stable pid or initial-class evidence");
            continue;
        }

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
            .id = "window." + rule->id + "." +
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
            .owner = std::string(CONFIG_TARGET_OWNER),
            .targetId = definition.id,
        };
        const std::array selected{window};
        auto attachment = resolveWindowAttachment(
            identity,
            definition,
            selected);
        if (!attachment) {
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
            .roundingPower = window.roundingPower,
        });
    }
    return Result<RuleResolution>::success(std::move(result));
}

} // namespace hfg::v2
