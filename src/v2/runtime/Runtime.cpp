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
        {"presentation_handoffs", {
            {"retain_until_drawn", true},
            {"target_kinds", json::array({"layer"})},
            {"geometry_morph", {
                {"layer_targets", true},
                {"coordinate_space", "surface-local"},
                {"coordinate_spaces", json::array({
                    "surface-local",
                    "output-local",
                })},
                {"shapes", json::array({"rounded-rect-uniform-radius"})},
                {"easings", json::array({"ease-out-cubic"})},
                {"anchor", "compositor-monotonic"},
                {"reversal", true},
                {"max_active_per_target", 1},
                {"max_active", Limits::MAX_DYNAMIC_TARGETS},
            }},
        }},
        {"visibility_transitions", {
            {"layer_targets", true},
            {"translation", true},
            {"opacity", true},
            {"activation", "first-successful-draw"},
            {"anchor", "compositor-monotonic"},
            {"easings", json::array({"ease-out-cubic"})},
            {"reversal", true},
            {"supersession", true},
            {"max_active_per_target", 1},
            {"max_active", Limits::MAX_DYNAMIC_TARGETS},
            {"maximum_duration_ms",
             Limits::MAX_VISIBILITY_TRANSITION_MS},
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
            {"presentation_handoff_ms", Limits::MAX_PRESENTATION_HANDOFF_MS},
            {"presentation_morph_ms", Limits::MAX_PRESENTATION_MORPH_MS},
            {"visibility_transition_ms",
             Limits::MAX_VISIBILITY_TRANSITION_MS},
        }},
    };
}

json statusJson(
    const ConfigStore& config,
    const SessionManager& sessions,
    const ReadinessTracker& readiness,
    const PresentationHandoffTracker& handoffs,
    const VisibilityTransitionTracker& visibility,
    const RendererRuntimeStatus& renderer) {
    json sessionList = json::array();
    std::map<std::string_view, std::size_t> readinessTotals;
    std::map<std::string_view, std::size_t> handoffTotals;
    std::map<std::string_view, std::size_t> morphTotals;
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
            if (const auto handoff = handoffs.target(identity)) {
                for (const auto& presentation : handoff->presentations)
                    ++handoffTotals[
                        presentationHandoffStateName(presentation.state)];
                if (handoff->morph)
                    ++morphTotals[
                        presentationMorphStateName(handoff->morph->state)];
            }
        }
    }

    json readinessJson = json::object();
    for (const auto& [state, count] : readinessTotals)
        readinessJson[std::string(state)] = count;
    json handoffJson = json::object();
    for (const auto& [state, count] : handoffTotals)
        handoffJson[std::string(state)] = count;
    json morphJson = json::object();
    for (const auto& [state, count] : morphTotals)
        morphJson[std::string(state)] = count;
    std::map<std::string_view, std::size_t> visibilityTotals;
    for (const auto& record : visibility.records())
        ++visibilityTotals[
            visibilityTransitionStateName(record.state)];
    json visibilityJson = json::object();
    for (const auto& [state, count] : visibilityTotals)
        visibilityJson[std::string(state)] = count;

    // One verdict per output: whether glass is drawing there. A client that
    // derives no geometry needs nothing finer than this to choose between glass
    // and its own neutral material.
    json outputsJson = json::array();
    for (const auto& liveness : outputGlassLiveness(readiness))
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
        {"presentation_handoffs", std::move(handoffJson)},
        {"presentation_morphs", std::move(morphJson)},
        {"visibility_transitions", std::move(visibilityJson)},
    };
}

