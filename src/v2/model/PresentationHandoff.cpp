#include "v2/model/PresentationHandoff.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

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

const Target* targetWithId(
    const std::vector<Target>& targets,
    std::string_view id) {
    const auto found = std::ranges::find(targets, id, &Target::id);
    return found == targets.end() ? nullptr : &*found;
}

std::uint64_t deadline(std::uint64_t nowMs, std::uint64_t timeoutMs) {
    if (timeoutMs > std::numeric_limits<std::uint64_t>::max() - nowMs)
        return std::numeric_limits<std::uint64_t>::max();
    return nowMs + timeoutMs;
}

const RoundedRectShape* roundedShape(const Target& target) {
    return std::get_if<RoundedRectShape>(&target.shape);
}

Rect envelope(const Rect& left, const Rect& right) {
    const auto x = std::min(left.x, right.x);
    const auto y = std::min(left.y, right.y);
    const auto farX = std::max(left.x + left.width, right.x + right.width);
    const auto farY = std::max(left.y + left.height, right.y + right.height);
    return {
        .x = x,
        .y = y,
        .width = farX - x,
        .height = farY - y,
    };
}

double easeOutCubic(double progress) {
    const auto remaining = 1.0 - std::clamp(progress, 0.0, 1.0);
    return 1.0 - remaining * remaining * remaining;
}

double interpolate(double source, double destination, double progress) {
    return std::round(source + (destination - source) * progress);
}

double endpointDistance(
    const PresentationMorphEndpoint& source,
    const PresentationMorphEndpoint& destination) {
    return std::max({
        std::abs(source.rect.x - destination.rect.x),
        std::abs(source.rect.y - destination.rect.y),
        std::abs(source.rect.width - destination.rect.width),
        std::abs(source.rect.height - destination.rect.height),
        std::abs(source.radius - destination.radius),
    });
}

std::optional<PresentationMorphEndpoint> targetEndpoint(const Target& target) {
    const auto* shape = roundedShape(target);
    if (!target.geometry || !shape)
        return std::nullopt;
    return PresentationMorphEndpoint{
        .rect = *target.geometry,
        .radius = shape->radius,
    };
}

bool validMorphEndpoint(const PresentationMorphEndpoint& endpoint) {
    return std::isfinite(endpoint.rect.x) &&
        std::isfinite(endpoint.rect.y) &&
        std::isfinite(endpoint.rect.width) &&
        std::isfinite(endpoint.rect.height) &&
        endpoint.rect.width > 0.0 &&
        endpoint.rect.height > 0.0 &&
        std::isfinite(endpoint.radius) &&
        endpoint.radius >= 0.0 &&
        endpoint.radius <=
            std::min(endpoint.rect.width, endpoint.rect.height) / 2.0;
}

PresentationMorphEndpoint endpointFromRequest(
    const PresentationHandoffRequest::MorphEndpoint& endpoint) {
    return {
        .rect = endpoint.rect,
        .radius = endpoint.radius,
    };
}

} // namespace

Result<ResolvedPresentationMorph> resolvePresentationMorph(
    const PresentationMorphRecord& morph,
    std::uint64_t nowMs) {
    if (morph.durationMs == 0U)
        return failure<ResolvedPresentationMorph>(
            ErrorCode::InvalidRequest,
            "morph.duration_ms",
            "morph duration must not be zero");
    const auto elapsed = nowMs > morph.anchorMs
        ? nowMs - morph.anchorMs
        : 0U;
    const auto linear = std::min(
        1.0,
        static_cast<double>(elapsed) /
            static_cast<double>(morph.durationMs));
    const auto progress = easeOutCubic(linear);
    const auto& source = morph.source;
    const auto& destination = morph.destination;
    return Result<ResolvedPresentationMorph>::success({
        .current = {
            .rect = {
                .x = interpolate(source.rect.x, destination.rect.x, progress),
                .y = interpolate(source.rect.y, destination.rect.y, progress),
                .width = interpolate(source.rect.width, destination.rect.width,
                                     progress),
                .height = interpolate(source.rect.height,
                                      destination.rect.height, progress),
            },
            .radius = interpolate(source.radius, destination.radius, progress),
        },
        .envelope = morph.envelope,
        .progress = linear,
        .active = morph.state == PresentationMorphState::Active &&
            linear < 1.0,
    });
}

