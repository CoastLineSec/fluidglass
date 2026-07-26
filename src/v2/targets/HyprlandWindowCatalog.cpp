#include "v2/targets/HyprlandWindowCatalog.hpp"

#include "v2/core/Limits.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
#include <utility>

namespace hfg::v2 {
namespace {

Result<std::vector<WindowSnapshot>> unavailable(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<std::vector<WindowSnapshot>>::failure({
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

std::string addressFor(const PHLWINDOW& window) {
    return std::format(
        "0x{:x}",
        reinterpret_cast<std::uintptr_t>(window.get()));
}

} // namespace

Result<std::uint64_t> HyprlandWindowCatalog::objectTokenFor(
    PHLWINDOWREF window) {
    std::erase_if(m_tokens, [](const auto& entry) {
        return entry.first.expired();
    });
    if (window.expired())
        return Result<std::uint64_t>::failure({
            ErrorCode::UnresolvedTarget,
            "window",
            "window object is no longer live",
        });
    const auto existing = m_tokens.find(window);
    if (existing != m_tokens.end())
        return Result<std::uint64_t>::success(existing->second);
    if (m_lastToken == std::numeric_limits<std::uint64_t>::max())
        return Result<std::uint64_t>::failure({
            ErrorCode::ResourceLimited,
            "window.object_token",
            "window object token space is exhausted",
        });
    const auto token = ++m_lastToken;
    m_tokens.emplace(std::move(window), token);
    return Result<std::uint64_t>::success(token);
}

Result<std::vector<WindowSnapshot>>
HyprlandWindowCatalog::allSnapshots() {
    if (!g_pCompositor)
        return unavailable(
            ErrorCode::UnsupportedOperation,
            "compositor",
            "Hyprland compositor is unavailable");

    std::erase_if(m_tokens, [](const auto& entry) {
        return entry.first.expired();
    });
    std::vector<WindowSnapshot> result;
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window)
            continue;
        const auto candidateAddress = addressFor(window);

        PHLWINDOWREF reference = window;
        auto token = objectTokenFor(reference);
        if (!token)
            return Result<std::vector<WindowSnapshot>>::failure(
                token.error());
        const auto position = window->position(
            Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const auto size = window->size(
            Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        result.push_back({
            .address = candidateAddress,
            .objectToken = token.value(),
            .pid = static_cast<std::int64_t>(window->getPID()),
            .initialClass = window->m_initialClass,
            .currentClass = window->m_class,
            .initialTitle = window->m_initialTitle,
            .currentTitle = window->m_title,
            .globalGeometry = Rect{
                .x = position.x,
                .y = position.y,
                .width = size.x,
                .height = size.y,
            },
            .rounding = static_cast<double>(window->rounding()),
            .roundingPower =
                static_cast<double>(window->roundingPower()),
            .opacity = static_cast<double>(window->effectiveAlpha()),
            .mapped = window->m_isMapped,
            .fadingOut = false,
            .readyToDelete = false,
        });
        if (result.size() > Limits::MAX_COMPOSITOR_OBJECTS)
            return unavailable(
                ErrorCode::ResourceLimited,
                "windows",
                "compositor window count exceeds the supported limit");
    }
    return Result<std::vector<WindowSnapshot>>::success(
        std::move(result));
}

Result<std::vector<WindowSnapshot>>
HyprlandWindowCatalog::snapshots(std::string_view address) {
    if (!validAddress(address))
        return unavailable(
            ErrorCode::InvalidRequest,
            "address",
            "expected a canonical lower-case hexadecimal window address");
    auto all = allSnapshots();
    if (!all)
        return all;

    std::vector<WindowSnapshot> result;
    for (auto& snapshot : all.value()) {
        if (snapshot.address != address)
            continue;
        result.push_back(std::move(snapshot));
        if (result.size() > 1U)
            return unavailable(
                ErrorCode::UnresolvedTarget,
                "windows",
                "more than one compositor window has the selected address");
    }
    return Result<std::vector<WindowSnapshot>>::success(
        std::move(result));
}

Result<PHLWINDOW> HyprlandWindowCatalog::windowFor(
    std::uint64_t objectToken) {
    if (objectToken == 0U)
        return Result<PHLWINDOW>::failure({
            .code = ErrorCode::InvalidRequest,
            .path = "object_token",
            .message = "window object token must not be zero",
        });

    std::erase_if(m_tokens, [](const auto& entry) {
        return entry.first.expired();
    });
    for (const auto& [reference, token] : m_tokens) {
        if (token != objectToken)
            continue;
        const auto window = reference.lock();
        if (window)
            return Result<PHLWINDOW>::success(window);
    }
    return Result<PHLWINDOW>::failure({
        .code = ErrorCode::UnresolvedTarget,
        .path = "object_token",
        .message = "window object token is no longer live",
    });
}

void HyprlandWindowCatalog::clear() noexcept {
    m_tokens.clear();
}

} // namespace hfg::v2
