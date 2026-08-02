#include "v2/render/GlassFramePlan.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

Result<GlassFramePlan> failure(ErrorCode code, std::string path,
                               std::string message) {
  return Result<GlassFramePlan>::failure({
      .code = code,
      .path = std::move(path),
      .message = std::move(message),
  });
}

RenderStage renderStage(RenderHookStage hook) {
  switch (hook) {
  case RenderHookStage::PostWallpaper:
    return RenderStage::PostWallpaper;
  case RenderHookStage::PreWindow:
    return RenderStage::PreWindow;
  case RenderHookStage::PostWindows:
    return RenderStage::PostWindows;
  case RenderHookStage::LastMoment:
    return RenderStage::PostLayer;
  }
  return RenderStage::PostWindows;
}

bool validDamage(const PixelRect &rect, const PixelSize &output) {
  if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0)
    return false;
  const auto right = static_cast<std::int64_t>(rect.x) + rect.width;
  const auto bottom = static_cast<std::int64_t>(rect.y) + rect.height;
  return right <= output.width && bottom <= output.height;
}

bool intersects(const PixelRect &left, const PixelRect &right) {
  const auto leftRight = static_cast<std::int64_t>(left.x) + left.width;
  const auto leftBottom = static_cast<std::int64_t>(left.y) + left.height;
  const auto rightRight = static_cast<std::int64_t>(right.x) + right.width;
  const auto rightBottom = static_cast<std::int64_t>(right.y) + right.height;
  return left.x < rightRight && right.x < leftRight && left.y < rightBottom &&
         right.y < leftBottom;
}

} // namespace

Result<GlassFramePlan> planGlassFrame(const GlassRenderScene &scene,
                                      const RenderHookEvent &event,
                                      std::span<const PixelRect> frameDamage) {
  if (auto valid = validateOutputSnapshot(event.output.snapshot); !valid)
    return failure(valid.error().code, "event." + valid.error().path,
                   valid.error().message);
  if (event.output.generation == 0U || event.frameToken == 0U)
    return failure(ErrorCode::InvalidRequest, "event",
                   "render event generation and frame token must not be zero");

  const auto stage = renderStage(event.hook);
  if ((stage == RenderStage::PreWindow) != (event.stageObjectToken != 0U))
    return failure(ErrorCode::InvalidRequest, "event.stage_object_token",
                   "only pre-window events carry an object token");
  const auto outputSize =
      outputOrientedPixelSize(event.output.snapshot);
  if (!outputSize)
    return failure(outputSize.error().code,
                   "event." + outputSize.error().path,
                   outputSize.error().message);
  for (std::size_t index = 0; index < frameDamage.size(); ++index)
    if (!validDamage(frameDamage[index], outputSize.value()))
      return failure(ErrorCode::InvalidRequest,
                     "frame_damage[" + std::to_string(index) + "]",
                     "frame damage lies outside the output buffer");

  std::map<std::uint64_t, CapturePlan> resources;
  for (std::size_t index = 0; index < scene.resources.size(); ++index) {
    const auto &resource = scene.resources[index];
    if (resource.token == 0U ||
        !resources.emplace(resource.token, resource.plan).second)
      return failure(ErrorCode::InvalidRequest,
                     "scene.resources[" + std::to_string(index) + "].token",
                     "resource token must be non-zero and unique");
    if (auto valid = validateCapturePlan(resource.plan); !valid)
      return failure(valid.error().code,
                     "scene.resources[" + std::to_string(index) + "]." +
                         valid.error().path,
                     valid.error().message);
  }

  GlassFramePlan result;
  std::set<std::uint64_t> selectedCaptures;
  for (std::size_t index = 0; index < scene.draws.size(); ++index) {
    const auto &draw = scene.draws[index];
    if (draw.key.output != event.output.snapshot.name ||
        draw.key.outputGeneration != event.output.generation)
      continue;
    result.blockDirectScanout = true;
    if (draw.capture.key.output != draw.key.output ||
        draw.capture.key.outputGeneration != draw.key.outputGeneration ||
        draw.capture.key.stage != draw.key.stage)
      return failure(ErrorCode::StaleGeneration,
                     "scene.draws[" + std::to_string(index) + "].capture.key",
                     "draw and capture identities differ");
    if (draw.key.stage != stage ||
        draw.capture.key.stageObjectToken != event.stageObjectToken)
      continue;
    if (!validDamage(draw.damageCoverage, outputSize.value()) ||
        !validDamage(draw.captureDamageCoverage, outputSize.value()))
      return failure(
          ErrorCode::InvalidRequest,
          "scene.draws[" + std::to_string(index) + "].damage_coverage",
          "draw damage coverage lies outside the output-oriented pixel space");

    const auto resource = resources.find(draw.resourceToken);
    if (resource == resources.end() || !(resource->second == draw.capture))
      return failure(ErrorCode::StaleGeneration,
                     "scene.draws[" + std::to_string(index) + "].resource",
                     "draw references a missing or different capture resource");

    const auto damaged =
        std::ranges::any_of(frameDamage, [&](const PixelRect &rect) {
          return intersects(rect, draw.captureDamageCoverage);
        });
    if (!damaged && !draw.transitionActive)
      continue;

    result.drawIndices.push_back(index);
    result.renderDamage.push_back(draw.damageCoverage);
    if (selectedCaptures.insert(draw.resourceToken).second)
      result.captureTokens.push_back(draw.resourceToken);
    if (draw.transitionActive)
      result.continuationDamage.push_back(
          draw.continuationDamage.width > 0.0 &&
                  draw.continuationDamage.height > 0.0
              ? draw.continuationDamage
              : Rect{
                    .x = event.output.snapshot.logicalX + draw.destination.x,
                    .y = event.output.snapshot.logicalY + draw.destination.y,
                    .width = draw.destination.width,
                    .height = draw.destination.height,
                });
  }
  return Result<GlassFramePlan>::success(std::move(result));
}

Result<std::vector<std::size_t>> planWindowDecorationDraws(
    const GlassRenderScene &scene, const RenderHookEvent &event,
    const TargetIdentity &identity, std::uint64_t objectToken,
    std::span<const PixelRect> frameDamage) {
  if (identity.owner.empty() || identity.targetId.empty())
    return Result<std::vector<std::size_t>>::failure({
        .code = ErrorCode::InvalidRequest,
        .path = "identity",
        .message = "window decoration identity must be complete",
    });
  if (event.hook != RenderHookStage::PreWindow ||
      event.stageObjectToken == 0U || event.stageObjectToken != objectToken)
    return Result<std::vector<std::size_t>>::failure({
        .code = ErrorCode::InvalidRequest,
        .path = "event.stage_object_token",
        .message =
            "window decoration must match an exact pre-window render event",
    });

  auto frame = planGlassFrame(scene, event, frameDamage);
  if (!frame)
    return Result<std::vector<std::size_t>>::failure(frame.error());

  std::vector<std::size_t> selected;
  for (const auto index : frame.value().drawIndices) {
    if (index >= scene.draws.size())
      return Result<std::vector<std::size_t>>::failure({
          .code = ErrorCode::InternalError,
          .path = "frame.draw_indices",
          .message = "frame references a draw absent from the scene",
      });
    if (scene.draws[index].key.identity != identity)
      continue;
    selected.push_back(index);
  }
  if (selected.size() > 1U)
    return Result<std::vector<std::size_t>>::failure({
        .code = ErrorCode::InternalError,
        .path = "scene.draws",
        .message = "window decoration identity resolves to more than one draw",
    });
  return Result<std::vector<std::size_t>>::success(std::move(selected));
}

} // namespace hfg::v2
