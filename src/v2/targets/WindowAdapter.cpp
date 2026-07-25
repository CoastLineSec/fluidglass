#include "v2/targets/WindowAdapter.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace hfg::v2 {
namespace {

constexpr double MAX_LOGICAL_VALUE = 1'000'000.0;

Result<std::optional<ResolvedAttachment>> invalid(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<std::optional<ResolvedAttachment>>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

bool validAddress(std::string_view value) {
    if (!value.starts_with("0x") ||
        value.size() <= 2U ||
        value.size() > 2U + 2U * sizeof(std::uintptr_t))
        return false;
    return std::ranges::all_of(
        value.substr(2U),
        [](const unsigned char character) {
            return std::isxdigit(character) &&
                !std::isupper(character);
        });
}

bool validOpaqueName(std::string_view value) {
    return !value.empty() &&
        value.size() <= Limits::MAX_IDENTIFIER_BYTES &&
        std::ranges::none_of(value, [](const unsigned char character) {
            return std::iscntrl(character);
        });
}

bool validCoordinate(double value) {
    return std::isfinite(value) &&
        std::abs(value) <= MAX_LOGICAL_VALUE;
}

bool validSize(double value) {
    return std::isfinite(value) &&
        value > 0.0 &&
        value <= MAX_LOGICAL_VALUE;
}

} // namespace

Result<std::optional<ResolvedAttachment>>
resolveWindowAttachment(
    TargetIdentity identity,
    const Target& target,
    std::span<const WindowSnapshot> windows) {
    if (identity.owner.empty() ||
        identity.targetId.empty() ||
        identity.targetId != target.id)
        return invalid(
            ErrorCode::InvalidRequest,
            "identity",
            "owner and matching target id are required");
    if (target.kind != TargetKind::Window)
        return invalid(
            ErrorCode::InvalidTarget,
            "target.kind",
            "window adapter requires a window target");
    const auto* selector = std::get_if<WindowSelector>(&target.selector);
    if (!selector)
        return invalid(
            ErrorCode::InvalidTarget,
            "target.selector",
            "window target requires an address selector");
    if (!validAddress(selector->address))
        return invalid(
            ErrorCode::InvalidTarget,
            "target.selector.address",
            "window address must be canonical lower-case hexadecimal");
    if (!selector->pid && !selector->initialClass)
        return invalid(
            ErrorCode::InvalidTarget,
            "target.selector",
            "window selector requires pid or initial-class identity evidence");
    if (!target.enabled)
        return Result<std::optional<ResolvedAttachment>>::success(
            std::nullopt);

    const WindowSnapshot* match = nullptr;
    for (std::size_t index = 0; index < windows.size(); ++index) {
        const auto& window = windows[index];
        if (window.address != selector->address)
            continue;
        if (!validAddress(window.address))
            return invalid(
                ErrorCode::InvalidRequest,
                "windows[" + std::to_string(index) + "].address",
                "window snapshot address is not canonical");
        if (!window.mapped ||
            window.fadingOut ||
            window.readyToDelete)
            continue;
        if (match)
            return invalid(
                ErrorCode::UnresolvedTarget,
                "target.selector.address",
                "window address resolves to more than one mapped object");
        match = &window;
    }
    if (!match)
        return invalid(
            ErrorCode::UnresolvedTarget,
            "target.selector.address",
            "no mapped window has the selected address");

    const auto& window = *match;
    if (window.objectToken == 0U)
        return invalid(
            ErrorCode::InvalidRequest,
            "window.object_token",
            "window object token must not be zero");
    if (selector->pid) {
        if (window.pid <= 0)
            return invalid(
                ErrorCode::InvalidRequest,
                "window.pid",
                "window snapshot pid must be greater than zero when used as identity evidence");
        if (*selector->pid != window.pid)
            return invalid(
                ErrorCode::UnresolvedTarget,
                "target.selector.pid",
                "window pid identity evidence no longer matches");
    }
    if (selector->initialClass) {
        if (!validOpaqueName(window.initialClass))
            return invalid(
                ErrorCode::InvalidRequest,
                "window.initial_class",
                "window snapshot initial class is invalid when used as identity evidence");
        if (*selector->initialClass != window.initialClass)
            return invalid(
                ErrorCode::UnresolvedTarget,
                "target.selector.initial_class",
                "window initial-class identity evidence no longer matches");
    }
    if (!validCoordinate(window.globalGeometry.x) ||
        !validCoordinate(window.globalGeometry.y) ||
        !validSize(window.globalGeometry.width) ||
        !validSize(window.globalGeometry.height))
        return invalid(
            ErrorCode::InvalidRequest,
            "window.geometry",
            "expected finite positive window geometry");
    if (!std::isfinite(window.opacity) ||
        window.opacity < 0.0 ||
        window.opacity > 1.0)
        return invalid(
            ErrorCode::InvalidRequest,
            "window.opacity",
            "expected a finite value from 0 to 1");

    return Result<std::optional<ResolvedAttachment>>::success(
        ResolvedAttachment{
            .identity = std::move(identity),
            .kind = TargetKind::Window,
            .objectToken = window.objectToken,
            .globalGeometry = window.globalGeometry,
            .stage = RenderStage::PreWindow,
            .outputFilter = std::nullopt,
            .opacity = window.opacity,
        });
}

} // namespace hfg::v2
