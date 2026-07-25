#include "v2/targets/MaterialResolver.hpp"

#include "v2/core/Limits.hpp"

#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

Result<Material> failure(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<Material>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

} // namespace

Result<Material> resolveTargetMaterial(
    const ResolvedTarget& target,
    const ConfigSnapshot* config,
    std::span<const SessionSnapshot> sessions) {
    if (target.attachment.identity.owner.empty() ||
        target.attachment.identity.targetId.empty())
        return failure(
            ErrorCode::InvalidTarget,
            "target.identity",
            "resolved target owner and id must not be empty");
    if (target.definition.material.name.empty())
        return failure(
            ErrorCode::InvalidMaterial,
            "target.material.name",
            "material name must not be empty");
    if (sessions.size() > Limits::MAX_SESSIONS)
        return failure(
            ErrorCode::ResourceLimited,
            "sessions",
            "session count exceeds the supported limit");

    switch (target.definition.material.source) {
        case MaterialSource::Config: {
            if (!config)
                return failure(
                    ErrorCode::UnresolvedTarget,
                    "target.material",
                    "configuration materials are unavailable");
            const auto material = config->materials.find(
                target.definition.material.name);
            if (material == config->materials.end())
                return failure(
                    ErrorCode::InvalidMaterial,
                    "target.material.name",
                    "referenced configuration material does not exist");
            return Result<Material>::success(material->second);
        }
        case MaterialSource::Session: {
            const SessionSnapshot* owner = nullptr;
            for (const auto& session : sessions) {
                if (session.owner !=
                    target.attachment.identity.owner)
                    continue;
                if (owner)
                    return failure(
                        ErrorCode::InvalidRequest,
                        "sessions.owner",
                        "more than one session has the target owner");
                owner = &session;
            }
            if (!owner)
                return failure(
                    ErrorCode::UnresolvedTarget,
                    "target.material",
                    "target owner session is unavailable");
            const auto material = owner->materials.find(
                target.definition.material.name);
            if (material == owner->materials.end())
                return failure(
                    ErrorCode::InvalidMaterial,
                    "target.material.name",
                    "referenced owner-session material does not exist");
            return Result<Material>::success(material->second);
        }
    }
    return failure(
        ErrorCode::InvalidMaterial,
        "target.material.source",
        "unsupported material authority");
}

} // namespace hfg::v2
