#include "v2/model/PresentationHandoff.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
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

} // namespace

Result<std::vector<PreparedPresentationHandoff>>
PresentationHandoffTracker::prepare(
    const SessionSnapshot& current,
    const SessionReplacement& replacement,
    const ReadinessTracker& readiness) const {
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
        const auto presentations = readiness.presentations(identity);
        if (presentations.empty() ||
            std::ranges::any_of(presentations, [](const auto& entry) {
                return entry.second.state != ReadinessState::Drawn;
            }))
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
        };
        item.presentations.reserve(presentations.size());
        for (const auto& [key, record] : presentations) {
            static_cast<void>(record);
            item.presentations.push_back(key);
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
    eraseOwner(owner);
    for (const auto& item : prepared) {
        PresentationHandoffRecord record{
            .identity = item.identity,
            .sourceGeneration = item.sourceGeneration,
            .successorGeneration = successorGeneration,
            .expiresAtMs = deadline(nowMs, item.timeoutMs),
            .presentations = {},
        };
        record.presentations.reserve(item.presentations.size());
        for (const auto& key : item.presentations)
            record.presentations.push_back({
                .key = key,
                .state = PresentationHandoffState::Retained,
                .detail = {},
            });
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
}

void PresentationHandoffTracker::expire(std::uint64_t nowMs) {
    for (auto& [identity, record] : m_records) {
        static_cast<void>(identity);
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

} // namespace hfg::v2
