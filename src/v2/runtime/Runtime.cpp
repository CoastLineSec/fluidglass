#include "v2/runtime/Runtime.hpp"

#include "v2/core/Limits.hpp"
#include "v2/ipc/Protocol.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace hfg::v2 {
namespace {

using json = nlohmann::json;

std::string_view sessionModeName(SessionMode mode) {
    return mode == SessionMode::Preview ? "preview" : "client";
}

std::string_view targetKindName(TargetKind kind) {
    switch (kind) {
        case TargetKind::Window: return "window";
        case TargetKind::Layer:  return "layer";
        case TargetKind::Region: return "region";
    }
    return "region";
}

std::string_view renderStageName(RenderStage stage) {
    switch (stage) {
        case RenderStage::PostWallpaper: return "post-wallpaper";
        case RenderStage::PreWindow:     return "pre-window";
        case RenderStage::PostWindows:   return "post-windows";
        case RenderStage::PostLayer:     return "post-layer";
    }
    return "post-windows";
}

std::string_view transitionPhaseName(TransitionPhase phase) {
    return phase == TransitionPhase::Exit ? "exit" : "enter";
}

std::string_view transitionEdgeName(TransitionEdge edge) {
    switch (edge) {
        case TransitionEdge::Top:    return "top";
        case TransitionEdge::Bottom: return "bottom";
        case TransitionEdge::Left:   return "left";
        case TransitionEdge::Right:  return "right";
    }
    return "top";
}

json transitionJson(const Transition& transition, std::optional<double> protrusion = std::nullopt) {
    json easing = json::array();
    for (const auto& segment : transition.easing)
        easing.push_back({
            {"control1_x", segment.control1X},
            {"control1_y", segment.control1Y},
            {"control2_x", segment.control2X},
            {"control2_y", segment.control2Y},
            {"end_x", segment.endX},
            {"end_y", segment.endY},
        });
    json result{
        {"id", transition.id},
        {"phase", transitionPhaseName(transition.phase)},
        {"edge", transitionEdgeName(transition.edge)},
        {"duration_ms", transition.durationMs},
        {"elapsed_ms", transition.elapsedMs},
        {"travel", transition.travel},
        {"easing", std::move(easing)},
    };
    if (protrusion)
        result["protrusion"] = *protrusion;
    return result;
}

json materialReferenceJson(const MaterialReference& reference) {
    return {
        {"source", reference.source == MaterialSource::Config ? "config" : "session"},
        {"name", reference.name},
    };
}

json rectJson(const Rect& rect, std::string_view space) {
    return {
        {"space", space},
        {"x", rect.x},
        {"y", rect.y},
        {"width", rect.width},
        {"height", rect.height},
    };
}

json localRectJson(const Rect& rect) {
    return {
        {"x", rect.x},
        {"y", rect.y},
        {"width", rect.width},
        {"height", rect.height},
    };
}

json cornerRadiiJson(const CornerRadii& corners) {
    return {
        {"top_left", corners.topLeft},
        {"top_right", corners.topRight},
        {"bottom_right", corners.bottomRight},
        {"bottom_left", corners.bottomLeft},
    };
}

bool uniformCorners(const CornerRadii& corners) {
    return corners.topLeft == corners.topRight &&
        corners.topLeft == corners.bottomRight &&
        corners.topLeft == corners.bottomLeft;
}

bool emptyCorners(const CornerRadii& corners) {
    return corners == CornerRadii{};
}

void insertCorners(json& object, const CornerRadii& corners) {
    if (uniformCorners(corners))
        object["radius"] = corners.topLeft;
    else
        object["corner_radii"] = cornerRadiiJson(corners);
}

json shapeJson(const Shape& shape) {
    return std::visit([](const auto& value) -> json {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, RoundedRectShape>) {
            return {
                {"kind", "rounded-rect"},
                {"radius", value.radius},
            };
        } else if constexpr (std::is_same_v<T, RingShape>) {
            return {
                {"kind", "ring"},
                {"outer_radius", value.outerRadius},
                {"thickness", value.thickness},
            };
        } else {
            json parts = json::array();
            for (const auto& part : value.parts) {
                json serialized = localRectJson(part.rect);
                insertCorners(serialized, part.corners);
                if (!emptyCorners(part.junctions))
                    serialized["junctions"] = cornerRadiiJson(part.junctions);
                if (part.materialExtent)
                    serialized["material_extent"] = localRectJson(*part.materialExtent);
                if (part.transition)
                    serialized["transition"] = transitionJson(
                        part.transition->motion,
                        part.transition->protrusion);
                if (part.opacity != 1.0)
                    serialized["opacity"] = part.opacity;
                parts.push_back(std::move(serialized));
            }
            json serialized{
                {"kind", "compound"},
                {"parts", std::move(parts)},
            };
            if (value.base) {
                json base = json::object();
                insertCorners(base, value.base->corners);
                serialized["base"] = std::move(base);
            }
            if (value.cutout) {
                json cutout = localRectJson(value.cutout->rect);
                insertCorners(cutout, value.cutout->corners);
                serialized["cutout"] = std::move(cutout);
            }
            if (!value.connectors.empty()) {
                json connectors = json::array();
                for (const auto& connector : value.connectors)
                    connectors.push_back(localRectJson(connector));
                serialized["connectors"] = std::move(connectors);
            }
            if (value.connectorCurve != 0.0)
                serialized["connector_curve"] = value.connectorCurve;
            return serialized;
        }
    }, shape);
}

