#include "TestHarness.hpp"

#include "v2/render/GlassFramePlan.hpp"

#include <array>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

CapturePlan capture(RenderStage stage = RenderStage::PostWindows,
                    std::uint64_t stageObjectToken = 0) {
  return {
      .key =
          {
              .output = "DP-1",
              .outputGeneration = 2,
              .stage = stage,
              .renderFormat = 1,
              .colorStateToken = 4,
              .stageObjectToken = stageObjectToken,
          },
      .region = {80, 60, 300, 180},
      .bytesPerPixel = 4,
      .pixelCount = 54000,
      .byteCount = 216000,
  };
}

GlassDrawPlan draw(std::uint64_t token = 7,
                   RenderStage stage = RenderStage::PostWindows,
                   std::uint64_t stageObjectToken = 0) {
  return {
      .key =
          {
              .identity =
                  {
                      .owner = "client:test:s1",
                      .targetId = "surface",
                  },
              .output = "DP-1",
              .outputGeneration = 2,
              .stage = stage,
          },
      .resourceToken = token,
      .capture = capture(stage, stageObjectToken),
      .destination = {100.0, 80.0, 240.0, 120.0},
      .destinationPixels = {100.0, 80.0, 240.0, 120.0},
      .damageCoverage = {100, 80, 240, 120},
      .sourceCorners = {},
      .fullSizePixels = {240.0, 120.0},
      .clipOffsetPixels = {},
      .clippedSizePixels = {240.0, 120.0},
      .shapePixels = RoundedRectShape{20.0},
      .roundingPower = 2.0,
      .material = {},
      .opacity = 1.0,
      .transitionActive = false,
  };
}

GlassRenderScene scene(GlassDrawPlan selected = draw()) {
  return {
      .resources =
          {
              CaptureResource{
                  .token = selected.resourceToken,
                  .plan = selected.capture,
              },
          },
      .draws = {std::move(selected)},
      .inactive = {},
      .suppressed = {},
      .targetFailures = {},
      .captureFailures = {},
      .drawFailures = {},
  };
}

RenderHookEvent event(RenderHookStage hook = RenderHookStage::PostWindows,
                      std::uint64_t stageObjectToken = 0) {
  return {
      .output =
          {
              .snapshot =
                  {
                      .name = "DP-1",
                      .objectToken = 1,
                      .modeToken = 2,
                      .bufferWidth = 800,
                      .bufferHeight = 600,
                      .logicalX = 20.0,
                      .logicalY = 30.0,
                      .logicalWidth = 800.0,
                      .logicalHeight = 600.0,
                      .scale = 1.0,
                      .transform = OutputTransform::Normal,
                      .renderFormat = 1,
                      .colorStateToken = 4,
                  },
              .generation = 2,
          },
      .hook = hook,
      .frameToken = 9,
      .stageObjectToken = stageObjectToken,
  };
}

} // namespace