Result<std::vector<PreparedPresentationHandoff>>
PresentationHandoffTracker::prepare(
    const SessionSnapshot& current,
    const SessionReplacement& replacement,
    const ReadinessTracker& readiness,
    std::uint64_t nowMs) const {
    std::vector<PreparedPresentationHandoff> prepared;
    prepared.reserve(replacement.handoffs.size());
    std::set<std::string_view> targetIds;
    for (std::size_t index = 0; index < replacement.handoffs.size(); ++index) {
        const auto& request = replacement.handoffs[index];
        const auto path = "handoffs[" + std::to_string(index) + "]";
        if (!targetIds.insert(request.targetId).second)
            return failure<std::vector<PreparedPresentationHandoff>>(
                ErrorCode::InvalidRequest,
                path + ".target_id",
                "handoff target ids must be unique");
        if (request.sourceGeneration != current.generation)
            return failure<std::vector<PreparedPresentationHandoff>>(
                ErrorCode::StaleGeneration,
                path + ".source_generation",
                "handoff source generation is not current");
        if (request.timeoutMs == 0U ||
            request.timeoutMs > Limits::MAX_PRESENTATION_HANDOFF_MS)
            return failure<std::vector<PreparedPresentationHandoff>>(
                ErrorCode::InvalidRequest,
                path + ".timeout_ms",
                "handoff timeout is outside the supported range");

        const auto* previous = targetWithId(current.targets, request.targetId);
        const auto* successor = targetWithId(replacement.targets, request.targetId);
        if (!previous || !successor)
            return failure<std::vector<PreparedPresentationHandoff>>(
                ErrorCode::UnresolvedTarget,
                path + ".target_id",
                "handoff requires the target in both generations");
        if (previous->kind != TargetKind::Layer ||
            successor->kind != TargetKind::Layer)
            return failure<std::vector<PreparedPresentationHandoff>>(
                ErrorCode::UnsupportedTarget,
                path + ".target_id",
                "presentation handoff currently supports layer targets");
        const auto* previousLayer = std::get_if<LayerSelector>(&previous->selector);
        const auto* successorLayer = std::get_if<LayerSelector>(&successor->selector);
        if (!previousLayer || !successorLayer ||
            previousLayer->namespaceName != successorLayer->namespaceName ||
            !previous->enabled || !successor->enabled)
            return failure<std::vector<PreparedPresentationHandoff>>(
                ErrorCode::UnresolvedTarget,
                path + ".target_id",
                "handoff requires the same enabled layer surface");

        const TargetIdentity identity{
            .owner = current.owner,
            .targetId = request.targetId,
        };
        const auto existing = m_records.find(identity);
        const auto existingMorphActive =
            existing != m_records.end() &&
            existing->second.successorGeneration == current.generation &&
            existing->second.morph &&
            (existing->second.morph->state ==
                 PresentationMorphState::Active ||
             existing->second.morph->state ==
                 PresentationMorphState::Settling);
        const auto retainedFallbackAvailable =
            existing != m_records.end() &&
            existing->second.successorGeneration == current.generation &&
            std::ranges::any_of(
                existing->second.presentations,
                [](const PresentationHandoffPresentation& presentation) {
                    return presentation.state ==
                        PresentationHandoffState::Retained;
                });
        const auto presentations = readiness.presentations(identity);
        const auto fullyDrawn =
            !presentations.empty() &&
            std::ranges::none_of(presentations, [](const auto& entry) {
                return entry.second.state != ReadinessState::Drawn;
            });
        if (!fullyDrawn && !retainedFallbackAvailable)
            return failure<std::vector<PreparedPresentationHandoff>>(
                ErrorCode::UnresolvedTarget,
                path + ".target_id",
                "handoff predecessor is not fully drawn");
        if (presentations.size() > Limits::MAX_PRESENTATIONS_PER_TARGET)
            return failure<std::vector<PreparedPresentationHandoff>>(
                ErrorCode::ResourceLimited,
                path + ".target_id",
                "handoff presentation limit exceeded");

        PreparedPresentationHandoff item{
            .identity = identity,
            .sourceGeneration = request.sourceGeneration,
            .timeoutMs = request.timeoutMs,
            .presentations = {},
            .morph = std::nullopt,
            .preserveActiveMorph = false,
        };
        item.presentations.reserve(
            fullyDrawn
                ? presentations.size()
                : existing->second.presentations.size());
        if (fullyDrawn) {
            for (const auto& [key, record] : presentations) {
                static_cast<void>(record);
                item.presentations.push_back(key);
            }
        } else {
            for (const auto& presentation :
                 existing->second.presentations)
                if (presentation.state ==
                    PresentationHandoffState::Retained)
                    item.presentations.push_back(presentation.key);
        }
        if (request.morph) {
            if (request.morph->transitionId.empty() ||
                request.morph->transitionId.size() >
                    Limits::MAX_IDENTIFIER_BYTES)
                return failure<std::vector<PreparedPresentationHandoff>>(
                    ErrorCode::InvalidRequest,
                    path + ".morph.transition_id",
                    "morph transition id is invalid");
            if (request.morph->durationMs == 0U ||
                request.morph->durationMs >
                    Limits::MAX_PRESENTATION_MORPH_MS ||
                request.morph->durationMs > request.timeoutMs)
                return failure<std::vector<PreparedPresentationHandoff>>(
                    ErrorCode::InvalidRequest,
                    path + ".morph.duration_ms",
                    "morph duration is outside the supported handoff range");
            std::optional<PresentationMorphEndpoint> destination;
            std::optional<PresentationMorphEndpoint> source;
            if (request.morph->coordinateSpace ==
                PresentationHandoffRequest::MorphCoordinateSpace::OutputLocal) {
                if (!request.morph->source ||
                    !request.morph->destination)
                    return failure<std::vector<PreparedPresentationHandoff>>(
                        ErrorCode::InvalidRequest,
                        path + ".morph",
                        "output-local morph requires source and destination endpoints");
                source = endpointFromRequest(*request.morph->source);
                destination =
                    endpointFromRequest(*request.morph->destination);
                const auto previousEndpoint = targetEndpoint(*previous);
                const auto successorEndpoint = targetEndpoint(*successor);
                if (!previousEndpoint || !successorEndpoint ||
                    source->rect.width != previousEndpoint->rect.width ||
                    source->rect.height != previousEndpoint->rect.height ||
                    source->radius != previousEndpoint->radius ||
                    destination->rect.width !=
                        successorEndpoint->rect.width ||
                    destination->rect.height !=
                        successorEndpoint->rect.height ||
                    destination->radius != successorEndpoint->radius)
                    return failure<std::vector<PreparedPresentationHandoff>>(
                        ErrorCode::InvalidTarget,
                        path + ".morph",
                        "output-local endpoints must match the source and destination target size and radius");
            } else {
                destination = targetEndpoint(*successor);
                source = targetEndpoint(*previous);
                if (request.morph->source ||
                    request.morph->destination)
                    return failure<std::vector<PreparedPresentationHandoff>>(
                        ErrorCode::InvalidRequest,
                        path + ".morph",
                        "surface-local morph derives its endpoints from the targets");
            }
            const auto fullSource = source;
            if (!source || !destination ||
                !validMorphEndpoint(*source) ||
                !validMorphEndpoint(*destination))
                return failure<std::vector<PreparedPresentationHandoff>>(
                    ErrorCode::UnsupportedTarget,
                    path + ".morph",
                    "geometry morph requires valid uniform rounded rectangles");
            if (existing != m_records.end() &&
                existing->second.successorGeneration == current.generation &&
                existing->second.morph &&
                existing->second.morph->coordinateSpace ==
                    request.morph->coordinateSpace &&
                (existing->second.morph->state ==
                     PresentationMorphState::Active ||
                 existing->second.morph->state ==
                     PresentationMorphState::Settling)) {
                auto visible = resolvePresentationMorph(
                    *existing->second.morph,
                    nowMs);
                if (!visible)
                    return failure<std::vector<PreparedPresentationHandoff>>(
                        visible.error().code,
                        path + "." + visible.error().path,
                        visible.error().message);
                source = visible.value().current;
            }
            auto effectiveDuration = request.morph->durationMs;
            if (fullSource && source != fullSource) {
                const auto fullDistance =
                    endpointDistance(*fullSource, *destination);
                const auto remainingDistance =
                    endpointDistance(*source, *destination);
                if (fullDistance > 0.0)
                    effectiveDuration = std::max<std::uint64_t>(
                        1U,
                        static_cast<std::uint64_t>(std::llround(
                            static_cast<double>(effectiveDuration) *
                            std::clamp(
                                remainingDistance / fullDistance,
                                0.0,
                                1.0))));
            }
            item.morph = PreparedPresentationMorph{
                .transitionId = request.morph->transitionId,
                .coordinateSpace = request.morph->coordinateSpace,
                .source = *source,
                .destination = *destination,
                .durationMs = effectiveDuration,
            };
        } else if (existingMorphActive) {
            if (existing->second.morph->coordinateSpace ==
                PresentationHandoffRequest::MorphCoordinateSpace::OutputLocal)
                item.preserveActiveMorph = true;
            else {
                const auto destination = targetEndpoint(*successor);
                item.preserveActiveMorph =
                    destination &&
                    *destination == existing->second.morph->destination;
            }
        }
        prepared.push_back(std::move(item));
    }
    return Result<std::vector<PreparedPresentationHandoff>>::success(
        std::move(prepared));
}