json selectorJson(const Target& target) {
    return std::visit([](const auto& selector) -> json {
        using T = std::decay_t<decltype(selector)>;
        if constexpr (std::is_same_v<T, WindowSelector>) {
            json result{{"address", selector.address}};
            if (selector.pid)
                result["pid"] = *selector.pid;
            if (selector.initialClass)
                result["initial_class"] = *selector.initialClass;
            return result;
        } else if constexpr (std::is_same_v<T, LayerSelector>) {
            return {{"namespace", selector.namespaceName}};
        } else {
            return {{"output", selector.output}};
        }
    }, target.selector);
}

json targetJson(const Target& target) {
    json result{
        {"id", target.id},
        {"kind", targetKindName(target.kind)},
        {"selector", selectorJson(target)},
        {"material", materialReferenceJson(target.material)},
        {"shape", shapeJson(target.shape)},
        {"enabled", target.enabled},
    };
    if (target.geometry) {
        const auto space = target.kind == TargetKind::Layer ? "surface-local" : "output-logical";
        result["geometry"] = rectJson(*target.geometry, space);
    }
    if (target.stage)
        result["stage"] = renderStageName(*target.stage);
    if (target.transition)
        result["transition"] = transitionJson(*target.transition);
    return result;
}

json handleJson(const SessionHandle& handle) {
    return {
        {"session_id", handle.sessionId},
        {"token", handle.token},
        {"generation", handle.generation},
        {"lease_ms", handle.leaseMs},
        {"expires_at_ms", handle.expiresAtMs},
    };
}

json renewalJson(const SessionHandle& handle) {
    return {
        {"session_id", handle.sessionId},
        {"generation", handle.generation},
        {"lease_ms", handle.leaseMs},
        {"expires_at_ms", handle.expiresAtMs},
    };
}

std::set<std::string> configMaterialNames(const ConfigStore& store) {
    std::set<std::string> result;
    if (const auto* active = store.active())
        for (const auto& [name, material] : active->materials) {
            static_cast<void>(material);
            result.insert(name);
        }
    return result;
}

json errorJson(const std::optional<Error>& error) {
    if (!error)
        return nullptr;
    return {
        {"code", errorCodeName(error->code)},
        {"path", error->path},
        {"message", error->message},
    };
}

