#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Material.hpp"
#include "v2/model/Target.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hfg::v2 {

enum class SessionMode {
    Client,
    Preview,
};

struct SessionHandle {
    std::string   sessionId;
    std::string   token;
    std::uint64_t generation  = 0;
    std::uint64_t leaseMs     = 0;
    std::uint64_t expiresAtMs = 0;

    friend bool operator==(const SessionHandle&, const SessionHandle&) = default;
};

struct PresentationHandoffRequest {
    enum class MorphCoordinateSpace {
        SurfaceLocal,
        OutputLocal,
    };

    struct MorphEndpoint {
        Rect   rect;
        double radius = 0.0;

        friend bool operator==(const MorphEndpoint&,
                               const MorphEndpoint&) = default;
    };

    struct Morph {
        std::string                 transitionId;
        std::uint64_t               durationMs = 0;
        MorphCoordinateSpace        coordinateSpace =
            MorphCoordinateSpace::SurfaceLocal;
        std::optional<MorphEndpoint> source = std::nullopt;
        std::optional<MorphEndpoint> destination = std::nullopt;

        friend bool operator==(const Morph&, const Morph&) = default;
    };

    std::string   targetId;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t timeoutMs = 0;
    std::optional<Morph> morph = std::nullopt;

    friend bool operator==(const PresentationHandoffRequest&,
                           const PresentationHandoffRequest&) = default;
};

struct SessionReplacement {
    std::uint64_t                generation = 0;
    std::map<std::string, Material> materials;
    std::vector<Target>          targets;
    std::vector<PresentationHandoffRequest> handoffs = {};
};

struct SessionSnapshot {
    std::string                    owner;
    std::string                    clientId;
    SessionMode                    mode = SessionMode::Client;
    std::uint64_t                  generation = 0;
    std::uint64_t                  expiresAtMs = 0;
    std::uint64_t                  transitionAnchorMs = 0;
    std::map<std::string, Material> materials;
    std::vector<Target>            targets;
};

struct ExpiredSession {
    std::string              owner;
    std::uint64_t generation = 0;
    std::vector<std::string> targetIds;
};

class SessionManager {
  public:
    using OpaqueIdFactory = std::function<std::string()>;

    explicit SessionManager(OpaqueIdFactory opaqueIdFactory);

    [[nodiscard]] Result<SessionHandle> open(std::string clientId, SessionMode mode, std::uint64_t nowMs);

    [[nodiscard]] Result<SessionSnapshot> replace(
        std::string_view sessionId,
        std::string_view token,
        SessionReplacement replacement,
        const std::set<std::string>& configMaterials,
        std::uint64_t nowMs);

    [[nodiscard]] Result<SessionHandle> heartbeat(
        std::string_view sessionId,
        std::string_view token,
        std::uint64_t generation,
        std::uint64_t nowMs);

    [[nodiscard]] Result<void> close(
        std::string_view sessionId,
        std::string_view token,
        std::uint64_t nowMs);

    [[nodiscard]] Result<SessionSnapshot> inspect(
        std::string_view sessionId,
        std::string_view token,
        std::uint64_t nowMs);

    [[nodiscard]] std::vector<ExpiredSession> expire(std::uint64_t nowMs);
    [[nodiscard]] std::optional<SessionSnapshot> snapshot(std::string_view sessionId) const;
    [[nodiscard]] std::vector<SessionSnapshot> snapshots() const;

    [[nodiscard]] std::size_t sessionCount() const noexcept;
    [[nodiscard]] std::size_t targetCount() const noexcept;

  private:
    struct Record {
        std::string                    sessionId;
        std::string                    token;
        std::string                    clientId;
        SessionMode                    mode = SessionMode::Client;
        std::uint64_t                  generation = 0;
        std::uint64_t                  leaseMs = 0;
        std::uint64_t                  expiresAtMs = 0;
        std::uint64_t                  transitionAnchorMs = 0;
        std::map<std::string, Material> materials;
        std::vector<Target>            targets;
    };

    struct Tombstone {
        std::string   token;
        std::uint64_t expiresAtMs = 0;
    };

    [[nodiscard]] static std::string ownerFor(const Record& record);
    [[nodiscard]] static SessionSnapshot snapshotFor(const Record& record);
    [[nodiscard]] static std::uint64_t saturatingDeadline(std::uint64_t nowMs, std::uint64_t leaseMs);
    void sweepExpired(std::uint64_t nowMs);
    void pruneTombstones(std::uint64_t nowMs);

    OpaqueIdFactory                m_opaqueIdFactory;
    std::map<std::string, Record>  m_sessions;
    std::map<std::string, Tombstone> m_tombstones;
    std::vector<ExpiredSession>    m_pendingExpired;
};

} // namespace hfg::v2