json visibilityJson(const VisibilityTransitionRecord& record) {
    return {
        {"target_id", record.identity.targetId},
        {"transition_id", record.transitionId},
        {"source_generation", record.sourceGeneration},
        {"successor_generation", record.successorGeneration},
        {"direction",
         visibilityTransitionDirectionName(record.direction)},
        {"state", visibilityTransitionStateName(record.state)},
        {"anchor_ms", record.anchorMs},
        {"duration_ms", record.durationMs},
        {"easing", "ease-out-cubic"},
        {"activation", "first-successful-draw"},
        {"starting_progress", record.startingProgress},
        {"source_offset", {
            {"x", record.sourceOffset.x},
            {"y", record.sourceOffset.y},
        }},
        {"destination_offset", {
            {"x", record.destinationOffset.x},
            {"y", record.destinationOffset.y},
        }},
        {"source_opacity", record.sourceOpacity},
        {"destination_opacity", record.destinationOpacity},
        {"output", record.output},
        {"output_generation",
         record.outputGeneration
             ? json(*record.outputGeneration)
             : json(nullptr)},
        {"detail", record.detail},
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

json handoffJson(const PresentationHandoffRecord& handoff) {
    json presentations = json::array();
    bool retained = false;
    bool failed = false;
    for (const auto& presentation : handoff.presentations) {
        retained = retained ||
            presentation.state == PresentationHandoffState::Retained;
        failed = failed ||
            presentation.state == PresentationHandoffState::Failed;
        presentations.push_back({
            {"output", presentation.key.output},
            {"output_generation", presentation.key.outputGeneration},
            {"stage", renderStageName(presentation.key.stage)},
            {"state", presentationHandoffStateName(presentation.state)},
            {"detail", presentation.detail},
        });
    }
    const auto state = retained
        ? PresentationHandoffState::Retained
        : failed
            ? PresentationHandoffState::Failed
            : PresentationHandoffState::Completed;
    json morph = nullptr;
    if (handoff.morph) {
        const auto endpointJson = [](const PresentationMorphEndpoint& endpoint) {
            return json{
                {"rect", {
                    {"x", endpoint.rect.x},
                    {"y", endpoint.rect.y},
                    {"width", endpoint.rect.width},
                    {"height", endpoint.rect.height},
                }},
                {"radius", endpoint.radius},
            };
        };
        morph = {
            {"transition_id", handoff.morph->transitionId},
            {"state", presentationMorphStateName(handoff.morph->state)},
            {"coordinate_space",
             handoff.morph->coordinateSpace ==
                     PresentationHandoffRequest::MorphCoordinateSpace::
                         OutputLocal
                 ? "output-local"
                 : "surface-local"},
            {"anchor_ms", handoff.morph->anchorMs},
            {"duration_ms", handoff.morph->durationMs},
            {"easing", "ease-out-cubic"},
            {"source", endpointJson(handoff.morph->source)},
            {"destination", endpointJson(handoff.morph->destination)},
            {"detail", handoff.morph->detail},
        };
    }
    return {
        {"target_id", handoff.identity.targetId},
        {"source_generation", handoff.sourceGeneration},
        {"successor_generation", handoff.successorGeneration},
        {"expires_at_ms", handoff.expiresAtMs},
        {"state", presentationHandoffStateName(state)},
        {"presentations", std::move(presentations)},
        {"morph", std::move(morph)},
    };
}

Result<json> inspectTarget(
    SessionManager& sessions,
    const ReadinessTracker& readiness,
    const PresentationHandoffTracker& handoffs,
    const VisibilityTransitionTracker& visibility,
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
    json handoff = nullptr;
    if (const auto record = handoffs.target(identity))
        handoff = handoffJson(*record);
    json visibilityTransition = nullptr;
    if (const auto record = visibility.target(identity))
        visibilityTransition = visibilityJson(*record);
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
        {"handoff", std::move(handoff)},
        {"visibility_transition", std::move(visibilityTransition)},
    });
}