json capabilitiesJson(const RendererRuntimeStatus& renderer) {
    return {
        {"protocol_versions", json::array({2})},
        {"rendering_ready", renderer.renderingReady},
        {"target_kinds", json::array({"window", "layer", "region"})},
        {"shapes", json::array({"rounded-rect", "ring", "compound"})},
        {"transitions", {
            {"targets", true},
            {"compound_parts", true},
        }},
        {"output_liveness", {
            // status.outputs[]: one row per renderer-known output, held
            // continuously — the client contract for the glass/neutral gate.
            {"rows", true},
            {"continuous", true},
        }},
        {"target_readiness", {
            {"inactive_reporting", true},
            {"inactive_reasons", json::array({
                "disabled",
                "empty-geometry",
                "offscreen",
                "suppressed",
            })},
            {"detail", true},
        }},
        {"render_stages", json::array({"post-wallpaper", "pre-window", "post-windows", "post-layer"})},
        {"operations", json::array({
            "capabilities",
            "status",
            "session.open",
            "session.replace",
            "session.heartbeat",
            "session.close",
            "target.inspect",
        })},
        {"limits", {
            {"request_bytes", Limits::MAX_REQUEST_BYTES},
            {"json_nesting", Limits::MAX_JSON_NESTING},
            {"identifier_bytes", Limits::MAX_IDENTIFIER_BYTES},
            {"regex_bytes", Limits::MAX_REGEX_BYTES},
            {"sessions", Limits::MAX_SESSIONS},
            {"targets_per_session", Limits::MAX_TARGETS_PER_SESSION},
            {"dynamic_targets", Limits::MAX_DYNAMIC_TARGETS},
            {"materials_per_owner", Limits::MAX_MATERIALS_PER_OWNER},
            {"compound_parts", Limits::MAX_COMPOUND_PARTS},
            {"compound_connectors", Limits::MAX_COMPOUND_CONNECTORS},
            {"bezier_segments", Limits::MAX_BEZIER_SEGMENTS},
            {"transition_ms", Limits::MAX_TRANSITION_MS},
        }},
    };
}

json statusJson(
    const ConfigStore& config,
    const SessionManager& sessions,
    const ReadinessTracker& readiness,
    const RendererRuntimeStatus& renderer) {
    json sessionList = json::array();
    std::map<std::string_view, std::size_t> readinessTotals;
    for (const auto& snapshot : sessions.snapshots()) {
        sessionList.push_back({
            {"owner", snapshot.owner},
            {"mode", sessionModeName(snapshot.mode)},
            {"generation", snapshot.generation},
            {"expires_at_ms", snapshot.expiresAtMs},
            {"materials", snapshot.materials.size()},
            {"targets", snapshot.targets.size()},
        });
        for (const auto& target : snapshot.targets) {
            const TargetIdentity identity{.owner = snapshot.owner, .targetId = target.id};
            if (const auto record = readiness.target(identity))
                ++readinessTotals[readinessStateName(record->state)];
            for (const auto& [key, record] : readiness.presentations(identity)) {
                static_cast<void>(key);
                ++readinessTotals[readinessStateName(record.state)];
            }
        }
    }

    json readinessJson = json::object();
    for (const auto& [state, count] : readinessTotals)
        readinessJson[std::string(state)] = count;

    // One verdict per output: whether glass is drawing there. A client that
    // derives no geometry needs nothing finer than this to choose between glass
    // and its own neutral material.
    json outputsJson = json::array();
    for (const auto& liveness :
         outputGlassLiveness(readiness, renderer.outputs))
        outputsJson.push_back({
            {"name", liveness.output},
            {"generation", liveness.outputGeneration},
            {"drawing", liveness.drawing},
            {"drawn", liveness.drawn},
            {"awaiting", liveness.awaiting},
            {"failed", liveness.failed},
            {"inactive", liveness.inactive},
        });

    const auto* active = config.active();
    json reloadError = nullptr;
    if (const auto& error = config.pendingError())
        reloadError = {
            {"code", errorCodeName(error->code)},
            {"path", error->path},
            {"message", error->message},
        };

    return {
        {"renderer", {
            {"state", renderer.renderer},
            {"rendering_ready", renderer.renderingReady},
            {"presentations", renderer.presentations},
            {"capture_resources", renderer.captureResources},
            {"draws", renderer.draws},
            {"window_attachments", renderer.windowAttachments},
            {"direct_scanout_leases", renderer.directScanoutLeases},
            {"last_error", errorJson(renderer.lastError)},
        }},
        {"config", {
            {"active", active != nullptr},
            {"enabled", active ? active->enabled : false},
            {"generation", config.generation()},
            {"materials", active ? active->materials.size() : 0U},
            {"window_rules", active ? active->windowRules.size() : 0U},
            {"layer_rules", active ? active->layerRules.size() : 0U},
            {"last_reload_error", std::move(reloadError)},
        }},
        {"sessions", std::move(sessionList)},
        {"totals", {
            {"sessions", sessions.sessionCount()},
            {"dynamic_targets", sessions.targetCount()},
        }},
        {"readiness", std::move(readinessJson)},
        {"outputs", std::move(outputsJson)},
    };
}

