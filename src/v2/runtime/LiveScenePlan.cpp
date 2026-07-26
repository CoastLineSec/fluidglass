#include "v2/runtime/LiveScenePlan.hpp"

#include "v2/core/Limits.hpp"

#include <map>
#include <string>

namespace hfg::v2 {

Result<LiveSceneBindings>
planLiveSceneBindings(std::span<const PlannedPresentation> presentations) {
    if (presentations.size() > Limits::MAX_DYNAMIC_TARGETS)
        return Result<LiveSceneBindings>::failure({
            .code = ErrorCode::ResourceLimited,
            .path = "presentations",
            .message = "live presentation count exceeds the supported limit",
        });

    std::map<TargetIdentity, WindowAttachmentState> attachments;
    std::map<std::string, DirectScanoutLease, std::less<>> leases;
    for (std::size_t index = 0; index < presentations.size(); ++index) {
        const auto& presentation = presentations[index];
        const auto& key = presentation.presentation.key;
        const auto path = "presentations[" + std::to_string(index) + "]";
        if (key.output.empty() || presentation.output.snapshot.name != key.output ||
            presentation.output.snapshot.objectToken == 0U)
            return Result<LiveSceneBindings>::failure({
                .code = ErrorCode::StaleGeneration,
                .path = path + ".output",
                .message = "presentation output identity is incomplete or inconsistent",
            });

        const DirectScanoutLease lease{
            .output = key.output,
            .objectToken = presentation.output.snapshot.objectToken,
        };
        const auto [leaseIt, insertedLease] = leases.emplace(lease.output, lease);
        if (!insertedLease && !(leaseIt->second == lease))
            return Result<LiveSceneBindings>::failure({
                .code = ErrorCode::StaleGeneration,
                .path = path + ".output",
                .message = "one output name resolved to multiple compositor objects",
            });

        if (presentation.target.attachment.kind != TargetKind::Window)
            continue;
        const WindowAttachmentState attachment{
            .identity = key.identity,
            .objectToken = presentation.target.attachment.objectToken,
        };
        if (attachment.objectToken == 0U)
            return Result<LiveSceneBindings>::failure({
                .code = ErrorCode::StaleGeneration,
                .path = path + ".window",
                .message = "window presentation has no compositor object token",
            });
        const auto [attachmentIt, insertedAttachment] =
            attachments.emplace(attachment.identity, attachment);
        if (!insertedAttachment && !(attachmentIt->second == attachment))
            return Result<LiveSceneBindings>::failure({
                .code = ErrorCode::StaleGeneration,
                .path = path + ".window",
                .message = "one target identity resolved to multiple windows",
            });
    }

    LiveSceneBindings result;
    for (const auto& [identity, attachment] : attachments) {
        static_cast<void>(identity);
        result.windowAttachments.push_back(attachment);
    }
    for (const auto& [output, lease] : leases) {
        static_cast<void>(output);
        result.directScanoutLeases.push_back(lease);
    }
    return Result<LiveSceneBindings>::success(std::move(result));
}

} // namespace hfg::v2
