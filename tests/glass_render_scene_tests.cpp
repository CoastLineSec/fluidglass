#include "TestHarness.hpp"

#include "v2/model/Material.hpp"
#include "v2/model/Target.hpp"
#include "v2/render/GlassRenderScene.hpp"
#include "v2/render/MaterialSampling.hpp"

#include <array>
#include <cstdint>
#include <utility>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

constexpr std::uint32_t AR24 = 0x34325241U;

struct Fixture {
  CaptureScene scene;
  CaptureResource resource;
};

Fixture fixture() {
  const OutputGeneration output{
      .snapshot =
          {
              .name = "DP-1",
              .objectToken = 10,
              .modeToken = 20,
              .bufferWidth = 800,
              .bufferHeight = 600,
              .logicalWidth = 800.0,
              .logicalHeight = 600.0,
              .scale = 1.0,
              .transform = OutputTransform::Normal,
              .renderFormat = AR24,
              .colorStateToken = 30,
          },
      .generation = 2,
  };
  MaterialInput materialInput;
  const auto material = validateMaterial("fluid", materialInput);
  if (!material)
    throw hfg::test::Failure("material fixture failed");

  TargetInput targetInput{
      .id = "surface",
      .kind = TargetKind::Region,
      .material =
          {
              .source = MaterialSource::Session,
              .name = "fluid",
          },
      .shape = RoundedRectShape{20.0},
      .selector = RegionSelector{"DP-1"},
      .geometry = Rect{100.0, 80.0, 240.0, 120.0},
      .stage = RenderStage::PostWindows,
      .transition = std::nullopt,
      .enabled = true,
  };
  const auto target = validateTarget(std::move(targetInput));
  if (!target)
    throw hfg::test::Failure("target fixture failed");

  const TargetIdentity identity{
      .owner = "client:test:s1",
      .targetId = "surface",
  };
  ResolvedTarget resolved{
      .definition = target.value(),
      .attachment =
          {
              .identity = identity,
              .kind = TargetKind::Region,
              .objectToken = 41,
              .globalGeometry = {100.0, 80.0, 240.0, 120.0},
              .stage = RenderStage::PostWindows,
              .outputFilter = "DP-1",
              .opacity = 0.8,
          },
      .roundingPower = 2.0,
  };
  const auto presentations =
      resolvePresentations(resolved.attachment, std::array{output});
  if (!presentations || presentations.value().size() != 1U)
    throw hfg::test::Failure("presentation fixture failed");
  const auto sampling =
      resolveMaterialSampling(material.value(), 240.0, 120.0, 1.0);
  if (!sampling)
    throw hfg::test::Failure("sampling fixture failed");

  PresentationScene presentationScene{
      .presentations =
          {
              PlannedPresentation{
                  .target = std::move(resolved),
                  .material = material.value(),
                  .presentation = presentations.value().front(),
                  .output = output,
                  .sampling = sampling.value(),
              },
          },
      .inactive =
          {
              TargetIdentity{"config", "inactive"},
          },
      .suppressed =
          {
              TargetIdentity{"config", "suppressed"},
          },
      .failures = {},
  };
  const CaptureLimits limits{
      .maxWidth = 800,
      .maxHeight = 600,
      .maxApronPixels = 1000,
      .maxBytesPerPixel = 8,
      .maxPixels = 480000,
      .maxBytes = 3840000,
  };
  const std::array formats{
      CaptureFormatLayout{
          .renderFormat = AR24,
          .bytesPerPixel = 4,
      },
  };
  const auto scene = buildCaptureScene(presentationScene, formats, limits);
  if (!scene || scene.value().captures.size() != 1U ||
      scene.value().assignments.size() != 1U)
    throw hfg::test::Failure("capture scene fixture failed");
  return {
      .scene = scene.value(),
      .resource =
          {
              .token = 9,
              .plan = scene.value().captures.front(),
          },
  };
}

} // namespace

int main() {
  return hfg::test::run({
      Case{"allocated resource produces a draw and keeps diagnostics",
           [] {
             const auto input = fixture();
             const auto result =
                 buildGlassRenderScene(input.scene, std::array{input.resource});
             require(result.hasValue(), "render scene failed");
             require(result.value().draws.size() == 1U &&
                         result.value().draws.front().resourceToken == 9U &&
                         result.value().resources ==
                             std::vector{input.resource},
                     "draw or selected resource changed");
             require(result.value().inactive == input.scene.inactive &&
                         result.value().suppressed == input.scene.suppressed,
                     "scene diagnostics were dropped");
           }},
      Case{"smallest covering resource is selected deterministically",
           [] {
             auto input = fixture();
             auto large = input.resource;
             large.token = 8;
             large.plan.region = {0, 0, 800, 600};
             large.plan.pixelCount = 480000;
             large.plan.byteCount = 1920000;
             const auto result =
                 buildGlassRenderScene(input.scene, std::array{
                                                        large,
                                                        input.resource,
                                                    });
             require(result.hasValue() &&
                         result.value().draws.front().resourceToken == 9U &&
                         result.value().resources ==
                             std::vector{input.resource},
                     "covering resource selection changed");
           }},
      Case{"missing allocation becomes a per-presentation failure",
           [] {
             const auto input = fixture();
             const auto result = buildGlassRenderScene(input.scene, {});
             require(result.hasValue() && result.value().draws.empty() &&
                         result.value().resources.empty() &&
                         result.value().drawFailures.size() == 1U &&
                         result.value().drawFailures.front().error.code ==
                             ErrorCode::ResourceLimited,
                     "allocation failure did not stay isolated");
           }},
      Case{"stale assignment capture index fails the whole scene",
           [] {
             auto input = fixture();
             input.scene.assignments.front().captureIndex = 1;
             const auto result =
                 buildGlassRenderScene(input.scene, std::array{input.resource});
             require(!result &&
                         result.error().path == "assignments[0].capture_index",
                     "forged capture index reached rendering");
           }},
      Case{"duplicate presentation identity fails closed",
           [] {
             auto input = fixture();
             input.scene.assignments.push_back(input.scene.assignments.front());
             const auto result =
                 buildGlassRenderScene(input.scene, std::array{input.resource});
             require(!result && result.error().path ==
                                    "assignments[1].presentation.key",
                     "duplicate presentation reached rendering");
           }},
      Case{"duplicate resource token fails closed",
           [] {
             const auto input = fixture();
             const auto result =
                 buildGlassRenderScene(input.scene, std::array{
                                                        input.resource,
                                                        input.resource,
                                                    });
             require(!result &&
                         result.error().path == "resources[1].resource.token",
                     "duplicate resource token reached rendering");
           }},
  });
}
