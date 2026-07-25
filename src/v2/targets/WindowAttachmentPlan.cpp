#include "v2/targets/WindowAttachmentPlan.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace hfg::v2 {
namespace {

using StateMap = std::map<TargetIdentity, WindowAttachmentState>;

Result<StateMap> indexStates(
    std::span<const WindowAttachmentState> states,
    std::string path) {
    if (states.size() > Limits::MAX_DYNAMIC_TARGETS)
        return Result<StateMap>::failure({
            .code = ErrorCode::ResourceLimited,
            .path = std::move(path),
            .message = "window attachment count exceeds the supported limit",
        });

    StateMap indexed;
    std::set<std::uint64_t> objectTokens;
    for (std::size_t index = 0; index < states.size(); ++index) {
        const auto& state = states[index];
        const auto itemPath =
            path + "[" + std::to_string(index) + "]";
        if (state.identity.owner.empty() ||
            state.identity.targetId.empty())
            return Result<StateMap>::failure({
                .code = ErrorCode::InvalidRequest,
                .path = itemPath + ".identity",
                .message = "window attachment identity must be complete",
            });
        if (state.objectToken == 0U)
            return Result<StateMap>::failure({
                .code = ErrorCode::InvalidRequest,
                .path = itemPath + ".object_token",
                .message = "window attachment object token must not be zero",
            });
        if (!indexed.emplace(state.identity, state).second)
            return Result<StateMap>::failure({
                .code = ErrorCode::InvalidRequest,
                .path = itemPath + ".identity",
                .message = "window attachment identity must be unique",
            });
        if (!objectTokens.insert(state.objectToken).second)
            return Result<StateMap>::failure({
                .code = ErrorCode::InvalidRequest,
                .path = itemPath + ".object_token",
                .message = "only one effective glass attachment may own a window",
            });
    }
    return Result<StateMap>::success(std::move(indexed));
}

} // namespace

Result<WindowAttachmentPlan>
planWindowAttachments(
    std::span<const WindowAttachmentState> current,
    std::span<const WindowAttachmentState> desired) {
    auto currentByIdentity = indexStates(current, "current");
    if (!currentByIdentity)
        return Result<WindowAttachmentPlan>::failure(
            currentByIdentity.error());
    auto desiredByIdentity = indexStates(desired, "desired");
    if (!desiredByIdentity)
        return Result<WindowAttachmentPlan>::failure(
            desiredByIdentity.error());

    WindowAttachmentPlan plan;
    for (const auto& [identity, desiredState] :
         desiredByIdentity.value()) {
        const auto existing =
            currentByIdentity.value().find(identity);
        if (existing != currentByIdentity.value().end() &&
            existing->second.objectToken ==
                desiredState.objectToken)
            plan.retain.push_back(desiredState);
        else
            plan.add.push_back(desiredState);
    }
    for (const auto& [identity, currentState] :
         currentByIdentity.value()) {
        const auto wanted =
            desiredByIdentity.value().find(identity);
        if (wanted == desiredByIdentity.value().end() ||
            wanted->second.objectToken !=
                currentState.objectToken)
            plan.remove.push_back(currentState);
    }
    return Result<WindowAttachmentPlan>::success(
        std::move(plan));
}

} // namespace hfg::v2
