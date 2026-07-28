#include "TestHarness.hpp"

#include "v2/render/GlassShaderSource.hpp"

#include <array>
#include <string_view>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

int main() {
    return hfg::test::run({
        Case{"vertex shader exposes tracked projection inputs", [] {
            const auto source = glassVertexShaderSource();
            require(
                source.contains("#version 320 es") &&
                    source.contains("uniform mat3 proj") &&
                    source.contains("in vec2 pos") &&
                    source.contains("in vec2 texcoord"),
                "vertex shader contract is incomplete");
        }},
        Case{"fragment shader keeps the generic shape vocabulary", [] {
            const auto source = glassFragmentShaderSource();
            for (const auto token : {
                     "uShapeKind",
                     "uRadius",
                     "uRingRadius",
                     "uRingThickness",
                     "uBaseEnabled",
                     "uCutoutEnabled",
                     "uPartRects",
                     "uPartRadii",
                     "uPartJunctions",
                     "uPartMaterialExtents",
                     "uPartOpacity",
                     "uConnectorRects",
                     "uConnectorCurve",
                 })
                require(
                    source.contains(token),
                    "shape uniform is missing");
            require(
                source.contains("const int MAX_PARTS = 32") &&
                    source.contains(
                        "const int MAX_CONNECTORS = 32"),
                "shader limit differs from the public compound limit");
        }},
        Case{"fragment shader preserves transformed bounded capture inputs", [] {
            const auto source = glassFragmentShaderSource();
            for (const auto token : {
                     "uSourceTL",
                     "uSourceTR",
                     "uSourceBR",
                     "uSourceBL",
                     "uFullSize",
                     "uClipOffset",
                     "uClipSize",
                     "captureCoordinate",
                 })
                require(
                    source.contains(token),
                    "bounded capture input is missing");
        }},
        Case{"fragment shader includes the complete material input", [] {
            const auto source = glassFragmentShaderSource();
            for (const auto token : {
                     "uRefractionPixels",
                     "uEdgeBandPixels",
                     "uBevelPixels",
                     "uRimWidthPixels",
                     "uLensBandPixels",
                     "uHighlight",
                     "uShadow",
                     "uSpecular",
                     "uChroma",
                     "uEdgeDepth",
                     "uLens",
                     "uGloss",
                     "uTint",
                     "uVeilSaturation",
                     "uLightDirection",
                     "uOpacity",
                 })
                require(
                    source.contains(token),
                    "material uniform is missing");
        }},
        Case{"fragment output is premultiplied", [] {
            const auto source = glassFragmentShaderSource();
            require(
                source.contains(
                    "fragColor = vec4(glass * alpha, alpha)"),
                "shader output is not premultiplied");
        }},
        Case{"compound opacity follows material extents", [] {
            const auto source = glassFragmentShaderSource();
            require(
                source.contains("uPartMaterialExtents[index]") &&
                    source.contains("materialOpacity") &&
                    source.contains("cutoutInterior") &&
                    source.contains("coverage *") &&
                    source.contains("materialOpacity"),
                "compound material extent or opacity handling is missing");
        }},
    });
}