void PresentationHandoffTracker::commit(
    std::string_view owner,
    std::uint64_t successorGeneration,
    std::span<const PreparedPresentationHandoff> prepared,
    std::uint64_t nowMs) {
    std::map<TargetIdentity, PresentationMorphRecord> preservedMorphs;
    for (const auto& item : prepared) {
        if (!item.preserveActiveMorph)
            continue;
        const auto existing = m_records.find(item.identity);
        if (existing != m_records.end() &&
            existing->second.morph &&
            (existing->second.morph->state ==
                 PresentationMorphState::Active ||
             existing->second.morph->state ==
                 PresentationMorphState::Settling))
            preservedMorphs.insert_or_assign(
                item.identity,
                *existing->second.morph);
    }
    eraseOwner(owner);
    for (const auto& item : prepared) {
        PresentationHandoffRecord record{
            .identity = item.identity,
            .sourceGeneration = item.sourceGeneration,
            .successorGeneration = successorGeneration,
            .expiresAtMs = deadline(nowMs, item.timeoutMs),
            .presentations = {},
            .morph = std::nullopt,
        };
        record.presentations.reserve(item.presentations.size());
        for (const auto& key : item.presentations)
            record.presentations.push_back({
                .key = key,
                .state = PresentationHandoffState::Retained,
                .detail = {},
            });
        if (item.morph)
            record.morph = PresentationMorphRecord{
                .transitionId = item.morph->transitionId,
                .coordinateSpace = item.morph->coordinateSpace,
                .source = item.morph->source,
                .destination = item.morph->destination,
                .envelope = envelope(
                    item.morph->source.rect,
                    item.morph->destination.rect),
                .anchorMs = nowMs,
                .durationMs = item.morph->durationMs,
                .state = PresentationMorphState::Active,
                .detail = {},
            };
        else if (const auto preserved =
                     preservedMorphs.find(item.identity);
                 preserved != preservedMorphs.end())
            record.morph = preserved->second;
        m_records.insert_or_assign(record.identity, std::move(record));
    }
}