int main() {
  return hfg::test::run({
      Case{"sampling-apron damage redraws full glass coverage",
           [] {
             const auto result = planGlassFrame(scene(), event(),
                                                std::array{
                                                    PixelRect{85, 65, 5, 5},
                                                });
             require(result.hasValue() &&
                         result.value().drawIndices ==
                             std::vector<std::size_t>{0} &&
                         result.value().captureTokens ==
                             std::vector<std::uint64_t>{7} &&
                         result.value().renderDamage ==
                             std::vector{
                                 PixelRect{
                                     100,
                                     80,
                                     240,
                                     120,
                                 },
                             } &&
                         result.value().blockDirectScanout,
                     "sampling damage did not expand to target coverage");
           }},
      Case{"unrelated damage does not schedule capture or draw",
           [] {
             const auto result = planGlassFrame(scene(), event(),
                                                std::array{
                                                    PixelRect{700, 500, 20, 20},
                                                });
             require(result.hasValue() && result.value().drawIndices.empty() &&
                         result.value().captureTokens.empty() &&
                         result.value().renderDamage.empty() &&
                         result.value().blockDirectScanout,
                     "unrelated damage scheduled rendering or scanout escaped");
           }},
      Case{"transition schedules a frame without external damage",
           [] {
             auto selected = draw();
             selected.transitionActive = true;
             const auto result =
                 planGlassFrame(scene(std::move(selected)), event(), {});
             require(result.hasValue() &&
                         result.value().drawIndices.size() == 1U &&
                         result.value().continuationDamage ==
                             std::vector{
                                 Rect{
                                     120.0,
                                     110.0,
                                     240.0,
                                     120.0,
                                 },
                             },
                     "transition did not sustain its output-global damage");
           }},
      Case{"pre-window identity selects only its exact object",
           [] {
             const auto selected = draw(8, RenderStage::PreWindow, 55);
             const std::array damage{
                 PixelRect{100, 80, 10, 10},
             };
             const auto matching =
                 planGlassFrame(scene(selected),
                                event(RenderHookStage::PreWindow, 55), damage);
             const auto other =
                 planGlassFrame(scene(selected),
                                event(RenderHookStage::PreWindow, 56), damage);
             require(matching.hasValue() &&
                         matching.value().drawIndices.size() == 1U &&
                         other.hasValue() && other.value().drawIndices.empty(),
                     "pre-window object identity was not exact");
           }},
      Case{"other outputs do not block direct scanout",
           [] {
             auto input = scene();
             input.draws.front().key.output = "HDMI-A-1";
             input.draws.front().capture.key.output = "HDMI-A-1";
             input.resources.front().plan.key.output = "HDMI-A-1";
             const auto result = planGlassFrame(input, event(), {});
             require(result.hasValue() && !result.value().blockDirectScanout,
                     "glass on another output blocked scanout");
           }},
      Case{"window decoration selects only its exact target identity",
           [] {
             auto first = draw(8, RenderStage::PreWindow, 55);
             first.key.identity = {.owner = "client:first",
                                   .targetId = "glass"};
             auto second = first;
             second.key.identity = {.owner = "client:second",
                                    .targetId = "glass"};
             second.destination.x += 30.0;
             second.destinationPixels.x += 30.0;
             second.damageCoverage.x += 30;
             const GlassRenderScene scene{
                 .resources =
                     {
                         CaptureResource{
                             .token = 8,
                             .plan = capture(RenderStage::PreWindow, 55),
                         },
                     },
                 .draws = {first, second},
                 .inactive = {},
                 .suppressed = {},
                 .targetFailures = {},
                 .captureFailures = {},
                 .drawFailures = {},
             };
             const std::array damage{PixelRect{0, 0, 300, 300}};
             const auto selected = planWindowDecorationDraws(
                 scene, event(RenderHookStage::PreWindow, 55),
                 first.key.identity, 55, damage);
             require(selected.hasValue() &&
                         selected.value() == std::vector<std::size_t>{0},
                     "decoration selected a sibling target");
           }},
      Case{"window decoration requires the exact compositor object",
           [] {
             const auto selected = draw(8, RenderStage::PreWindow, 55);
             const GlassRenderScene scene{
                 .resources =
                     {
                         CaptureResource{
                             .token = 8,
                             .plan = capture(RenderStage::PreWindow, 55),
                         },
                     },
                 .draws = {selected},
                 .inactive = {},
                 .suppressed = {},
                 .targetFailures = {},
                 .captureFailures = {},
                 .drawFailures = {},
             };
             const std::array damage{PixelRect{0, 0, 300, 300}};
             require(!planWindowDecorationDraws(
                         scene, event(RenderHookStage::PreWindow, 55),
                         selected.key.identity, 56, damage),
                     "decoration accepted a different window object");
           }},
      Case{"stale resource and malformed damage fail closed",
           [] {
             auto stale = scene();
             stale.resources.front().plan.region.x += 1;
             require(!planGlassFrame(stale, event(), {}),
                     "stale capture resource reached a frame");
             require(!planGlassFrame(scene(), event(),
                                     std::array{
                                         PixelRect{-1, 0, 10, 10},
                                     }),
                     "out-of-output damage reached a frame");
           }},
      Case{"draw and capture identity mismatch fails closed",
           [] {
             auto input = scene();
             input.draws.front().capture.key.outputGeneration = 3;
             input.resources.front().plan = input.draws.front().capture;
             const auto result = planGlassFrame(input, event(), {});
             require(!result &&
                         result.error().path == "scene.draws[0].capture.key",
                     "mismatched draw identity reached a frame");
           }},
  });
}
