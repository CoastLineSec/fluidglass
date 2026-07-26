#include "TestHarness.hpp"

#include "v2/render/GlassUniformPayload.hpp"

#include <cmath>
#include <limits>
#include <variant>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

GlassDrawPlan plan(Shape shape) {
  return {
      .key =
          {
              .identity =
                  {
                      .owner = "client:test:session",
                      .targetId = "surface",
                  },
              .output = "DP-1",
              .outputGeneration = 2,
              .stage = RenderStage::PostWindows,
          },
      .resourceToken = 9,
      .capture =
          {
              .key =
                  {
                      .output = "DP-1",
                      .outputGeneration = 2,
                      .stage = RenderStage::PostWindows,
                      .renderFormat = 1,
                      .colorStateToken = 3,
                  },
              .region = {0, 0, 400, 300},
              .bytesPerPixel = 4,
              .pixelCount = 120000,
              .byteCount = 480000,
          },
      .destination = {10.0, 20.0, 100.0, 60.0},
      .destinationPixels = {12.5, 25.0, 125.0, 75.0},
      .damageCoverage = {12, 25, 126, 75},
      .sourceCorners =
          {
              TextureCoordinate{0.1, 0.2},
              TextureCoordinate{0.4, 0.2},
              TextureCoordinate{0.4, 0.5},
              TextureCoordinate{0.1, 0.5},
          },
      .fullSizePixels = {150.0, 100.0},
      .clipOffsetPixels = {25.0, 10.0},
      .clippedSizePixels = {125.0, 75.0},
      .shapePixels = std::move(shape),
      .roundingPower = 2.0,
      .material =
          {
              .blurPixels = 18.0,
              .refractionPixels = 45.0,
              .rimBandPixels = 30.0,
              .bevelPixels = 24.0,
              .rimWidthPixels = 3.0,
              .lensBandPixels = 32.0,
              .highlight = 0.1,
              .shadow = 0.2,
              .specular = 0.3,
              .chroma = 0.15,
              .edgeDepth = 0.14,
              .lens = 0.12,
              .gloss = 0.08,
              .tintStrength = 0.43,
              .veilSaturation = 0.95,
              .lightDirection = {0.0, 1.0},
              .tintColor = {0.2, 0.3, 0.4},
          },
      .opacity = 0.75,
  };
}

} // namespace

int main() {
  return hfg::test::run({
      Case{"rounded plan packs complete material and geometry",
           [] {
             const auto result =
                 buildGlassUniformPayload(plan(RoundedRectShape{16.0}));
             require(result.hasValue(), "rounded payload failed");
             const auto &payload = result.value();
             require(payload.shapeKind == 0 && payload.radius == 16.0F &&
                         payload.fullSize == UniformVec2{150.0F, 100.0F} &&
                         payload.clipOffset == UniformVec2{25.0F, 10.0F} &&
                         payload.clipSize == UniformVec2{125.0F, 75.0F},
                     "rounded geometry changed");
             require(payload.sourceCorners[2] == UniformVec2{0.4F, 0.5F} &&
                         payload.blurPixels == 18.0F &&
                         payload.tint ==
                             UniformVec4{
                                 0.2F,
                                 0.3F,
                                 0.4F,
                                 0.43F,
                             } &&
                         payload.opacity == 0.75F,
                     "capture or material payload changed");
           }},
      Case{"ring plan keeps independent radius and thickness",
           [] {
             const auto result = buildGlassUniformPayload(plan(RingShape{
                 .outerRadius = 40.0,
                 .thickness = 9.0,
             }));
             require(result.hasValue() && result.value().shapeKind == 1 &&
                         result.value().ringRadius == 40.0F &&
                         result.value().ringThickness == 9.0F,
                     "ring payload changed");
           }},
      Case{"compound arrays preserve extents and clear unused slots",
           [] {
             CompoundShape shape;
             shape.base = CompoundBase{
                 .corners = {1.0, 2.0, 3.0, 4.0},
             };
             shape.cutout = CompoundCutout{
                 .rect = {10.0, 11.0, 80.0, 40.0},
                 .corners = {5.0, 6.0, 7.0, 8.0},
             };
             shape.parts.push_back({
                 .rect = {20.0, 30.0, 40.0, 20.0},
                 .corners = {2.0, 3.0, 4.0, 5.0},
                 .junctions = {6.0, 7.0, 8.0, 9.0},
                 .materialExtent = Rect{15.0, 25.0, 50.0, 30.0},
                 .transition = std::nullopt,
                 .opacity = 0.6,
             });
             shape.parts.push_back({
                 .rect = {70.0, 30.0, 20.0, 20.0},
                 .corners = {},
                 .junctions = {},
                 .materialExtent = std::nullopt,
                 .transition = std::nullopt,
                 .opacity = 1.0,
             });
             shape.connectors.push_back({60.0, 35.0, 10.0, 8.0});
             shape.connectorCurve = 4.0;

             const auto result =
                 buildGlassUniformPayload(plan(std::move(shape)));
             require(result.hasValue(), "compound payload failed");
             const auto &payload = result.value();
             require(payload.shapeKind == 2 && payload.baseEnabled == 1 &&
                         payload.cutoutEnabled == 1 && payload.partCount == 2 &&
                         payload.connectorCount == 1,
                     "compound topology changed");
             require(payload.partMaterialExtents[0] ==
                             UniformVec4{
                                 15.0F,
                                 25.0F,
                                 50.0F,
                                 30.0F,
                             } &&
                         payload.partMaterialExtents[1] ==
                             payload.partRects[1] &&
                         payload.partOpacity[0] == 0.6F &&
                         payload.partRects[2] == UniformVec4{} &&
                         payload.connectorRects[1] == UniformVec4{},
                     "compound array packing or reset changed");
           }},
      Case{"capture identity mismatch fails closed",
           [] {
             auto input = plan(RoundedRectShape{16.0});
             input.capture.key.outputGeneration = 3;
             const auto result = buildGlassUniformPayload(input);
             require(!result && result.error().path == "plan.capture.key",
                     "stale capture identity reached uniforms");
           }},
      Case{"non-finite geometry and material fail closed",
           [] {
             auto geometry = plan(RoundedRectShape{16.0});
             geometry.destinationPixels.width =
                 std::numeric_limits<double>::quiet_NaN();
             require(!buildGlassUniformPayload(geometry),
                     "non-finite destination reached uniforms");

             auto material = plan(RoundedRectShape{16.0});
             material.material.blurPixels =
                 std::numeric_limits<double>::infinity();
             require(!buildGlassUniformPayload(material),
                     "non-finite material reached uniforms");
           }},
      Case{"clipping cannot exceed the full shape",
           [] {
             auto input = plan(RoundedRectShape{16.0});
             input.clipOffsetPixels.x = 30.0;
             input.clippedSizePixels.width = 125.0;
             require(!buildGlassUniformPayload(input),
                     "out-of-bounds clip reached uniforms");
           }},
      Case{"forged compound over the public limit fails closed",
           [] {
             CompoundShape shape;
             shape.parts.resize(Limits::MAX_COMPOUND_PARTS + 1U);
             for (auto &part : shape.parts)
               part.rect = {0.0, 0.0, 1.0, 1.0};
             require(!buildGlassUniformPayload(plan(std::move(shape))),
                     "over-limit compound reached uniforms");
           }},
  });
}