void PresentationHandoffTracker::complete(const PresentationKey& key) {
    const auto found = m_records.find(key.identity);
    if (found == m_records.end())
        return;
    for (auto& presentation : found->second.presentations)
        if (presentation.key == key &&
            presentation.state == PresentationHandoffState::Retained) {
            presentation.state = PresentationHandoffState::Completed;
            presentation.detail.clear();
        }
}

void PresentationHandoffTracker::fail(
    const PresentationKey& key,
    std::string detail) {
    const auto found = m_records.find(key.identity);
    if (found == m_records.end())
        return;
    for (auto& presentation : found->second.presentations)
        if (presentation.key == key &&
            presentation.state == PresentationHandoffState::Retained) {
            presentation.state = PresentationHandoffState::Failed;
            presentation.detail = detail;
        }
    if (found->second.morph) {
        found->second.morph->state = PresentationMorphState::Failed;
        found->second.morph->detail = detail;
    }
}

void PresentationHandoffTracker::fail(
    const TargetIdentity& identity,
    std::string detail) {
    const auto found = m_records.find(identity);
    if (found == m_records.end())
        return;
    for (auto& presentation : found->second.presentations)
        if (presentation.state == PresentationHandoffState::Retained) {
            presentation.state = PresentationHandoffState::Failed;
            presentation.detail = detail;
        }
    if (found->second.morph) {
        found->second.morph->state = PresentationMorphState::Failed;
        found->second.morph->detail = detail;
    }
}

