#include "v2/model/Readiness.hpp"

#include <algorithm>
#include <utility>

namespace hfg::v2 {
namespace {

Result<ReadinessRecord> failure(std::string path, std::string message) {
    return Result<ReadinessRecord>::failure({
        .code = ErrorCode::InvalidRequest,
        .path = std::move(path),
        .message = std::move(message),
    });
}

} // namespace

Result<ReadinessRecord> ReadinessTracker::accept(TargetIdentity identity) {
    if (identity.owner.empty() || identity.targetId.empty())
        return failure("identity", "owner and target id must not be empty");

    erase(identity);
    auto record = nextRecord(ReadinessState::Accepted, {});
    m_targets.emplace(std::move(identity), record);
    return Result<ReadinessRecord>::success(record);
}

Result<ReadinessRecord> ReadinessTracker::failTarget(
    const TargetIdentity& identity,
    ReadinessState state,
    std::string detail) {
    const auto targetRecord = m_targets.find(identity);
    if (targetRecord == m_targets.end())
        return failure("identity", "target has not been accepted");
    if (!validTargetFailure(state))
        return failure("state", "state is not a target-level failure");

    auto record = nextRecord(state, std::move(detail));
    targetRecord->second = record;
    for (auto& [key, presentationRecord] : m_presentations)
        if (key.identity == identity)
            presentationRecord = record;
    return Result<ReadinessRecord>::success(record);
}

Result<ReadinessRecord> ReadinessTracker::resolvePresentation(PresentationKey key) {
    const auto targetRecord = m_targets.find(key.identity);
    if (targetRecord == m_targets.end())
        return failure("identity", "target has not been accepted");
    if (key.output.empty())
        return failure("output", "presentation output must not be empty");

    auto record = nextRecord(ReadinessState::Resolved, {});
    m_presentations.insert_or_assign(std::move(key), record);
    return Result<ReadinessRecord>::success(record);
}

Result<ReadinessRecord> ReadinessTracker::transition(
    const PresentationKey& key,
    ReadinessState state,
    std::string detail) {
    const auto record = m_presentations.find(key);
    if (record == m_presentations.end())
        return failure("presentation", "presentation has not been resolved");
    if (!validPresentationTransition(record->second.state, state))
        return failure(
            "state",
            "invalid readiness transition from " + std::string(readinessStateName(record->second.state)) +
                " to " + std::string(readinessStateName(state)));

    record->second = nextRecord(state, std::move(detail));
    return Result<ReadinessRecord>::success(record->second);
}

std::optional<ReadinessRecord> ReadinessTracker::target(const TargetIdentity& identity) const {
    const auto record = m_targets.find(identity);
    if (record == m_targets.end())
        return std::nullopt;
    return record->second;
}

std::optional<ReadinessRecord> ReadinessTracker::presentation(const PresentationKey& key) const {
    const auto record = m_presentations.find(key);
    if (record == m_presentations.end())
        return std::nullopt;
    return record->second;
}

std::vector<std::pair<PresentationKey, ReadinessRecord>> ReadinessTracker::presentations(
    const TargetIdentity& identity) const {
    std::vector<std::pair<PresentationKey, ReadinessRecord>> result;
    for (const auto& entry : m_presentations)
        if (entry.first.identity == identity)
            result.push_back(entry);
    return result;
}

void ReadinessTracker::erase(const TargetIdentity& identity) {
    m_targets.erase(identity);
    std::erase_if(m_presentations, [&](const auto& entry) {
        return entry.first.identity == identity;
    });
}

ReadinessRecord ReadinessTracker::nextRecord(ReadinessState state, std::string detail) {
    return {
        .state = state,
        .sequence = ++m_sequence,
        .detail = std::move(detail),
    };
}

bool ReadinessTracker::validPresentationTransition(ReadinessState from, ReadinessState to) {
    if (from == to)
        return true;
    if (to == ReadinessState::Expired || to == ReadinessState::Detached)
        return from != ReadinessState::Expired;

    switch (from) {
        case ReadinessState::Resolved:
            return to == ReadinessState::Attached || to == ReadinessState::Unresolved ||
                to == ReadinessState::Unsupported || to == ReadinessState::ResourceLimited;
        case ReadinessState::Attached:
            return to == ReadinessState::CaptureReady || to == ReadinessState::Unresolved ||
                to == ReadinessState::Unsupported || to == ReadinessState::CaptureFailed ||
                to == ReadinessState::ShaderFailed || to == ReadinessState::ResourceLimited;
        case ReadinessState::CaptureReady:
            return to == ReadinessState::Drawn || to == ReadinessState::CaptureFailed ||
                to == ReadinessState::ShaderFailed || to == ReadinessState::ResourceLimited;
        case ReadinessState::Drawn:
            return to == ReadinessState::Attached || to == ReadinessState::CaptureReady ||
                to == ReadinessState::CaptureFailed || to == ReadinessState::ShaderFailed ||
                to == ReadinessState::ResourceLimited;
        case ReadinessState::Unresolved:
        case ReadinessState::Unsupported:
        case ReadinessState::Detached:
            return to == ReadinessState::Resolved;
        case ReadinessState::CaptureFailed:
        case ReadinessState::ShaderFailed:
        case ReadinessState::ResourceLimited:
            return to == ReadinessState::Attached || to == ReadinessState::CaptureReady ||
                to == ReadinessState::Resolved;
        case ReadinessState::Accepted:
        case ReadinessState::Invalid:
        case ReadinessState::Expired:
            return false;
    }
    return false;
}

bool ReadinessTracker::validTargetFailure(ReadinessState state) {
    return state == ReadinessState::Invalid || state == ReadinessState::Unresolved ||
        state == ReadinessState::Unsupported || state == ReadinessState::ResourceLimited ||
        state == ReadinessState::Expired || state == ReadinessState::Detached;
}

} // namespace hfg::v2