json presentationJson(const PresentationKey& key, const ReadinessRecord& record) {
    return {
        {"output", key.output},
        {"output_generation", key.outputGeneration},
        {"stage", renderStageName(key.stage)},
        {"state", readinessStateName(record.state)},
        {"sequence", record.sequence},
        {"detail", record.detail},
    };
}

Result<json> inspectTarget(
    SessionManager& sessions,
    const ReadinessTracker& readiness,
    const InspectTargetRequest& request,
    std::uint64_t nowMs) {
    auto snapshot = sessions.inspect(request.sessionId, request.token, nowMs);
    if (!snapshot)
        return Result<json>::failure(snapshot.error());
    const auto target = std::ranges::find_if(snapshot.value().targets, [&](const Target& candidate) {
        return candidate.id == request.targetId;
    });
    if (target == snapshot.value().targets.end()) {
        return Result<json>::failure({
            .code = ErrorCode::InvalidTarget,
            .path = "target_id",
            .message = "target was not found in this session",
        });
    }

    const TargetIdentity identity{
        .owner = snapshot.value().owner,
        .targetId = target->id,
    };
    json presentations = json::array();
    for (const auto& [key, record] : readiness.presentations(identity))
        presentations.push_back(presentationJson(key, record));

    const auto targetState = readiness.target(identity);
    // "inactive" carries no error, so the reason is the only thing that tells
    // a client whether to keep waiting or to stop.
    json detail = nullptr;
    if (targetState && !targetState->detail.empty())
        detail = targetState->detail;
    return Result<json>::success({
        {"owner", snapshot.value().owner},
        {"generation", snapshot.value().generation},
        {"target", targetJson(*target)},
        {"state", targetState ? readinessStateName(targetState->state) : "accepted"},
        {"detail", std::move(detail)},
        {"presentations", std::move(presentations)},
    });
}

