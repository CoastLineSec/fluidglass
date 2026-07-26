#include "v2/render/GlassShaderSource.hpp"

namespace hfg::v2 {

std::string_view glassVertexShaderSource() noexcept {
    return R"GLSL(#version 320 es
uniform mat3 proj;
in vec2 pos;
in vec2 texcoord;
out vec2 vTexcoord;

void main() {
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
    vTexcoord = texcoord;
}
)GLSL";
}

std::string_view glassFragmentShaderSource() noexcept {
    return R"GLSL(#version 320 es
precision highp float;

in vec2 vTexcoord;
layout(location = 0) out vec4 fragColor;

const int MAX_PARTS = 32;
const int MAX_CONNECTORS = 32;

uniform sampler2D uCapture;
uniform vec2 uSourceTL;
uniform vec2 uSourceTR;
uniform vec2 uSourceBR;
uniform vec2 uSourceBL;
uniform vec2 uFullSize;
uniform vec2 uClipOffset;
uniform vec2 uClipSize;

uniform int uShapeKind;
uniform float uRadius;
uniform float uRoundingPower;
uniform float uRingRadius;
uniform float uRingThickness;
uniform int uBaseEnabled;
uniform vec4 uBaseRadii;
uniform int uCutoutEnabled;
uniform vec4 uCutoutRect;
uniform vec4 uCutoutRadii;
uniform int uPartCount;
uniform vec4 uPartRects[MAX_PARTS];
uniform vec4 uPartRadii[MAX_PARTS];
uniform vec4 uPartJunctions[MAX_PARTS];
uniform vec4 uPartMaterialExtents[MAX_PARTS];
uniform float uPartOpacity[MAX_PARTS];
uniform int uConnectorCount;
uniform vec4 uConnectorRects[MAX_CONNECTORS];
uniform float uConnectorCurve;

uniform float uBlurPixels;
uniform float uRefractionPixels;
uniform float uEdgeBandPixels;
uniform float uBevelPixels;
uniform float uRimWidthPixels;
uniform float uLensBandPixels;
uniform float uHighlight;
uniform float uShadow;
uniform float uSpecular;
uniform float uChroma;
uniform float uEdgeDepth;
uniform float uLens;
uniform float uGloss;
uniform vec4 uTint;
uniform float uVeilSaturation;
uniform vec2 uLightDirection;
uniform float uOpacity;

float powerLength(vec2 value, float power) {
    vec2 bounded = max(value, vec2(0.0));
    float p = clamp(power, 1.0, 16.0);
    return pow(pow(bounded.x, p) + pow(bounded.y, p), 1.0 / p);
}

float radiusForPoint(vec2 point, vec4 radii) {
    if (point.x < 0.0)
        return point.y < 0.0 ? radii.x : radii.w;
    return point.y < 0.0 ? radii.y : radii.z;
}

float roundedRectDistance(
    vec2 pixel,
    vec4 rect,
    vec4 radii,
    float roundingPower) {
    vec2 halfSize = max(rect.zw * 0.5, vec2(0.0));
    vec2 point = pixel - (rect.xy + halfSize);
    float radius = min(
        max(radiusForPoint(point, radii), 0.0),
        min(halfSize.x, halfSize.y));
    vec2 outside = abs(point) - halfSize + radius;
    return min(max(outside.x, outside.y), 0.0) +
        powerLength(outside, roundingPower) -
        radius;
}

float smoothUnion(float left, float right, float radius) {
    if (radius <= 0.0001)
        return min(left, right);
    float h = max(radius - abs(left - right), 0.0) / radius;
    return min(left, right) - h * h * radius * 0.25;
}

float rectangleDistance(vec2 pixel, vec4 rect) {
    vec2 halfSize = max(rect.zw * 0.5, vec2(0.0));
    vec2 outside = abs(pixel - (rect.xy + halfSize)) - halfSize;
    return min(max(outside.x, outside.y), 0.0) +
        length(max(outside, vec2(0.0)));
}

float simpleRoundedDistance(vec2 pixel) {
    return roundedRectDistance(
        pixel,
        vec4(0.0, 0.0, uFullSize.x, uFullSize.y),
        vec4(uRadius),
        uRoundingPower);
}

float ringDistance(vec2 pixel) {
    vec2 center = uFullSize * 0.5;
    float outer = min(
        max(uRingRadius, 0.0),
        min(center.x, center.y));
    float thickness = min(
        max(uRingThickness, 0.0),
        outer);
    return abs(length(pixel - center) - (outer - thickness * 0.5)) -
        thickness * 0.5;
}

float compoundDistance(vec2 pixel) {
    float distance = 1e20;
    if (uBaseEnabled != 0)
        distance = roundedRectDistance(
            pixel,
            vec4(0.0, 0.0, uFullSize.x, uFullSize.y),
            uBaseRadii,
            uRoundingPower);
    if (uCutoutEnabled != 0) {
        float cutout = roundedRectDistance(
            pixel,
            uCutoutRect,
            uCutoutRadii,
            uRoundingPower);
        distance = max(distance, -cutout);
    }
    for (int index = 0; index < MAX_PARTS; ++index) {
        if (index >= uPartCount)
            break;
        if (uPartRects[index].z <= 0.0 ||
            uPartRects[index].w <= 0.0)
            continue;
        float part = roundedRectDistance(
            pixel,
            uPartRects[index],
            uPartRadii[index],
            uRoundingPower);
        float junction = max(
            max(
                uPartJunctions[index].x,
                uPartJunctions[index].y),
            max(
                uPartJunctions[index].z,
                uPartJunctions[index].w));
        distance = smoothUnion(distance, part, junction);
    }
    for (int index = 0; index < MAX_CONNECTORS; ++index) {
        if (index >= uConnectorCount)
            break;
        if (uConnectorRects[index].z <= 0.0 ||
            uConnectorRects[index].w <= 0.0)
            continue;
        distance = smoothUnion(
            distance,
            rectangleDistance(
                pixel,
                uConnectorRects[index]),
            uConnectorCurve);
    }
    return distance;
}

float shapeDistance(vec2 pixel) {
    if (uShapeKind == 1)
        return ringDistance(pixel);
    if (uShapeKind == 2)
        return compoundDistance(pixel);
    return simpleRoundedDistance(pixel);
}

float compoundCoverage(vec2 pixel, float antialias) {
    float coverage = 0.0;
    if (uBaseEnabled != 0) {
        float base = roundedRectDistance(
            pixel,
            vec4(0.0, 0.0, uFullSize.x, uFullSize.y),
            uBaseRadii,
            uRoundingPower);
        float baseCoverage =
            1.0 - smoothstep(-antialias, antialias, base);
        if (uCutoutEnabled != 0) {
            float cutout = roundedRectDistance(
                pixel,
                uCutoutRect,
                uCutoutRadii,
                uRoundingPower);
            baseCoverage *= smoothstep(
                -antialias,
                antialias,
                cutout);
        }
        coverage = max(coverage, baseCoverage);
    }
    for (int index = 0; index < MAX_PARTS; ++index) {
        if (index >= uPartCount)
            break;
        if (uPartRects[index].z <= 0.0 ||
            uPartRects[index].w <= 0.0)
            continue;
        float distance = roundedRectDistance(
            pixel,
            uPartRects[index],
            uPartRadii[index],
            uRoundingPower);
        float partCoverage =
            1.0 - smoothstep(-antialias, antialias, distance);
        coverage = max(coverage, partCoverage);
    }
    for (int index = 0; index < MAX_CONNECTORS; ++index) {
        if (index >= uConnectorCount)
            break;
        if (uConnectorRects[index].z <= 0.0 ||
            uConnectorRects[index].w <= 0.0)
            continue;
        float distance = rectangleDistance(
            pixel,
            uConnectorRects[index]);
        coverage = max(
            coverage,
            1.0 - smoothstep(
                -antialias,
                antialias,
                distance));
    }
    return coverage;
}

vec2 shapeNormal(vec2 pixel) {
    const float stepSize = 1.5;
    float dx = shapeDistance(
        pixel + vec2(stepSize, 0.0)) -
        shapeDistance(pixel - vec2(stepSize, 0.0));
    float dy = shapeDistance(
        pixel + vec2(0.0, stepSize)) -
        shapeDistance(pixel - vec2(0.0, stepSize));
    return normalize(vec2(dx, dy) + vec2(1e-6));
}

vec2 captureCoordinate(vec2 localPixel) {
    vec2 clipped = (localPixel - uClipOffset) /
        max(uClipSize, vec2(1.0));
    vec2 top = mix(uSourceTL, uSourceTR, clipped.x);
    vec2 bottom = mix(uSourceBL, uSourceBR, clipped.x);
    return mix(top, bottom, clipped.y);
}

vec3 captured(vec2 localPixel) {
    return texture(uCapture, captureCoordinate(localPixel)).rgb;
}

vec3 frosted(vec2 localPixel) {
    float radius = max(uBlurPixels, 0.0);
    if (radius < 0.5)
        return captured(localPixel);
    vec2 quarter = vec2(radius * 0.25);
    vec2 halfRadius = vec2(radius * 0.5);
    vec2 fullRadius = vec2(radius);
    vec3 color = captured(localPixel) * 0.20;
    color += captured(localPixel + vec2(quarter.x, 0.0)) * 0.10;
    color += captured(localPixel - vec2(quarter.x, 0.0)) * 0.10;
    color += captured(localPixel + vec2(0.0, quarter.y)) * 0.10;
    color += captured(localPixel - vec2(0.0, quarter.y)) * 0.10;
    color += captured(localPixel + halfRadius) * 0.075;
    color += captured(localPixel - halfRadius) * 0.075;
    color += captured(localPixel + vec2(halfRadius.x, -halfRadius.y)) * 0.075;
    color += captured(localPixel + vec2(-halfRadius.x, halfRadius.y)) * 0.075;
    color += captured(localPixel + vec2(fullRadius.x, 0.0)) * 0.025;
    color += captured(localPixel - vec2(fullRadius.x, 0.0)) * 0.025;
    color += captured(localPixel + vec2(0.0, fullRadius.y)) * 0.025;
    color += captured(localPixel - vec2(0.0, fullRadius.y)) * 0.025;
    return color;
}

void main() {
    vec2 localPixel = uClipOffset + vTexcoord * uClipSize;
    float distance = shapeDistance(localPixel);
    float antialias = max(fwidth(distance), 0.001);
    float coverage = uShapeKind == 2
        ? compoundCoverage(localPixel, antialias)
        : 1.0 - smoothstep(-antialias, antialias, distance);
    if (coverage <= 0.001)
        discard;

    float depth = max(-distance, 0.0);
    vec2 normal = shapeNormal(localPixel);
    float edgeBand = max(uEdgeBandPixels, 0.001);
    float edge = 1.0 - smoothstep(0.0, edgeBand, depth);
    float softIn = smoothstep(0.0, 3.0, depth);
    float bend = edge * edge * softIn;

    float displacement = bend * uRefractionPixels;
    if (uShapeKind == 2)
        displacement = min(displacement, depth * 1.2 + 2.0);
    vec2 samplePixel =
        localPixel - normal * displacement;

    float lensBand = max(uLensBandPixels, 1.0);
    float lensReach =
        (1.0 - smoothstep(0.0, lensBand, depth)) *
        softIn;
    samplePixel +=
        -normal * lensReach * lensReach *
        uLens * lensBand;

    vec3 glass = frosted(samplePixel);
    float chromaPixels =
        bend * uChroma * uRefractionPixels;
    if (chromaPixels > 0.05) {
        glass.r = mix(
            glass.r,
            frosted(samplePixel + normal * chromaPixels).r,
            0.7);
        glass.b = mix(
            glass.b,
            frosted(samplePixel - normal * chromaPixels).b,
            0.7);
    }

    float sourceLuma =
        dot(glass, vec3(0.299, 0.587, 0.114));
    vec3 sourceChroma = glass - vec3(sourceLuma);
    float veilLuma =
        dot(uTint.rgb, vec3(0.299, 0.587, 0.114));
    float backgroundCurve = clamp(
        sourceLuma +
            0.7 * sourceLuma * (1.0 - sourceLuma),
        0.0,
        1.0);
    float newLuma = mix(
        backgroundCurve,
        veilLuma,
        clamp(uTint.a, 0.0, 1.0));
    vec3 tintChroma = uTint.rgb - vec3(veilLuma);
    glass = clamp(
        vec3(newLuma) +
            sourceChroma *
                clamp(uVeilSaturation, 0.0, 1.0) +
            tintChroma *
                clamp(uTint.a, 0.0, 1.0),
        0.0,
        1.0);

    float vertical = normal.y * normal.y;
    float horizontal = normal.x * normal.x;
    float finishBand = min(
        max(uBevelPixels, 0.001),
        max(uRimWidthPixels * 1.5, 3.0));
    float finish =
        1.0 - smoothstep(0.0, finishBand, depth);
    glass = mix(
        glass,
        vec3(1.0),
        finish * vertical * uHighlight);
    glass *=
        1.0 - finish * horizontal * uShadow;
    glass *=
        1.0 - pow(edge, 2.2) * uEdgeDepth;

    float light = clamp(
        dot(normal, uLightDirection) * 0.5 + 0.5,
        0.0,
        1.0);
    float sheen =
        light * edge * edge * uGloss;
    vec3 sheenColor = mix(
        vec3(1.0),
        min(glass * 1.7, vec3(1.0)),
        0.6);
    glass = mix(glass, sheenColor, sheen);

    float rim =
        1.0 - smoothstep(
            0.0,
            max(uRimWidthPixels, 0.001),
            depth);
    glass = mix(
        glass,
        vec3(1.0),
        rim * vertical * uSpecular);
    glass = mix(
        glass,
        vec3(0.0),
        rim * horizontal * uSpecular);

    float materialOpacity = 1.0;
    if (uShapeKind == 2) {
        float cutoutInterior = 1.0;
        if (uCutoutEnabled != 0) {
            float cutoutDistance = roundedRectDistance(
                localPixel,
                uCutoutRect,
                uCutoutRadii,
                uRoundingPower);
            cutoutInterior =
                1.0 - smoothstep(-12.0, 0.0, cutoutDistance);
        }
        for (int index = 0; index < MAX_PARTS; ++index) {
            if (index >= uPartCount)
                break;
            float partOpacity =
                clamp(uPartOpacity[index], 0.0, 1.0);
            if (partOpacity >= 0.999)
                continue;
            float extentDistance = rectangleDistance(
                localPixel,
                uPartMaterialExtents[index]);
            float influence =
                (1.0 - smoothstep(
                    0.0,
                    24.0,
                    max(extentDistance, 0.0))) *
                cutoutInterior;
            materialOpacity = min(
                materialOpacity,
                mix(1.0, partOpacity, influence));
        }
    }

    float alpha =
        clamp(uOpacity, 0.0, 1.0) *
        coverage *
        materialOpacity;
    fragColor = vec4(glass * alpha, alpha);
}
)GLSL";
}

} // namespace hfg::v2
