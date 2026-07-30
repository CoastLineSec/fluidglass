#include "v2/render/GlassRenderScene.hpp"

#include "v2/core/Limits.hpp"

#include <set>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

Result<GlassRenderScene> failure(ErrorCode code, std::string path,
                                 std::string message) {
  return Result<GlassRenderScene>::failure({
      .code = code,
      .path = std::move(path),
      .message = std::move(message),
  });
}

} // namespace

Result<GlassRenderScene>
buildGlassRenderScene(const CaptureScene &captures,
                      std::span<const CaptureResource> resources) {
  if (captures.captures.size() > Limits::MAX_CAPTURE_REQUESTS ||
      resources.size() > Limits::MAX_CAPTURE_REQUESTS)
    return failure(ErrorCode::ResourceLimited, "scene",
                   "capture or resource count exceeds the supported limit");

  for (std::size_t index = 0; index < captures.captures.size(); ++index) {
    if (auto valid = validateCapturePlan(captures.captures[index]); !valid)
      return failure(valid.error().code,
                     "captures[" + std::to_string(index) + "]." +
                         valid.error().path,
                     valid.error().message);
    for (std::size_t previous = 0; previous < index; ++previous)
      if (captures.captures[previous] == captures.captures[index])
        return failure(ErrorCode::InvalidRequest,
                       "captures[" + std::to_string(index) + "]",
                       "capture plans must be unique");
  }

  CaptureResourceIndex resourceIndex;
  for (std::size_t index = 0; index < resources.size(); ++index) {
    if (auto added = resourceIndex.add(resources[index]); !added)
      return failure(added.error().code,
                     "resources[" + std::to_string(index) + "]." +
                         added.error().path,
                     added.error().message);
  }

  GlassRenderScene result{
      .resources = {},
      .draws = {},
      .handoffs = captures.handoffs,
      .inactive = captures.inactive,
      .suppressed = captures.suppressed,
      .targetFailures = captures.targetFailures,
      .captureFailures = captures.captureFailures,
      .drawFailures = {},
  };
  std::set<PresentationKey> presentationKeys;
  std::set<std::uint64_t> selectedTokens;
  result.draws.reserve(captures.assignments.size());
  for (std::size_t index = 0; index < captures.assignments.size(); ++index) {
    const auto &assignment = captures.assignments[index];
    const auto &key = assignment.presentation.presentation.key;
    if (!presentationKeys.insert(key).second)
      return failure(ErrorCode::InvalidRequest,
                     "assignments[" + std::to_string(index) +
                         "].presentation.key",
                     "presentation keys must be unique");
    if (assignment.captureIndex >= captures.captures.size() ||
        !(assignment.required == captures.captures[assignment.captureIndex]))
      return failure(ErrorCode::InvalidRequest,
                     "assignments[" + std::to_string(index) + "].capture_index",
                     "assignment does not reference its canonical capture");

    const auto selected = resourceIndex.findCovering(assignment.required);
    if (!selected) {
      result.drawFailures.push_back({
          .key = key,
          .error =
              {
                  .code = ErrorCode::ResourceLimited,
                  .path = "resource",
                  .message = "no allocated capture covers this presentation",
              },
      });
      continue;
    }
    auto draw = buildGlassDrawPlan(assignment, *selected);
    if (!draw) {
      result.drawFailures.push_back({
          .key = key,
          .error = draw.error(),
      });
      continue;
    }
    result.draws.push_back(std::move(draw.value()));
    if (selectedTokens.insert(selected->token).second)
      result.resources.push_back(*selected);
  }
  return Result<GlassRenderScene>::success(std::move(result));
}

} // namespace hfg::v2