Result<json> dispatchRequest(
    const Request& request,
    ConfigStore& config,
    SessionManager& sessions,
    ReadinessTracker& readiness,
    const RendererRuntimeStatus& renderer,
    std::uint64_t nowMs) {
    return std::visit([&](const auto& body) -> Result<json> {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, CapabilitiesRequest>) {
            return Result<json>::success(capabilitiesJson(renderer));
        } else if constexpr (std::is_same_v<T, StatusRequest>) {
            return Result<json>::success(
                statusJson(config, sessions, readiness, renderer));
        } else if constexpr (std::is_same_v<T, OpenSessionRequest>) {
            auto opened = sessions.open(body.clientId, body.mode, nowMs);
            if (!opened)
                return Result<json>::failure(opened.error());
            return Result<json>::success(handleJson(opened.value()));
        } else if constexpr (std::is_same_v<T, ReplaceSessionRequest>) {
            const auto previous = sessions.snapshot(body.sessionId);
            auto replaced = sessions.replace(
                body.sessionId,
                body.token,
                body.replacement,
                configMaterialNames(config),
                nowMs);
            if (!replaced)
                return Result<json>::failure(replaced.error());

            // Readiness survives a replace for every target the successor
            // generation retains. Erasing them all would blink each output's
            // liveness on every publish — the renderer never stopped drawing,
            // so the signal must not claim it did. Only dropped targets are
            // erased, and only new ones start from accepted.
            std::set<std::string> successorIds;
            for (const auto& target : replaced.value().targets)
                successorIds.insert(target.id);
            if (previous)
                for (const auto& target : previous->targets)
                    if (!successorIds.contains(target.id))
                        readiness.erase(
                            {.owner = previous->owner, .targetId = target.id});
            for (const auto& target : replaced.value().targets) {
                const TargetIdentity identity{
                    .owner = replaced.value().owner,
                    .targetId = target.id,
                };
                if (readiness.target(identity))
                    continue;
                const auto accepted = readiness.accept(identity);
                if (!accepted)
                    return Result<json>::failure(accepted.error());
            }
            return Result<json>::success({
                {"owner", replaced.value().owner},
                {"generation", replaced.value().generation},
                {"expires_at_ms", replaced.value().expiresAtMs},
                {"materials", replaced.value().materials.size()},
                {"targets", replaced.value().targets.size()},
            });
        } else if constexpr (std::is_same_v<T, HeartbeatSessionRequest>) {
            auto renewed = sessions.heartbeat(
                body.sessionId,
                body.token,
                body.generation,
                nowMs);
            if (!renewed)
                return Result<json>::failure(renewed.error());
            return Result<json>::success(renewalJson(renewed.value()));
        } else if constexpr (std::is_same_v<T, CloseSessionRequest>) {
            const auto previous = sessions.snapshot(body.sessionId);
            auto closed = sessions.close(body.sessionId, body.token, nowMs);
            if (!closed)
                return Result<json>::failure(closed.error());
            if (previous)
                for (const auto& target : previous->targets)
                    readiness.erase({.owner = previous->owner, .targetId = target.id});
            return Result<json>::success({{"closed", true}});
        } else {
            return inspectTarget(sessions, readiness, body, nowMs);
        }
    }, request.body);
}

constexpr std::string_view INTERNAL_FAILURE =
    R"({"ok":false,"version":2,"error":{"code":"internal-error","path":"","message":"internal runtime failure"}})";

} // namespace

RuntimeService::RuntimeService(SessionManager::OpaqueIdFactory opaqueIdFactory)
    : m_sessions(std::move(opaqueIdFactory)) {}

std::string RuntimeService::handle(std::string_view payload, std::uint64_t nowMs) noexcept {
    try {
        tick(nowMs);
        auto request = parseRequest(payload);
        if (!request)
            return failureResponse(std::nullopt, request.error());
        auto result = dispatchRequest(request.value(), m_config, m_sessions,
                                      m_readiness, m_rendererStatus, nowMs);
        if (!result)
            return failureResponse(request.value().requestId, result.error());
        return successResponse(request.value().requestId, result.value());
    } catch (...) {
        return std::string(INTERNAL_FAILURE);
    }
}

void RuntimeService::tick(std::uint64_t nowMs) noexcept {
    try {
        expireSessions(nowMs);
    } catch (...) {
    }
}

ConfigStore& RuntimeService::configStore() noexcept {
    return m_config;
}

const ConfigStore& RuntimeService::configStore() const noexcept {
    return m_config;
}

SessionManager& RuntimeService::sessionManager() noexcept {
    return m_sessions;
}

const SessionManager& RuntimeService::sessionManager() const noexcept {
    return m_sessions;
}

ReadinessTracker& RuntimeService::readinessTracker() noexcept {
    return m_readiness;
}

const ReadinessTracker& RuntimeService::readinessTracker() const noexcept {
    return m_readiness;
}

void RuntimeService::setRendererStatus(RendererRuntimeStatus status) noexcept {
    m_rendererStatus = std::move(status);
}

const RendererRuntimeStatus& RuntimeService::rendererStatus() const noexcept {
    return m_rendererStatus;
}

void RuntimeService::expireSessions(std::uint64_t nowMs) {
    for (const auto& expired : m_sessions.expire(nowMs)) {
        for (const auto& targetId : expired.targetIds)
            m_readiness.erase({
                .owner = expired.owner,
                .targetId = targetId,
            });
    }
}

} // namespace hfg::v2
