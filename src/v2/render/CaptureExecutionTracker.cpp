#include "v2/render/CaptureExecutionTracker.hpp"

#include <string>

namespace hfg::v2 {
namespace {

Result<void> invalidToken(std::string path) {
    return Result<void>::failure({
        .code = ErrorCode::InvalidRequest,
        .path = std::move(path),
        .message = "capture execution tokens must be non-zero",
    });
}

} // namespace

Result<void> CaptureExecutionTracker::schedule(std::uint64_t resourceToken,
                                                std::uint64_t frameToken) {
    if (resourceToken == 0)
        return invalidToken("resource_token");
    if (frameToken == 0)
        return invalidToken("frame_token");
    m_scheduled[resourceToken] = frameToken;
    m_completed.erase(resourceToken);
    return Result<void>::success();
}

Result<void> CaptureExecutionTracker::complete(std::uint64_t resourceToken,
                                                std::uint64_t frameToken) {
    if (!ready(resourceToken, frameToken)) {
        const auto scheduled = m_scheduled.find(resourceToken);
        if (resourceToken == 0 || frameToken == 0)
            return invalidToken(resourceToken == 0 ? "resource_token" : "frame_token");
        if (scheduled == m_scheduled.end() || scheduled->second != frameToken)
            return Result<void>::failure({
                .code = ErrorCode::StaleGeneration,
                .path = "capture",
                .message = "capture completed outside its scheduled frame",
            });
    }
    m_completed[resourceToken] = frameToken;
    return Result<void>::success();
}

void CaptureExecutionTracker::fail(std::uint64_t resourceToken,
                                    std::uint64_t frameToken) noexcept {
    const auto scheduled = m_scheduled.find(resourceToken);
    if (scheduled != m_scheduled.end() && scheduled->second == frameToken)
        m_completed.erase(resourceToken);
}

bool CaptureExecutionTracker::ready(std::uint64_t resourceToken,
                                    std::uint64_t frameToken) const noexcept {
    const auto scheduled = m_scheduled.find(resourceToken);
    if (scheduled == m_scheduled.end() || scheduled->second != frameToken)
        return false;
    const auto completed = m_completed.find(resourceToken);
    return completed != m_completed.end() && completed->second == frameToken;
}

void CaptureExecutionTracker::retire(std::uint64_t resourceToken) noexcept {
    m_scheduled.erase(resourceToken);
    m_completed.erase(resourceToken);
}

void CaptureExecutionTracker::clear() noexcept {
    m_scheduled.clear();
    m_completed.clear();
}

} // namespace hfg::v2
