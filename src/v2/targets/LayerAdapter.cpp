#include "v2/targets/LayerAdapter.hpp"

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

bool validName(std::string_view value) {
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

std::optional<RenderStage> stageFor(LayerLevel level) {
    switch (level) {
        case LayerLevel::Background:
        case LayerLevel::Bottom:
            return RenderStage::PostWallpaper;
        case LayerLevel::Top:
        case LayerLevel::Overlay:
            return RenderStage::PostWindows;
    }
    return std::nullopt;
}

} // namespace

Result<std::optional<ResolvedAttachment>> resolveLayerAttachment(
    TargetIdentity identity,
    const Target& target,
    std::span<const LayerSurfaceSnapshot> surfaces) {
    if (identity.owner.empty() ||
        identity.targetId.empty() ||
        identity.targetId != target.id)
        return invalid(
            ErrorCode::InvalidRequest,
            "identity",
            "owner and matching target id are required");
    if (target.kind != TargetKind::Layer)
        return invalid(
            ErrorCode::InvalidTarget,
            "target.kind",
            "layer adapter requires a layer target");
    const auto* selector = std::get_if<LayerSelector>(&target.selector);
    if (!selector)
        return invalid(
            ErrorCode::InvalidTarget,
            "target.selector",
            "layer target requires a namespace selector");
    if (!target.enabled)
        return Result<std::optional<ResolvedAttachment>>::success(
            std::nullopt);

    const LayerSurfaceSnapshot* match = nullptr;
    for (std::size_t index = 0; index < surfaces.size(); ++index) {
        const auto& surface = surfaces[index];
        if (surface.namespaceName != selector->namespaceName)
            continue;
        if (!validName(surface.namespaceName))
            return invalid(
                ErrorCode::InvalidRequest,
                "surfaces[" + std::to_string(index) + "].namespace",
                "expected a non-empty bounded layer namespace");
        if (!surface.mapped ||
            surface.fadingOut ||
            surface.readyToDelete)
            continue;
        if (match)
            return invalid(
                ErrorCode::UnresolvedTarget,
                "target.selector.namespace",
                "layer namespace resolves to more than one mapped surface");
        match = &surface;
    }
    if (!match)
        return invalid(
            ErrorCode::UnresolvedTarget,
            "target.selector.namespace",
            "no mapped layer surface has the selected namespace");

    const auto& surface = *match;
    if (surface.objectToken == 0U)
        return invalid(
            ErrorCode::InvalidRequest,
            "surface.object_token",
            "layer object token must not be zero");
    if (!validName(surface.output))
        return invalid(
            ErrorCode::InvalidRequest,
            "surface.output",
            "expected a non-empty bounded output name");
    if (!validCoordinate(surface.globalGeometry.x) ||
        !validCoordinate(surface.globalGeometry.y) ||
        !validSize(surface.globalGeometry.width) ||
        !validSize(surface.globalGeometry.height))
        return invalid(
            ErrorCode::InvalidRequest,
            "surface.geometry",
            "expected finite positive layer geometry");
    if (!std::isfinite(surface.opacity) ||
        surface.opacity < 0.0 ||
        surface.opacity > 1.0)
        return invalid(
            ErrorCode::InvalidRequest,
            "surface.opacity",
            "expected a finite value from 0 to 1");
    const auto stage = stageFor(surface.level);
    if (!stage)
        return invalid(
            ErrorCode::UnsupportedOperation,
            "surface.level",
            "unsupported layer level");

    Rect geometry = surface.globalGeometry;
    if (target.geometry) {
        const double left = std::max(0.0, target.geometry->x);
        const double top = std::max(0.0, target.geometry->y);
        const double right = std::min(
            surface.globalGeometry.width,
            target.geometry->x + target.geometry->width);
        const double bottom = std::min(
            surface.globalGeometry.height,
            target.geometry->y + target.geometry->height);
        if (right <= left || bottom <= top)
            return Result<std::optional<ResolvedAttachment>>::success(
                std::nullopt);
        geometry = {
            .x = surface.globalGeometry.x + left,
            .y = surface.globalGeometry.y + top,
            .width = right - left,
            .height = bottom - top,
        };
    }

    return Result<std::optional<ResolvedAttachment>>::success(
        ResolvedAttachment{
            .identity = std::move(identity),
            .kind = TargetKind::Layer,
            .objectToken = surface.objectToken,
            .globalGeometry = geometry,
            .stage = *stage,
            .outputFilter = surface.output,
            .opacity = surface.opacity,
        });
}

} // namespace hfg::v2
