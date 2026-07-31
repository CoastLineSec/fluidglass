#include "v2/model/VisibilityTransition.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <set>

namespace hfg::v2 {
namespace {

std::uint64_t deadline(std::uint64_t nowMs, std::uint64_t timeoutMs) {
    if (timeoutMs > UINT64_MAX - nowMs)
        return UINT64_MAX;
    return nowMs + timeoutMs;
}

Point outward(TransitionEdge edge, double travel) {
    switch (edge) {
        case TransitionEdge::Top: return {.x = 0.0, .y = -travel};
        case TransitionEdge::Bottom: return {.x = 0.0, .y = travel};
        case TransitionEdge::Left: return {.x = -travel, .y = 0.0};
        case TransitionEdge::Right: return {.x = travel, .y = 0.0};
    }
    return {};
}

bool finiteRect(const Rect& rect) {
    return std::isfinite(rect.x) && std::isfinite(rect.y) &&
        std::isfinite(rect.width) && std::isfinite(rect.height) &&
        rect.width > 0.0 && rect.height > 0.0;
}

} // namespace

std::string_view visibilityTransitionStateName(
    VisibilityTransitionState state) noexcept {
    switch (state) {
        case VisibilityTransitionState::Armed: return "armed";
        case VisibilityTransitionState::Active: return "active";
        case VisibilityTransitionState::Completed: return "completed";
        case VisibilityTransitionState::Failed: return "failed";
        case VisibilityTransitionState::Superseded: return "superseded";
    }
    return "failed";
}

std::string_view visibilityTransitionDirectionName(
    VisibilityTransitionDirection direction) noexcept {
    return direction == VisibilityTransitionDirection::Hide
        ? "hide"
        : "reveal";
}

Result<std::vector<PreparedVisibilityTransition>>
VisibilityTransitionTracker::prepare(
    const std::optional<SessionSnapshot>& previous,
    const SessionReplacement& replacement,
    std::string_view owner,
    std::uint64_t nowMs) const {
    std::vector<PreparedVisibilityTransition> result;
    std::set<std::string_view> targetIds;
    for (const auto& request : replacement.visibilityTransitions) {
        if (!targetIds.insert(request.targetId).second)
            return Result<std::vector<PreparedVisibilityTransition>>::failure({
                ErrorCode::InvalidRequest,
                "visibility_transitions",
                "visibility transition target ids must be unique",
            });
        if (!previous ||
            request.sourceGeneration != previous->generation)
            return Result<std::vector<PreparedVisibilityTransition>>::failure({
                ErrorCode::StaleGeneration,
                "visibility_transitions.source_generation",
                "visibility transition source generation is not current",
            });
        if (request.durationMs == 0 ||
            request.durationMs > Limits::MAX_VISIBILITY_TRANSITION_MS ||
            request.timeoutMs == 0 ||
            request.timeoutMs > Limits::MAX_PRESENTATION_HANDOFF_MS)
            return Result<std::vector<PreparedVisibilityTransition>>::failure({
                ErrorCode::InvalidRequest,
                "visibility_transitions.duration_ms",
                "visibility transition timing is outside supported limits",
            });
        if (!finiteRect(request.sourceRect) ||
            !std::isfinite(request.sourceRadius) ||
            request.sourceRadius < 0.0 ||
            !std::isfinite(request.travel) || request.travel <= 0.0)
            return Result<std::vector<PreparedVisibilityTransition>>::failure({
                ErrorCode::InvalidTarget,
                "visibility_transitions.source",
                "visibility transition geometry is invalid",
            });
        const auto successor = std::ranges::find(
            replacement.targets, request.targetId, &Target::id);
        if (successor == replacement.targets.end() ||
            successor->kind != TargetKind::Layer)
            return Result<std::vector<PreparedVisibilityTransition>>::failure({
                ErrorCode::InvalidTarget,
                "visibility_transitions.target_id",
                "visibility transition requires a successor layer target",
            });
        const auto* selector =
            std::get_if<LayerSelector>(&successor->selector);
        if (!selector ||
            selector->namespaceName != request.namespaceName)
            return Result<std::vector<PreparedVisibilityTransition>>::failure({
                ErrorCode::InvalidTarget,
                "visibility_transitions.namespace",
                "visibility transition namespace does not match its target",
            });

        VisibilityTransitionRecord record{
            .identity = {
                .owner = std::string(owner),
                .targetId = request.targetId,
            },
            .transitionId = request.transitionId,
            .sourceGeneration = request.sourceGeneration,
            .successorGeneration = replacement.generation,
            .direction = request.direction,
            .state = VisibilityTransitionState::Armed,
            .sourceRect = request.sourceRect,
            .sourceRadius = request.sourceRadius,
            .sourceOffset = {},
            .destinationOffset = {},
            .sourceOpacity =
                request.direction == VisibilityTransitionDirection::Hide
                    ? 1.0 : 0.0,
            .destinationOpacity =
                request.direction == VisibilityTransitionDirection::Hide
                    ? 0.0 : 1.0,
            .anchorMs = 0,
            .durationMs = request.durationMs,
            .expiresAtMs = deadline(nowMs, request.timeoutMs),
            .output = request.output,
            .namespaceName = request.namespaceName,
            .detail = "waiting for first successful successor draw",
        };

        const auto hidden = outward(request.edge, request.travel);
        if (request.direction == VisibilityTransitionDirection::Hide)
            record.destinationOffset = hidden;
        else
            record.sourceOffset = hidden;

        const auto existing = m_records.find(record.identity);
        if (existing != m_records.end() &&
            (existing->second.state == VisibilityTransitionState::Armed ||
             existing->second.state == VisibilityTransitionState::Active)) {
            const auto current = sampleRecord(existing->second, nowMs);
            record.sourceOffset = current.offset;
            record.sourceOpacity = current.opacity;
            record.startingProgress = current.progress;
            record.detail = "superseded transition starts from current presentation";
        }
        result.push_back({.record = std::move(record)});
    }
    return Result<std::vector<PreparedVisibilityTransition>>::success(
        std::move(result));
}

void VisibilityTransitionTracker::commit(
    std::vector<PreparedVisibilityTransition> prepared) {
    for (auto& item : prepared) {
        if (const auto existing = m_records.find(item.record.identity);
            existing != m_records.end()) {
            existing->second.state = VisibilityTransitionState::Superseded;
            existing->second.detail = "superseded by a newer transition";
        }
        m_records.insert_or_assign(
            item.record.identity, std::move(item.record));
    }
}

std::optional<VisibilityTransitionRecord>
VisibilityTransitionTracker::target(
    const TargetIdentity& identity) const {
    const auto found = m_records.find(identity);
    return found == m_records.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::vector<VisibilityTransitionRecord>
VisibilityTransitionTracker::records() const {
    std::vector<VisibilityTransitionRecord> result;
    for (const auto& [identity, record] : m_records) {
        static_cast<void>(identity);
        result.push_back(record);
    }
    return result;
}

VisibilityTransitionSample VisibilityTransitionTracker::sampleRecord(
    const VisibilityTransitionRecord& record,
    std::uint64_t nowMs) {
    if (record.state == VisibilityTransitionState::Armed)
        return {
            .offset =
                record.direction == VisibilityTransitionDirection::Reveal &&
                    record.startingProgress == 0.0 &&
                    record.sourceOpacity == 0.0
                    ? record.destinationOffset
                    : record.sourceOffset,
            .opacity = record.sourceOpacity,
            .progress = 0.0,
            .active = true,
        };
    const auto elapsed = nowMs > record.anchorMs
        ? std::min(nowMs - record.anchorMs, record.durationMs)
        : 0U;
    const auto linear = record.durationMs == 0
        ? 1.0
        : static_cast<double>(elapsed) /
            static_cast<double>(record.durationMs);
    const auto progress = 1.0 - std::pow(1.0 - linear, 3.0);
    return {
        .offset = {
            .x = record.sourceOffset.x +
                (record.destinationOffset.x - record.sourceOffset.x) *
                    progress,
            .y = record.sourceOffset.y +
                (record.destinationOffset.y - record.sourceOffset.y) *
                    progress,
        },
        .opacity = record.sourceOpacity +
            (record.destinationOpacity - record.sourceOpacity) * progress,
        .progress = linear,
        .active = elapsed < record.durationMs,
    };
}

Result<VisibilityTransitionSample> VisibilityTransitionTracker::sample(
    const TargetIdentity& identity,
    std::uint64_t nowMs) const {
    const auto found = m_records.find(identity);
    if (found == m_records.end())
        return Result<VisibilityTransitionSample>::failure({
            ErrorCode::InvalidTarget,
            "visibility_transition",
            "visibility transition was not found",
        });
    return Result<VisibilityTransitionSample>::success(
        sampleRecord(found->second, nowMs));
}

bool VisibilityTransitionTracker::bind(
    const TargetIdentity& identity,
    std::string_view output,
    std::uint64_t outputGeneration,
    std::uint64_t objectToken) {
    const auto found = m_records.find(identity);
    if (found == m_records.end())
        return false;
    auto& record = found->second;
    if (record.output != output) {
        fail(identity, "resolved output differs from requested output");
        return false;
    }
    if ((record.outputGeneration &&
         *record.outputGeneration != outputGeneration) ||
        (record.objectToken && *record.objectToken != objectToken)) {
        fail(identity, "surface or output lifetime changed");
        return false;
    }
    record.outputGeneration = outputGeneration;
    record.objectToken = objectToken;
    return true;
}

bool VisibilityTransitionTracker::activate(
    const PresentationKey& key,
    std::uint64_t nowMs) {
    const auto found = m_records.find(key.identity);
    if (found == m_records.end() ||
        found->second.state != VisibilityTransitionState::Armed)
        return false;
    auto& record = found->second;
    if (!record.outputGeneration ||
        record.output != key.output ||
        *record.outputGeneration != key.outputGeneration)
        return false;
    record.state = VisibilityTransitionState::Active;
    record.anchorMs = nowMs;
    record.detail = "anchored to first successful successor draw";
    return true;
}

void VisibilityTransitionTracker::fail(
    const TargetIdentity& identity,
    std::string detail) {
    const auto found = m_records.find(identity);
    if (found == m_records.end())
        return;
    found->second.state = VisibilityTransitionState::Failed;
    found->second.detail = std::move(detail);
}

void VisibilityTransitionTracker::eraseOwner(std::string_view owner) {
    std::erase_if(m_records, [&](const auto& item) {
        return item.first.owner == owner;
    });
}

void VisibilityTransitionTracker::erase(
    const TargetIdentity& identity) {
    m_records.erase(identity);
}

void VisibilityTransitionTracker::expire(std::uint64_t nowMs) {
    for (auto& [identity, record] : m_records) {
        static_cast<void>(identity);
        if ((record.state == VisibilityTransitionState::Armed ||
             record.state == VisibilityTransitionState::Active) &&
            nowMs >= record.expiresAtMs) {
            record.state = VisibilityTransitionState::Failed;
            record.detail = "visibility transition timeout expired";
        } else if (record.state == VisibilityTransitionState::Active) {
            const auto current = sampleRecord(record, nowMs);
            if (!current.active) {
                record.state = VisibilityTransitionState::Completed;
                record.detail = "visibility transition completed";
            }
        }
    }
}

void VisibilityTransitionTracker::clear() {
    m_records.clear();
}

} // namespace hfg::v2
