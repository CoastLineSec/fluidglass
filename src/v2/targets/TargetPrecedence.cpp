#include "v2/targets/TargetPrecedence.hpp"

#include "v2/core/Limits.hpp"

#include <cmath>
#include <map>
#include <optional>
#include <tuple>
#include <utility>

namespace hfg::v2 {
namespace {

enum class AuthorityRank {
    Config = 0,
    Client = 1,
    Preview = 2,
};

struct CollisionKey {
    TargetKind  kind = TargetKind::Region;
    std::uint64_t objectToken = 0;
    RenderStage stage = RenderStage::PostWindows;
    std::string output;
    Rect        geometry;

    friend bool operator==(const CollisionKey&, const CollisionKey&) = default;

    friend bool operator<(
        const CollisionKey& left,
        const CollisionKey& right) {
        return std::tie(
                   left.kind,
                   left.objectToken,
                   left.stage,
                   left.output,
                   left.geometry.x,
                   left.geometry.y,
                   left.geometry.width,
                   left.geometry.height) <
            std::tie(
                   right.kind,
                   right.objectToken,
                   right.stage,
                   right.output,
                   right.geometry.x,
                   right.geometry.y,
                   right.geometry.width,
                   right.geometry.height);
    }
};

struct Candidate {
    const ResolvedTarget* target = nullptr;
    AuthorityRank         rank = AuthorityRank::Config;
};

Result<EffectiveTargetSelection> invalid(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<EffectiveTargetSelection>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

bool validRect(const Rect& geometry) {
    return std::isfinite(geometry.x) &&
        std::isfinite(geometry.y) &&
        std::isfinite(geometry.width) &&
        std::isfinite(geometry.height) &&
        geometry.width > 0.0 &&
        geometry.height > 0.0;
}

Result<CollisionKey> collisionKey(
    const ResolvedTarget& target) {
    const auto& attachment = target.attachment;
    if (attachment.objectToken == 0U)
        return Result<CollisionKey>::failure({
            .code = ErrorCode::InvalidRequest,
            .path = "attachment.object_token",
            .message = "effective target object token must not be zero",
        });
    if (!validRect(attachment.globalGeometry))
        return Result<CollisionKey>::failure({
            .code = ErrorCode::InvalidRequest,
            .path = "attachment.geometry",
            .message = "effective target geometry must be finite and positive",
        });

    CollisionKey key{
        .kind = attachment.kind,
        .objectToken = attachment.objectToken,
        .stage = attachment.stage,
        .output = attachment.outputFilter.value_or(""),
        .geometry = attachment.globalGeometry,
    };
    if (attachment.kind == TargetKind::Window) {
        key.stage = RenderStage::PreWindow;
        key.output.clear();
        key.geometry = {};
    }
    // A layer target without explicit geometry derives its rectangle from the
    // surface, so two such targets on one surface always derive the same rect.
    // Keying them on the rect would make the collision an accident of that
    // equality; keying on the surface makes it the rule — one derived glass
    // per surface and stage, resolved by authority. Explicit sub-region
    // targets keep their rect so distinct subregions still coexist.
    if (attachment.kind == TargetKind::Layer &&
        !target.definition.geometry)
        key.geometry = {};
    return Result<CollisionKey>::success(std::move(key));
}

} // namespace

Result<EffectiveTargetSelection>
selectEffectiveTargets(
    std::span<const ResolvedTarget> durable,
    std::span<const ResolvedTarget> leased,
    std::span<const SessionSnapshot> sessions) {
    if (durable.size() >
            Limits::MAX_CAPTURE_REQUESTS ||
        leased.size() >
            Limits::MAX_CAPTURE_REQUESTS -
                durable.size())
        return invalid(
            ErrorCode::ResourceLimited,
            "targets",
            "effective target candidate count exceeds the supported limit");

    std::map<std::string_view, SessionMode> modes;
    for (const auto& session : sessions) {
        if (session.owner.empty() ||
            !modes.emplace(session.owner, session.mode).second)
            return invalid(
                ErrorCode::InvalidRequest,
                "sessions.owner",
                "session owners must be non-empty and unique");
    }

    std::map<CollisionKey, std::vector<Candidate>> groups;
    for (const auto& target : durable) {
        if (target.attachment.identity.owner != "config")
            return invalid(
                ErrorCode::InvalidRequest,
                "durable.identity.owner",
                "durable target must belong to config");
        auto key = collisionKey(target);
        if (!key)
            return Result<EffectiveTargetSelection>::failure(
                key.error());
        groups[std::move(key.value())].push_back({
            .target = &target,
            .rank = AuthorityRank::Config,
        });
    }
    for (const auto& target : leased) {
        const auto mode = modes.find(
            target.attachment.identity.owner);
        if (mode == modes.end())
            return invalid(
                ErrorCode::InvalidRequest,
                "leased.identity.owner",
                "leased target owner has no live session");
        auto key = collisionKey(target);
        if (!key)
            return Result<EffectiveTargetSelection>::failure(
                key.error());
        groups[std::move(key.value())].push_back({
            .target = &target,
            .rank = mode->second == SessionMode::Preview ?
                AuthorityRank::Preview :
                AuthorityRank::Client,
        });
    }

    EffectiveTargetSelection selection;
    for (const auto& [key, candidates] : groups) {
        static_cast<void>(key);
        AuthorityRank highest = AuthorityRank::Config;
        for (const auto& candidate : candidates)
            if (candidate.rank > highest)
                highest = candidate.rank;

        std::vector<const ResolvedTarget*> winners;
        for (const auto& candidate : candidates) {
            if (candidate.rank == highest)
                winners.push_back(candidate.target);
            else
                selection.suppressed.push_back(
                    candidate.target->attachment.identity);
        }
        if (winners.size() == 1U) {
            selection.targets.push_back(*winners.front());
            continue;
        }
        for (const auto* winner : winners)
            selection.conflicts.push_back({
                .identity = winner->attachment.identity,
                .error = {
                    .code = ErrorCode::UnresolvedTarget,
                    .path = "attachment",
                    .message = "equal-precedence targets select the same compositor attachment",
                },
            });
    }
    return Result<EffectiveTargetSelection>::success(
        std::move(selection));
}

} // namespace hfg::v2
