#include "v2/model/Session.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <ranges>
#include <string_view>

namespace hfg::v2 {
namespace {

template <typename T>
Result<T> failure(ErrorCode code, std::string path, std::string message) {
    return Result<T>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

bool validClientId(std::string_view value) {
    if (value.empty() || value.size() > Limits::MAX_IDENTIFIER_BYTES)
        return false;
    return std::ranges::all_of(value, [](const unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-' || character == '.';
    });
}

bool validOpaqueValue(std::string_view value) {
    return !value.empty() && value.size() <= Limits::MAX_IDENTIFIER_BYTES &&
        std::ranges::none_of(value, [](const unsigned char character) {
            return std::iscntrl(character);
        });
}

} // namespace

SessionManager::SessionManager(OpaqueIdFactory opaqueIdFactory) : m_opaqueIdFactory(std::move(opaqueIdFactory)) {}

Result<SessionHandle> SessionManager::open(std::string clientId, SessionMode mode, std::uint64_t nowMs) {
    sweepExpired(nowMs);
    pruneTombstones(nowMs);

    if (!validClientId(clientId))
        return failure<SessionHandle>(ErrorCode::InvalidRequest, "client_id", "invalid client id");
    if (!m_opaqueIdFactory)
        return failure<SessionHandle>(ErrorCode::InternalError, "", "opaque id factory is unavailable");
    if (m_sessions.size() >= Limits::MAX_SESSIONS)
        return failure<SessionHandle>(ErrorCode::ResourceLimited, "sessions", "session limit reached");

    std::string sessionId;
    for (std::size_t attempt = 0; attempt < 16U; ++attempt) {
        sessionId = m_opaqueIdFactory();
        if (validOpaqueValue(sessionId) && !m_sessions.contains(sessionId) && !m_tombstones.contains(sessionId))
            break;
        sessionId.clear();
    }
    if (sessionId.empty())
        return failure<SessionHandle>(ErrorCode::InternalError, "session_id", "could not mint a unique session id");

    const std::string token = m_opaqueIdFactory();
    if (!validOpaqueValue(token))
        return failure<SessionHandle>(ErrorCode::InternalError, "token", "could not mint a valid session token");

    const std::uint64_t lease = mode == SessionMode::Preview ? Limits::PREVIEW_LEASE_MS : Limits::CLIENT_LEASE_MS;
    Record record{
        .sessionId = sessionId,
        .token = token,
        .clientId = std::move(clientId),
        .mode = mode,
        .generation = 0,
        .leaseMs = lease,
        .expiresAtMs = saturatingDeadline(nowMs, lease),
        .materials = {},
        .targets = {},
    };
    const auto handle = SessionHandle{
        .sessionId = record.sessionId,
        .token = record.token,
        .generation = record.generation,
        .leaseMs = record.leaseMs,
        .expiresAtMs = record.expiresAtMs,
    };
    m_sessions.emplace(record.sessionId, std::move(record));
    return Result<SessionHandle>::success(handle);
}

Result<SessionSnapshot> SessionManager::replace(
    std::string_view sessionId,
    std::string_view token,
    SessionReplacement replacement,
    const std::set<std::string>& configMaterials,
    std::uint64_t nowMs) {
    sweepExpired(nowMs);
    auto record = m_sessions.find(std::string(sessionId));
    if (record == m_sessions.end())
        return failure<SessionSnapshot>(ErrorCode::SessionNotFound, "session_id", "session not found");
    if (record->second.token != token)
        return failure<SessionSnapshot>(ErrorCode::InvalidToken, "token", "invalid session token");
    if (replacement.generation != record->second.generation + 1U)
        return failure<SessionSnapshot>(ErrorCode::StaleGeneration, "generation", "expected current generation plus one");
    if (replacement.materials.size() > Limits::MAX_MATERIALS_PER_OWNER)
        return failure<SessionSnapshot>(ErrorCode::ResourceLimited, "materials", "material limit exceeded");
    if (replacement.targets.size() > Limits::MAX_TARGETS_PER_SESSION)
        return failure<SessionSnapshot>(ErrorCode::ResourceLimited, "targets", "per-session target limit exceeded");

    const std::size_t otherTargets = targetCount() - record->second.targets.size();
    if (replacement.targets.size() > Limits::MAX_DYNAMIC_TARGETS - otherTargets)
        return failure<SessionSnapshot>(ErrorCode::ResourceLimited, "targets", "global dynamic target limit exceeded");

    for (const auto& [name, material] : replacement.materials) {
        if (name != material.name)
            return failure<SessionSnapshot>(ErrorCode::InvalidMaterial, "materials." + name, "material key and name differ");
    }

    std::set<std::string> targetIds;
    for (std::size_t index = 0; index < replacement.targets.size(); ++index) {
        const auto& target = replacement.targets[index];
        const auto  path = "targets[" + std::to_string(index) + "]";
        if (!targetIds.insert(target.id).second)
            return failure<SessionSnapshot>(ErrorCode::InvalidTarget, path + ".id", "target ids must be unique");
        if (target.material.source == MaterialSource::Session) {
            if (!replacement.materials.contains(target.material.name))
                return failure<SessionSnapshot>(ErrorCode::InvalidMaterial, path + ".material", "session material not found");
        } else if (!configMaterials.contains(target.material.name)) {
            return failure<SessionSnapshot>(ErrorCode::InvalidMaterial, path + ".material", "config material not found");
        }
    }

    record->second.generation  = replacement.generation;
    record->second.materials   = std::move(replacement.materials);
    record->second.targets     = std::move(replacement.targets);
    record->second.expiresAtMs = saturatingDeadline(nowMs, record->second.leaseMs);
    return Result<SessionSnapshot>::success(snapshotFor(record->second));
}

Result<SessionHandle> SessionManager::heartbeat(
    std::string_view sessionId,
    std::string_view token,
    std::uint64_t generation,
    std::uint64_t nowMs) {
    sweepExpired(nowMs);
    auto record = m_sessions.find(std::string(sessionId));
    if (record == m_sessions.end())
        return failure<SessionHandle>(ErrorCode::SessionNotFound, "session_id", "session not found");
    if (record->second.token != token)
        return failure<SessionHandle>(ErrorCode::InvalidToken, "token", "invalid session token");
    if (record->second.generation != generation)
        return failure<SessionHandle>(ErrorCode::StaleGeneration, "generation", "heartbeat generation is stale");

    record->second.expiresAtMs = saturatingDeadline(nowMs, record->second.leaseMs);
    return Result<SessionHandle>::success({
        .sessionId = record->second.sessionId,
        .token = record->second.token,
        .generation = record->second.generation,
        .leaseMs = record->second.leaseMs,
        .expiresAtMs = record->second.expiresAtMs,
    });
}

Result<void> SessionManager::close(std::string_view sessionId, std::string_view token, std::uint64_t nowMs) {
    sweepExpired(nowMs);
    pruneTombstones(nowMs);
    auto record = m_sessions.find(std::string(sessionId));
    if (record == m_sessions.end()) {
        const auto tombstone = m_tombstones.find(std::string(sessionId));
        if (tombstone != m_tombstones.end() && tombstone->second.token == token)
            return Result<void>::success();
        return failure<void>(ErrorCode::SessionNotFound, "session_id", "session not found");
    }
    if (record->second.token != token)
        return failure<void>(ErrorCode::InvalidToken, "token", "invalid session token");

    m_tombstones[record->first] = {
        .token = record->second.token,
        .expiresAtMs = saturatingDeadline(nowMs, Limits::SESSION_TOMBSTONE_MS),
    };
    m_sessions.erase(record);
    return Result<void>::success();
}

std::vector<ExpiredSession> SessionManager::expire(std::uint64_t nowMs) {
    sweepExpired(nowMs);
    auto expired = std::move(m_pendingExpired);
    m_pendingExpired.clear();
    return expired;
}

void SessionManager::sweepExpired(std::uint64_t nowMs) {
    for (auto record = m_sessions.begin(); record != m_sessions.end();) {
        if (record->second.expiresAtMs > nowMs) {
            ++record;
            continue;
        }
        m_pendingExpired.push_back({
            .owner = ownerFor(record->second),
            .generation = record->second.generation,
        });
        record = m_sessions.erase(record);
    }
    pruneTombstones(nowMs);
}

std::optional<SessionSnapshot> SessionManager::snapshot(std::string_view sessionId) const {
    const auto record = m_sessions.find(std::string(sessionId));
    if (record == m_sessions.end())
        return std::nullopt;
    return snapshotFor(record->second);
}

std::size_t SessionManager::sessionCount() const noexcept {
    return m_sessions.size();
}

std::size_t SessionManager::targetCount() const noexcept {
    std::size_t count = 0;
    for (const auto& [id, record] : m_sessions) {
        static_cast<void>(id);
        count += record.targets.size();
    }
    return count;
}

std::string SessionManager::ownerFor(const Record& record) {
    const std::string prefix = record.mode == SessionMode::Preview ? "preview:" : "client:";
    return prefix + record.clientId + ":" + record.sessionId;
}

SessionSnapshot SessionManager::snapshotFor(const Record& record) {
    return {
        .owner = ownerFor(record),
        .clientId = record.clientId,
        .mode = record.mode,
        .generation = record.generation,
        .expiresAtMs = record.expiresAtMs,
        .materials = record.materials,
        .targets = record.targets,
    };
}

std::uint64_t SessionManager::saturatingDeadline(std::uint64_t nowMs, std::uint64_t leaseMs) {
    if (leaseMs > std::numeric_limits<std::uint64_t>::max() - nowMs)
        return std::numeric_limits<std::uint64_t>::max();
    return nowMs + leaseMs;
}

void SessionManager::pruneTombstones(std::uint64_t nowMs) {
    std::erase_if(m_tombstones, [nowMs](const auto& entry) {
        return entry.second.expiresAtMs <= nowMs;
    });
}

} // namespace hfg::v2
