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

void ReadinessTracker::erasePresentation(const PresentationKey& key) {
    m_presentations.erase(key);
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
            // Unresolved/Unsupported are reachable here and from Drawn: a
            // window decoration can fail after its presentation drew, and a
            // rejected report would leave the record claiming drawn forever.
            return to == ReadinessState::Drawn || to == ReadinessState::CaptureFailed ||
                to == ReadinessState::ShaderFailed || to == ReadinessState::ResourceLimited ||
                to == ReadinessState::Unresolved || to == ReadinessState::Unsupported;
        case ReadinessState::Drawn:
            return to == ReadinessState::Attached || to == ReadinessState::CaptureReady ||
                to == ReadinessState::CaptureFailed || to == ReadinessState::ShaderFailed ||
                to == ReadinessState::ResourceLimited ||
                to == ReadinessState::Unresolved || to == ReadinessState::Unsupported;
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
        case ReadinessState::Inactive:
            return false;
    }
    return false;
}

bool ReadinessTracker::validTargetFailure(ReadinessState state) {
    return state == ReadinessState::Invalid || state == ReadinessState::Unresolved ||
        state == ReadinessState::Unsupported || state == ReadinessState::ResourceLimited ||
        state == ReadinessState::Expired || state == ReadinessState::Detached ||
        state == ReadinessState::Inactive;
}

std::vector<std::pair<PresentationKey, ReadinessRecord>>
ReadinessTracker::allPresentations() const {
    return {m_presentations.begin(), m_presentations.end()};
}

std::vector<TargetIdentity> ReadinessTracker::targetIdentities() const {
    std::vector<TargetIdentity> result;
    result.reserve(m_targets.size());
    for (const auto& [identity, record] : m_targets)
        result.push_back(identity);
    return result;
}

std::vector<OutputGlassLiveness> outputGlassLiveness(
    const ReadinessTracker& readiness,
    std::span<const KnownOutput> knownOutputs) {
    std::map<std::string, OutputGlassLiveness> byOutput;

    for (const auto& known : knownOutputs) {
        auto& liveness = byOutput[known.name];
        liveness.output = known.name;
        liveness.outputGeneration =
            std::max(liveness.outputGeneration, known.generation);
    }

    for (const auto& [key, record] : readiness.allPresentations()) {
        auto& liveness = byOutput[key.output];
        liveness.output = key.output;
        // The newest generation seen wins the label, so a client can tell an
        // output has been rebuilt underneath it.
        liveness.outputGeneration =
            std::max(liveness.outputGeneration, key.outputGeneration);

        switch (record.state) {
            case ReadinessState::Drawn:
                ++liveness.drawn;
                break;
            case ReadinessState::Inactive:
                // Resolved, and will never draw as published. Not a failure,
                // and not something to keep waiting on.
                ++liveness.inactive;
                break;
            case ReadinessState::Invalid:
            case ReadinessState::Unresolved:
            case ReadinessState::Unsupported:
            case ReadinessState::CaptureFailed:
            case ReadinessState::ShaderFailed:
            case ReadinessState::ResourceLimited:
            case ReadinessState::Expired:
            case ReadinessState::Detached:
                ++liveness.failed;
                break;
            case ReadinessState::Accepted:
            case ReadinessState::Resolved:
            case ReadinessState::Attached:
            case ReadinessState::CaptureReady:
                ++liveness.awaiting;
                break;
        }
    }

    std::vector<OutputGlassLiveness> result;
    result.reserve(byOutput.size());
    for (auto& [name, liveness] : byOutput) {
        liveness.drawing = liveness.drawn > 0;
        result.push_back(std::move(liveness));
    }
    return result;
}

} // namespace hfg::v2