void PresentationHandoffTracker::settleMorph(
    const TargetIdentity& identity) {
    const auto found = m_records.find(identity);
    if (found == m_records.end() || !found->second.morph)
        return;
    auto& morph = *found->second.morph;
    if (morph.state != PresentationMorphState::Active &&
        morph.state != PresentationMorphState::Settling)
        return;
    morph.state = PresentationMorphState::Completed;
    morph.detail.clear();
}

void PresentationHandoffTracker::expire(std::uint64_t nowMs) {
    for (auto& [identity, record] : m_records) {
        static_cast<void>(identity);
        if (record.morph &&
            (record.morph->state == PresentationMorphState::Active ||
             record.morph->state == PresentationMorphState::Settling)) {
            if (record.expiresAtMs <= nowMs) {
                record.morph->state = PresentationMorphState::Failed;
                record.morph->detail = "handoff timeout expired";
            } else if (deadline(record.morph->anchorMs,
                                record.morph->durationMs) <= nowMs) {
                if (record.morph->coordinateSpace ==
                    PresentationHandoffRequest::MorphCoordinateSpace::OutputLocal) {
                    record.morph->state =
                        PresentationMorphState::Settling;
                    record.morph->detail =
                        "waiting for the layer attachment to settle";
                } else {
                    record.morph->state =
                        PresentationMorphState::Completed;
                    record.morph->detail.clear();
                }
            }
        }
        if (record.expiresAtMs > nowMs)
            continue;
        for (auto& presentation : record.presentations)
            if (presentation.state == PresentationHandoffState::Retained) {
                presentation.state = PresentationHandoffState::Failed;
                presentation.detail = "handoff timeout expired";
            }
    }
}

void PresentationHandoffTracker::eraseOwner(std::string_view owner) {
    std::erase_if(m_records, [&](const auto& entry) {
        return entry.first.owner == owner;
    });
}

void PresentationHandoffTracker::clear() noexcept {
    m_records.clear();
}

std::optional<PresentationHandoffRecord>
PresentationHandoffTracker::target(
    const TargetIdentity& identity) const {
    const auto found = m_records.find(identity);
    return found == m_records.end()
        ? std::nullopt
        : std::optional<PresentationHandoffRecord>{found->second};
}

std::vector<PresentationHandoffRecord>
PresentationHandoffTracker::active() const {
    std::vector<PresentationHandoffRecord> result;
    for (const auto& [identity, record] : m_records) {
        static_cast<void>(identity);
        if (std::ranges::any_of(record.presentations, [](const auto& item) {
                return item.state == PresentationHandoffState::Retained;
            }))
            result.push_back(record);
    }
    return result;
}

std::vector<PresentationHandoffRecord>
PresentationHandoffTracker::morphing() const {
    std::vector<PresentationHandoffRecord> result;
    for (const auto& [identity, record] : m_records) {
        static_cast<void>(identity);
        if (record.morph &&
            (record.morph->state == PresentationMorphState::Active ||
             record.morph->state == PresentationMorphState::Settling))
            result.push_back(record);
    }
    return result;
}

} // namespace hfg::v2
