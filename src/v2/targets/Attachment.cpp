#include "v2/targets/Attachment.hpp"

#include "v2/core/Limits.hpp"

#include <set>
#include <utility>

namespace hfg::v2 {
namespace {

Result<std::vector<ResolvedPresentation>> invalid(
    std::string path,
    std::string message,
    ErrorCode code = ErrorCode::InvalidRequest) {
    return Result<std::vector<ResolvedPresentation>>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

bool validKind(TargetKind kind) {
    switch (kind) {
        case TargetKind::Window:
        case TargetKind::Layer:
        case TargetKind::Region:
            return true;
    }
    return false;
}

bool validStage(RenderStage stage) {
    switch (stage) {
        case RenderStage::PostWallpaper:
        case RenderStage::PreWindow:
        case RenderStage::PostWindows:
        case RenderStage::PostLayer:
            return true;
    }
    return false;
}

} // namespace

Result<std::vector<ResolvedPresentation>> resolvePresentations(
    const ResolvedAttachment& attachment,
    std::span<const OutputGeneration> outputs) {
    if (attachment.identity.owner.empty() ||
        attachment.identity.targetId.empty())
        return invalid("attachment.identity", "owner and target id must not be empty");
    if (!validKind(attachment.kind))
        return invalid("attachment.kind", "unsupported target kind");
    if (attachment.objectToken == 0U)
        return invalid("attachment.object_token", "attachment object token must not be zero");
    if (!validStage(attachment.stage))
        return invalid("attachment.stage", "unsupported render stage");
    if (attachment.outputFilter && attachment.outputFilter->empty())
        return invalid("attachment.output_filter", "output filter must not be empty");
    if (outputs.size() > Limits::MAX_PRESENTATIONS_PER_TARGET)
        return invalid(
            "outputs",
            "output count exceeds the per-target presentation limit",
            ErrorCode::ResourceLimited);

    std::set<std::string> outputNames;
    std::vector<ResolvedPresentation> presentations;
    presentations.reserve(outputs.size());
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        const auto& output = outputs[index];
        if (!outputNames.emplace(output.snapshot.name).second)
            return invalid(
                "outputs[" + std::to_string(index) + "]",
                "duplicate output generation");
        if (attachment.outputFilter &&
            *attachment.outputFilter != output.snapshot.name)
            continue;

        auto mapped = mapGlobalLogicalRect(
            attachment.globalGeometry,
            output);
        if (!mapped)
            return Result<std::vector<ResolvedPresentation>>::failure({
                .code = mapped.error().code,
                .path = "outputs[" + std::to_string(index) + "]." +
                    mapped.error().path,
                .message = mapped.error().message,
            });
        if (!mapped.value())
            continue;
        presentations.push_back({
            .key = PresentationKey{
                .identity = attachment.identity,
                .output = output.snapshot.name,
                .outputGeneration = output.generation,
                .stage = attachment.stage,
            },
            .attachmentToken = attachment.objectToken,
            .geometry = std::move(*mapped.value()),
        });
    }
    return Result<std::vector<ResolvedPresentation>>::success(
        std::move(presentations));
}

} // namespace hfg::v2