Result<json> dispatchRequest(
    const Request& request,
    ConfigStore& config,
    SessionManager& sessions,
    ReadinessTracker& readiness,
    PresentationHandoffTracker& handoffs,
    VisibilityTransitionTracker& visibility,
    const RendererRuntimeStatus& renderer,
    std::uint64_t nowMs) {
    return std::visit([&](const auto& body) -> Result<json> {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, CapabilitiesRequest>) {
            return Result<json>::success(capabilitiesJson(renderer));
        } else if constexpr (std::is_same_v<T, StatusRequest>) {
            return Result<json>::success(
                statusJson(
                    config, sessions, readiness, handoffs, visibility,
                    renderer));
        } else if constexpr (std::is_same_v<T, OpenSessionRequest>) {
            auto opened = sessions.open(body.clientId, body.mode, nowMs);
            if (!opened)
                return Result<json>::failure(opened.error());
            return Result<json>::success(handleJson(opened.value()));
        } else if constexpr (std::is_same_v<T, ReplaceSessionRequest>) {
            const auto previous = sessions.snapshot(body.sessionId);
            const auto owner = previous ? previous->owner : std::string{};
            std::vector<PreparedPresentationHandoff> prepared;
            if (previous) {
                auto preparation = handoffs.prepare(
                    *previous,
                    body.replacement,
                    readiness,
                    nowMs);
                if (!preparation)
                    return Result<json>::failure(preparation.error());
                prepared = std::move(preparation.value());
            }
            auto visibilityPreparation = visibility.prepare(
                previous,
                body.replacement,
                owner,
                nowMs);
            if (!visibilityPreparation)
                return Result<json>::failure(
                    visibilityPreparation.error());
            auto replaced = sessions.replace(
                body.sessionId,
                body.token,
                body.replacement,
                configMaterialNames(config),
                nowMs);
            if (!replaced)
                return Result<json>::failure(replaced.error());

            handoffs.commit(
                replaced.value().owner,
                replaced.value().generation,
                prepared,
                nowMs);
            visibility.commit(
                std::move(visibilityPreparation.value()));
            for (const auto& record : visibility.records()) {
                if (record.identity.owner != replaced.value().owner)
                    continue;
                const auto retained = std::ranges::any_of(
                    replaced.value().targets,
                    [&](const Target& target) {
                        return target.id == record.identity.targetId;
                    });
                if (!retained)
                    visibility.erase(record.identity);
            }

            if (previous)
                for (const auto& target : previous->targets)
                    readiness.erase({.owner = previous->owner, .targetId = target.id});
            for (const auto& target : replaced.value().targets) {
                const auto accepted = readiness.accept({
                    .owner = replaced.value().owner,
                    .targetId = target.id,
                });
                if (!accepted)
                    return Result<json>::failure(accepted.error());
            }
            json retained = json::array();
            for (const auto& item : prepared) {
                const auto record = handoffs.target(item.identity);
                if (record)
                    retained.push_back(handoffJson(*record));
            }
            json acceptedVisibility = json::array();
            for (const auto& request :
                 body.replacement.visibilityTransitions) {
                const auto record = visibility.target({
                    .owner = replaced.value().owner,
                    .targetId = request.targetId,
                });
                if (record)
                    acceptedVisibility.push_back(
                        visibilityJson(*record));
            }
            return Result<json>::success({
                {"owner", replaced.value().owner},
                {"generation", replaced.value().generation},
                {"expires_at_ms", replaced.value().expiresAtMs},
                {"materials", replaced.value().materials.size()},
                {"targets", replaced.value().targets.size()},
                {"handoffs", std::move(retained)},
                {"visibility_transitions",
                 std::move(acceptedVisibility)},
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
            if (previous)
                handoffs.eraseOwner(previous->owner);
            if (previous)
                visibility.eraseOwner(previous->owner);
            return Result<json>::success({{"closed", true}});
        } else {
            return inspectTarget(
                sessions, readiness, handoffs, visibility, body, nowMs);
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
                                      m_readiness, m_handoffs, m_visibility,
                                      m_rendererStatus, nowMs);
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
        m_handoffs.expire(nowMs);
        m_visibility.expire(nowMs);
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

PresentationHandoffTracker& RuntimeService::handoffTracker() noexcept {
    return m_handoffs;
}

const PresentationHandoffTracker& RuntimeService::handoffTracker() const noexcept {
    return m_handoffs;
}

VisibilityTransitionTracker& RuntimeService::visibilityTracker() noexcept {
    return m_visibility;
}

const VisibilityTransitionTracker&
RuntimeService::visibilityTracker() const noexcept {
    return m_visibility;
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
        m_handoffs.eraseOwner(expired.owner);
        m_visibility.eraseOwner(expired.owner);
    }
}

} // namespace hfg::v2
