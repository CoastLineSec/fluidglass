// ─────────────────────────────────────────────────────────────────────────
//  fluidglass — live fluid-glass compositor material for Hyprland
//
//  A client (e.g. a shell/bar) sends element geometry over hyprctl; this plugin
//  captures the real framebuffer behind each element at RENDER_POST_WINDOWS and
//  runs the fluid-glass shader over it — refraction, frost, bevel, specular rim
//  and an optional cursor-tracked point light. No coordinate guessing: the
//  client provides the exact rect (or anchors it to a window) and we render it.
//
//  Material model: glass-level → blur/tint, with pixel params scaled as a ratio
//  of the element's min-dimension, capped at a 200px design reference, then by
//  the monitor scale. Blur and tint can be driven independently (blurLevel /
//  tintLevel) or derived together from glassLevel.
//
//  Handles rotated/flipped monitors: the backdrop is captured in the monitor's
//  physical pixel basis and element corners are mapped through the inverse
//  monitor transform (displayExtentForCapture / inverseTransformPoint). The raw
//  currentFB capture (captureBackdropForCurrentMonitor / captureFBFor) is
//  feedback-safe; both paths are load-bearing — change with care.
//
//  IPC: see README.md for the hyprctl dispatcher contract and payload schema.
// ─────────────────────────────────────────────────────────────────────────

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/desktop/view/Window.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef FLUIDGLASS_PLUGIN_VERSION
#define FLUIDGLASS_PLUGIN_VERSION "0.1.0"
#endif

namespace {

using json = nlohmann::json;

// ── Globals ───────────────────────────────────────────────────────────────
HANDLE              g_handle = nullptr;
SP<SHyprCtlCommand> g_statusCommand;
SP<SHyprCtlCommand> g_applyCommand;
SP<SHyprCtlCommand> g_clearCommand;
SP<SHyprCtlCommand> g_materialCommand;
CHyprSignalListener g_renderStageListener;

std::mutex   g_stateMutex;
bool         g_enabled = false;          // global on/off
std::string  g_lastApplyStatus = "none";
std::string  g_lastError;
std::string  g_lastRenderStatus = "disabled";
uint64_t     g_generation = 0;

SP<CShader> g_shader;
bool        g_shaderAttempted = false;
bool        g_shaderCompiled  = false;
std::string g_shaderError;

// One glass surface the shell asked us to draw.
struct GlassElement {
    std::string id;
    std::string monitor;          // monitor name
    double      x = 0, y = 0;      // monitor-local LOGICAL px (top-left)
    double      w = 0, h = 0;      // LOGICAL px
    double      radius = 0;        // LOGICAL px
    double      glassLevel = 0.5;  // 0..1 → blur + tint amount
    double      blurLevel  = -1.0; // custom frost 0..1; <0 = derive from glassLevel (preset)
    double      tintLevel  = -1.0; // custom tint  0..1; <0 = derive from glassLevel (preset)
    bool        tintEnabled = false;
    float       tintR = 1.0F, tintG = 1.0F, tintB = 1.0F;
    // Locked material (design-px at the 200px ref); overridable per element for live tuning.
    double      refraction = 45.0, rimBand = 40.0, bevel = 46.0, rimWidth = 3.0;
    double      highlight = 0.10, shadow = 0.10, lightDeg = 90.0, specular = 0.21, rimWrap = 0.45;
    // Mouse tracking — ONE master toggle gates the point light + distance falloff. OFF = static glass
    // (highlight/shadow at the value above). ON = point light, and highlight/shadow swing around that
    // value (the midpoint): up to peak (= 2*value - floor) at the bar, down to floor far away.
    bool        lightFollowsMouse = false;
    double      falloffReach = 1000.0;       // px; distance over which the tracking brightness fades
    double      falloffFloor = 0.05;         // highlight/shadow strength far away (peak = 2*value - floor)
    double      lightTightness = 1.0;        // point-light spot exponent (1 = natural, higher = tighter)
    // Window anchoring — the Labs preview lives in a floating window the shell can't self-locate.
    // When anchorWindow is set, monitor/x/y are recomputed each frame from the window's live
    // server-side position + offset, so the glass tracks the window. Generic: any window can host glass.
    std::string anchorWindow;                 // getWindowByRegex selector; empty = use monitor + x/y directly
    double      offsetX = 0.0, offsetY = 0.0; // glass-rect offset within the window (logical px)
};
std::map<std::string, GlassElement> g_elements;
std::map<std::string, std::string>  g_elementDebug;   // per-element last draw outcome (diagnostic)

// Per-monitor raw backdrop capture (currentFB copy), produced each frame.
std::map<std::string, SP<Render::IFramebuffer>> g_captureFBs;

struct Pt { double x = 0, y = 0; };

// ── Small helpers ─────────────────────────────────────────────────────────
double clampd(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

std::string trim(std::string v) {
    const auto a = v.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const auto b = v.find_last_not_of(" \t\r\n");
    return v.substr(a, b - a + 1);
}
std::string removePrefix(std::string v, const std::string& p) {
    if (v.rfind(p, 0) == 0) v = v.substr(p.size());
    return trim(std::move(v));
}
std::string lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
    return v;
}
double jnum(const json& o, std::string_view k, double def = 0.0) {
    if (auto it = o.find(k); it != o.end() && it->is_number()) return it->get<double>();
    return def;
}
bool jbool(const json& o, std::string_view k, bool def = false) {
    if (auto it = o.find(k); it != o.end() && it->is_boolean()) return it->get<bool>();
    return def;
}
std::string jstr(const json& o, std::string_view k) {
    if (auto it = o.find(k); it != o.end() && it->is_string()) return it->get<std::string>();
    return "";
}
void parseHex(const std::string& hex, float& r, float& g, float& b) {
    if (hex.size() < 7 || hex[0] != '#') return;
    auto byte = [&](int i) { return static_cast<float>(std::stoi(hex.substr(i, 2), nullptr, 16)) / 255.0F; };
    try { r = byte(1); g = byte(3); b = byte(5); } catch (...) {}
}

// ── Locked material + size-scaling (see fluid-glass-rebuild memory) ────────
// Pixel params are RATIOS of the element's min-dimension, capped at a 200px
// design reference, then × monitorScale. Strengths/angle are size-independent.
namespace mat {
    constexpr double REF_PX  = 200.0;   // design reference (px); pixel params scale to this
    constexpr double BLUR_LO = 6.0;     // glass-level 0 (design px)
    constexpr double BLUR_HI = 22.0;    // glass-level 1
    constexpr double TINT_LO = 0.04;    // glass-level 0
    constexpr double TINT_HI = 0.30;    // glass-level 1
}

struct ResolvedParams {
    double blurPx, refractPx, rimBandPx, bevelPx, rimWidthPx, radiusPx;
    double highlight, shadow, specular, rimWrap, tintStrength;
    double lightX, lightY;
    float  tintR, tintG, tintB;
};

ResolvedParams resolveParams(const GlassElement& el, double scale) {
    const double t       = clampd(el.glassLevel, 0.0, 1.0);
    const double bt      = (el.blurLevel >= 0.0) ? clampd(el.blurLevel, 0.0, 1.0) : t;  // custom or preset frost
    const double tt      = (el.tintLevel >= 0.0) ? clampd(el.tintLevel, 0.0, 1.0) : t;  // custom or preset tint
    const double effDim  = std::min(std::min(el.w, el.h), mat::REF_PX);  // cap at the design ref
    const double sizeFac = (effDim / mat::REF_PX) * scale;               // design px -> physical px
    auto px = [&](double designPx) { return designPx * sizeFac; };

    ResolvedParams r;
    r.blurPx     = px(mat::BLUR_LO + (mat::BLUR_HI - mat::BLUR_LO) * bt);
    r.refractPx  = px(el.refraction);
    r.rimBandPx  = px(el.rimBand);
    r.bevelPx    = px(el.bevel);
    r.rimWidthPx = std::max(1.0, px(el.rimWidth));
    r.radiusPx   = el.radius * scale;
    r.highlight  = el.highlight;
    r.shadow     = el.shadow;
    r.specular   = el.specular;
    r.rimWrap    = el.rimWrap;
    const double a = el.lightDeg * M_PI / 180.0;
    r.lightX = std::cos(a);
    r.lightY = std::sin(a);
    r.tintStrength = el.tintEnabled ? (mat::TINT_LO + (mat::TINT_HI - mat::TINT_LO) * tt) : 0.0;
    r.tintR = el.tintR; r.tintG = el.tintG; r.tintB = el.tintB;
    return r;
}

// ── Transform map (PRESERVED — handles rotated/flipped displays) ───────────
// The captured backdrop lives in the monitor's PHYSICAL pixel basis (raw GPU
// framebuffer, no transform applied). Element corners arrive in transformed
// DISPLAY space. On a rotated/flipped monitor those differ, so we map each
// corner into the physical texture basis via the INVERSE monitor transform.
bool transformSwapsAxes(int t) { return t == 1 || t == 3 || t == 5 || t == 7; }

Pt displayExtentForCapture(double cw, double ch, int transform) {
    return transformSwapsAxes(transform) ? Pt{ch, cw} : Pt{cw, ch};
}

Pt inverseTransformPoint(Pt p, int transform, double cw, double ch) {
    const Pt   ext     = displayExtentForCapture(cw, ch, transform);
    const auto inverse = Math::wlTransformToHyprutils(Math::invertTransform(static_cast<wl_output_transform>(transform)));
    const double x = p.x, y = p.y, w = ext.x, h = ext.y;
    switch (static_cast<int>(inverse)) {
        case 0: return {x, y};
        case 1: return {h - y, x};
        case 2: return {w - x, h - y};
        case 3: return {y, w - x};
        case 4: return {w - x, y};
        case 5: return {h - y, w - x};
        case 6: return {x, h - y};
        case 7: return {y, x};
        default: return {x, y};
    }
}

// ── Shader ─────────────────────────────────────────────────────────────────
const char* vertexSource() {
    return R"GLSL(#version 320 es
uniform mat3 proj;
in vec2 pos;
in vec2 texcoord;
out vec2 v_texcoord;
void main() {
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
    v_texcoord = texcoord;
}
)GLSL";
}

const char* fragmentSource() {
    return R"GLSL(#version 320 es
precision highp float;
in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

uniform sampler2D tex;
uniform vec2  uSrcTL;       // capture-texture UV of the element's 4 corners
uniform vec2  uSrcTR;
uniform vec2  uSrcBR;
uniform vec2  uSrcBL;
uniform vec2  uDestSize;    // element size, px
uniform float uRadiusPx;
uniform float uBlurPx;      // frost
uniform float uRefractPx;   // edge lensing
uniform float uEdgeBandPx;  // rim band
uniform float uBevelPx;     // bevel band
uniform float uHighlight;
uniform float uShadow;
uniform vec2  uLightDir;    // direction toward light (directional mode)
uniform vec2  uLightPos;    // point-light position in element px (mouse mode)
uniform float uPointLight;  // 1 = point light at uLightPos, else directional uLightDir
uniform float uTightness;   // highlight/shadow spot exponent (1 = natural)
uniform float uSpecular;
uniform float uRimWidthPx;
uniform float uRimWrap;
uniform vec4  uTint;        // rgb + strength
uniform float uAlpha;

float sdRoundRect(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - (b - vec2(r));
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// local 0..1 surface uv -> capture-texture uv (bilinear across the 4 corners,
// so the transform-mapped corners carry rotated/flipped displays correctly).
vec2 srcUv(vec2 local) {
    vec2 c = clamp(local, 0.0, 1.0);
    vec2 t = mix(uSrcTL, uSrcTR, c.x);
    vec2 b = mix(uSrcBL, uSrcBR, c.x);
    return mix(t, b, c.y);
}

// Interleaved-gradient-noise dither angle. Rotating each fragment's sample
// grid breaks the regular pattern, so frost reads smooth over sharp content
// (windows/text) instead of a screen-door mesh.
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

// Controlled Gaussian frost, in local surface uv; radius in element px.
vec3 frost(vec2 local, float radiusPx) {
    if (radiusPx < 0.5) return texture(tex, srcUv(local)).rgb;
    vec2  toUv  = 1.0 / max(uDestSize, vec2(1.0));
    const int K = 5;
    float step  = radiusPx / float(K);
    float sigma = max(radiusPx * 0.5, 1e-3);
    float ang   = ign(local * uDestSize) * 6.2831853;
    mat2  rot   = mat2(cos(ang), -sin(ang), sin(ang), cos(ang));
    vec3  sum   = vec3(0.0);
    float wsum  = 0.0;
    for (int j = -K; j <= K; ++j)
    for (int i = -K; i <= K; ++i) {
        vec2  o   = rot * (vec2(float(i), float(j)) * step);
        float wgt = exp(-dot(o, o) / (2.0 * sigma * sigma));
        sum  += texture(tex, srcUv(local + o * toUv)).rgb * wgt;
        wsum += wgt;
    }
    return sum / max(wsum, 1e-4);
}

void main() {
    vec2  localUv = clamp(v_texcoord, 0.0, 1.0);
    vec2  localPx = localUv * uDestSize;
    vec2  half_   = uDestSize * 0.5;
    float r       = min(uRadiusPx, min(half_.x, half_.y));
    vec2  p       = localPx - half_;
    float d       = sdRoundRect(p, half_, r);

    float aa     = max(fwidth(d), 0.001);
    float inside = 1.0 - smoothstep(-aa, aa, d);
    if (inside <= 0.001)
        discard;

    // Edge lensing — outward normal from the SDF gradient.
    vec2  e = vec2(1.0, 0.0);
    vec2  n = normalize(vec2(
                  sdRoundRect(p + e.xy, half_, r) - sdRoundRect(p - e.xy, half_, r),
                  sdRoundRect(p + e.yx, half_, r) - sdRoundRect(p - e.yx, half_, r)
              ) + 1e-6);
    float depth = -d;
    float edge  = 1.0 - smoothstep(0.0, max(uEdgeBandPx, 0.001), depth);
    float bend  = edge * edge;
    vec2  uv    = localUv - n * bend * (uRefractPx / max(uDestSize, vec2(1.0)));

    vec3 glass = frost(uv, uBlurPx);

    // Tint (before lighting, so the highlight + specular stay bright on top).
    glass = mix(glass, uTint.rgb, clamp(uTint.a, 0.0, 1.0));

    // Convex bevel — inner highlight + inner shadow. The light is a fixed direction,
    // or a POINT at the cursor (uLightPos): the per-fragment direction makes the
    // highlight localise near the mouse and broaden with distance (point-light cosine).
    vec2  L = (uPointLight > 0.5) ? (uLightPos - localPx) : uLightDir;
    float towardLight = dot(n, normalize(L + 1e-6));
    float lit   = pow(clamp( towardLight, 0.0, 1.0), max(uTightness, 0.01));
    float shd   = pow(clamp(-towardLight, 0.0, 1.0), max(uTightness, 0.01));
    float bevel = 1.0 - smoothstep(0.0, max(uBevelPx, 0.001), depth);
    float hi    = bevel * lit * uHighlight;
    float sh    = bevel * shd * uShadow;
    glass = mix(glass, vec3(1.0), hi);
    glass *= (1.0 - sh);

    // Specular rim — thin bright glint at the very edge.
    float rim     = 1.0 - smoothstep(0.0, max(uRimWidthPx, 0.001), depth);
    float litWrap = uRimWrap + (1.0 - uRimWrap) * clamp(towardLight, 0.0, 1.0);
    glass = mix(glass, vec3(1.0), rim * litWrap * uSpecular);

    float a = clamp(uAlpha, 0.0, 1.0) * inside;
    fragColor = vec4(glass * a, a);   // premultiplied
}
)GLSL";
}

bool ensureShader() {
    if (g_shader && g_shaderCompiled) return true;
    if (g_shaderAttempted && !g_shaderCompiled) return false;
    g_shaderAttempted = true;
    if (!Render::GL::g_pHyprOpenGL) { g_shaderError = "OpenGL renderer unavailable"; return false; }

    auto shader = makeShared<CShader>();
    if (!shader || !shader->createProgram(vertexSource(), fragmentSource(), true, true)) {
        g_shader.reset();
        g_shaderError    = "shader compile/link failed";
        g_shaderCompiled = false;
        return false;
    }
    shader->setUsesCustomUV(true);
    g_shader         = shader;
    g_shaderCompiled = true;
    g_shaderError.clear();
    return true;
}

// ── Raw backdrop capture (PRESERVED — feedback-safe currentFB copy) ────────
SP<Render::IFramebuffer> captureFBFor(const PHLMONITOR& monitor, const SP<Render::IFramebuffer>& sourceFB) {
    if (!g_pHyprRenderer || !monitor || !sourceFB) return nullptr;
    auto& fb = g_captureFBs[monitor->m_name];
    if (!fb) fb = g_pHyprRenderer->createFB("fluidglass backdrop " + monitor->m_name);
    if (!fb) return nullptr;

    const auto srcTex = sourceFB->getTexture();
    if (!srcTex || !srcTex->ok()) return nullptr;
    const int width  = static_cast<int>(std::round(srcTex->m_size.x));
    const int height = static_cast<int>(std::round(srcTex->m_size.y));
    if (width <= 0 || height <= 0) return nullptr;

    auto format = sourceFB->m_drmFormat;
    if (format == DRM_FORMAT_INVALID) format = DRM_FORMAT_ABGR8888;
    fb->alloc(width, height, format);
    if (const auto imgDesc = sourceFB->imageDescription())
        fb->setImageDescription(imgDesc);
    else
        fb->setImageDescription(monitor->workBufferImageDescription());
    return fb;
}

SP<Render::ITexture> captureBackdropForCurrentMonitor() {
    if (!g_pHyprRenderer) return nullptr;
    const auto monitor = g_pHyprRenderer->renderData().pMonitor.lock();
    if (!monitor) return nullptr;

    const auto sourceFB = g_pHyprRenderer->renderData().currentFB;
    if (!sourceFB || !sourceFB->isAllocated() || !sourceFB->getTexture()) return nullptr;
    const auto captureFB = captureFBFor(monitor, sourceFB);
    if (!captureFB || !captureFB->isAllocated()) return nullptr;
    const auto srcTex = sourceFB->getTexture();
    if (!srcTex || !srcTex->ok()) return nullptr;

    const CBox srcBox = {0, 0, srcTex->m_size.x, srcTex->m_size.y};
    if (srcBox.width <= 0 || srcBox.height <= 0) return nullptr;

    {
        auto guard           = g_pHyprRenderer->bindTempFB(captureFB);
        const auto oldProj   = g_pHyprRenderer->m_renderData.projectionType;
        const auto oldFbSize = g_pHyprRenderer->m_renderData.fbSize;
        const auto oldTfDmg  = g_pHyprRenderer->m_renderData.transformDamage;

        g_pHyprRenderer->m_renderData.fbSize = Vector2D{static_cast<double>(srcBox.width), static_cast<double>(srcBox.height)};
        g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
        g_pHyprRenderer->m_renderData.transformDamage = false;
        g_pHyprRenderer->setViewport(0, 0, srcBox.width, srcBox.height);
        g_pHyprRenderer->blend(false);

        CTexPassElement::SRenderData copy;
        copy.tex           = srcTex;
        copy.box           = srcBox;
        copy.a             = 1.0F;
        copy.damage        = CRegion{CBox(0, 0, srcBox.width, srcBox.height)};
        copy.allowCustomUV = false;
        copy.wrapX         = WRAP_CLAMP_TO_EDGE;
        copy.wrapY         = WRAP_CLAMP_TO_EDGE;
        g_pHyprRenderer->draw(copy, copy.damage);

        g_pHyprRenderer->blend(true);
        g_pHyprRenderer->m_renderData.fbSize          = oldFbSize;
        g_pHyprRenderer->m_renderData.transformDamage = oldTfDmg;
        g_pHyprRenderer->setProjectionType(oldProj);
        g_pHyprRenderer->setViewport(0, 0, static_cast<int>(monitor->m_pixelSize.x), static_cast<int>(monitor->m_pixelSize.y));
    }

    const auto tex = captureFB->getTexture();
    return (tex && tex->ok()) ? tex : nullptr;
}

// ── Draw one glass element ────────────────────────────────────────────────
void drawElement(const GlassElement& el, const SP<Render::ITexture>& capture) {
    if (!g_pHyprRenderer || !capture || !capture->ok()) return;
    if (!ensureShader()) return;
    const auto monitor = g_pHyprRenderer->renderData().pMonitor.lock();
    if (!monitor || monitor->m_scale <= 0) return;

    const double scale = monitor->m_scale;
    const int    tf    = static_cast<int>(monitor->m_transform);
    const double cw    = capture->m_size.x;
    const double ch    = capture->m_size.y;
    if (cw <= 0 || ch <= 0 || el.w <= 0 || el.h <= 0) return;

    // Element rect in display-space physical px (monitor-local).
    const double dx = el.x * scale, dy = el.y * scale;
    const double dw = el.w * scale, dh = el.h * scale;

    // Source-texture UV of the 4 corners (transform-mapped for rotated displays).
    auto cornerUv = [&](double px, double py) -> Pt {
        Pt phys = (tf == 0) ? Pt{px, py} : inverseTransformPoint({px, py}, tf, cw, ch);
        return {clampd(phys.x / cw, 0.0, 1.0), clampd(phys.y / ch, 0.0, 1.0)};
    };
    const Pt uvTL = cornerUv(dx, dy);
    const Pt uvTR = cornerUv(dx + dw, dy);
    const Pt uvBR = cornerUv(dx + dw, dy + dh);
    const Pt uvBL = cornerUv(dx, dy + dh);

    CBox box(dx, dy, dw, dh);
    CRegion overlap{g_pHyprRenderer->m_renderData.damage};
    overlap.intersect(box.x, box.y, box.width, box.height);
    if (overlap.empty()) return;          // backdrop under the glass didn't change
    CRegion boxRegion{box};

    CBox projected = box;
    g_pHyprRenderer->m_renderData.renderModif.applyToBox(projected);

    auto transform = capture->m_transform;
    if (g_pHyprRenderer->monitorTransformEnabled()) {
        const auto inv = Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform));
        transform = Math::composeTransform(inv, transform);
    }
    const auto glMatrix = g_pHyprRenderer->projectBoxToTarget(projected, transform);

    auto shader = Render::GL::g_pHyprOpenGL->useShader(g_shader);
    if (!shader || shader->program() == 0) return;

    glActiveTexture(GL_TEXTURE0);
    capture->bind();
    capture->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    capture->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    capture->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    capture->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    const ResolvedParams rp = resolveParams(el, scale);

    // Light direction: fixed angle, or — the "gyroscope" — following the cursor, so the
    // specular highlight sweeps around the glass as the mouse moves (Apple's tilt trick).
    // Light: fixed directional, or — mouse-follow — a POINT light AT the cursor.
    double falloffT   = 0.0;                        // 0 at the bar -> 1 far (brightness fade)
    double lightPosX  = 0.0, lightPosY = 0.0;       // cursor in element-local px
    float  pointLight = 0.0F;
    if (el.lightFollowsMouse && g_pInputManager) {
        const Vector2D cursor = g_pInputManager->getMouseCoordsInternal();
        const double elemLeft = monitor->m_position.x + el.x;
        const double elemTop  = monitor->m_position.y + el.y;
        lightPosX  = (cursor.x - elemLeft) * scale;
        lightPosY  = (cursor.y - elemTop)  * scale;
        pointLight = 1.0F;
        if (el.falloffReach > 1.0) {                // brightness fades with distance from the bar
            const double dxm = cursor.x - (elemLeft + el.w * 0.5);
            const double dym = cursor.y - (elemTop  + el.h * 0.5);
            const double qx  = std::max(std::abs(dxm) - el.w * 0.5, 0.0);
            const double qy  = std::max(std::abs(dym) - el.h * 0.5, 0.0);
            const double s   = clampd(std::sqrt(qx * qx + qy * qy) / el.falloffReach, 0.0, 1.0);
            falloffT = s * s * (3.0 - 2.0 * s);     // smoothstep
        }
    }

    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, glMatrix.getMatrix());
    shader->setUniformInt(SHADER_TEX, 0);
    shader->setUniformFloat(SHADER_ALPHA, 1.0F);

    const GLuint program = shader->program();
    auto u1 = [&](const char* n, float v) { const GLint l = glGetUniformLocation(program, n); if (l >= 0) glUniform1f(l, v); };
    auto u2 = [&](const char* n, float x, float y) { const GLint l = glGetUniformLocation(program, n); if (l >= 0) glUniform2f(l, x, y); };
    auto u4 = [&](const char* n, float x, float y, float z, float w) { const GLint l = glGetUniformLocation(program, n); if (l >= 0) glUniform4f(l, x, y, z, w); };

    u2("uSrcTL", static_cast<float>(uvTL.x), static_cast<float>(uvTL.y));
    u2("uSrcTR", static_cast<float>(uvTR.x), static_cast<float>(uvTR.y));
    u2("uSrcBR", static_cast<float>(uvBR.x), static_cast<float>(uvBR.y));
    u2("uSrcBL", static_cast<float>(uvBL.x), static_cast<float>(uvBL.y));
    u2("uDestSize", static_cast<float>(dw), static_cast<float>(dh));
    u1("uRadiusPx", static_cast<float>(rp.radiusPx));
    u1("uBlurPx", static_cast<float>(rp.blurPx));
    u1("uRefractPx", static_cast<float>(rp.refractPx));
    u1("uEdgeBandPx", static_cast<float>(rp.rimBandPx));
    u1("uBevelPx", static_cast<float>(rp.bevelPx));
    // Mouse tracking: highlight/shadow swing around their static value (the midpoint) — up to
    // peak (= 2*value - floor) at the bar, down to floor far away. Static when tracking is off.
    auto bandValue = [&](double mid) -> double {
        if (pointLight <= 0.5F) return mid;                        // tracking off → static value
        const double peak = 2.0 * mid - el.falloffFloor;
        return peak + (el.falloffFloor - peak) * falloffT;        // peak at the bar → floor far away
    };
    u1("uHighlight", static_cast<float>(bandValue(rp.highlight)));
    u1("uShadow",    static_cast<float>(bandValue(rp.shadow)));
    // Shader Y is mirrored vs the lab → flip light-Y so 90° = top (matches the lab).
    u2("uLightDir", static_cast<float>(rp.lightX), static_cast<float>(-rp.lightY));
    u2("uLightPos", static_cast<float>(lightPosX), static_cast<float>(lightPosY));
    u1("uPointLight", pointLight);
    u1("uTightness", static_cast<float>(el.lightTightness));
    u1("uSpecular", static_cast<float>(rp.specular));
    u1("uRimWidthPx", static_cast<float>(rp.rimWidthPx));
    u1("uRimWrap", static_cast<float>(rp.rimWrap));
    u4("uTint", rp.tintR, rp.tintG, rp.tintB, static_cast<float>(rp.tintStrength));
    u1("uAlpha", 1.0F);

    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    glBindBuffer(GL_ARRAY_BUFFER, shader->getUniformLocation(SHADER_SHADER_VBO));
    auto verts = Render::GL::fullVerts;
    verts[0].u = 0.0F; verts[0].v = 0.0F;
    verts[1].u = 0.0F; verts[1].v = 1.0F;
    verts[2].u = 1.0F; verts[2].v = 0.0F;
    verts[3].u = 1.0F; verts[3].v = 1.0F;
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), nullptr, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts.data());

    // Redraw the WHOLE element whenever any of its backdrop changed, so the
    // frost/refraction recompute consistently (no left-behind window edges).
    Render::GL::g_pHyprOpenGL->blend(true);
    boxRegion.forEachRect([](const auto& rect) {
        Render::GL::g_pHyprOpenGL->scissor(&rect, g_pHyprRenderer->m_renderData.transformDamage);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    });
    Render::GL::g_pHyprOpenGL->scissor(nullptr);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    capture->unbind();
}

// ── Render-pass injection ─────────────────────────────────────────────────
class CCapturePass : public IPassElement {
  public:
    bool needsLiveBlur() override { return false; }
    bool needsPrecomputeBlur() override { return false; }
    const char* passName() override { return "FluidGlassCapture"; }
    ePassElementType type() override { return EK_CUSTOM; }
    std::optional<CBox> boundingBox() override { return std::nullopt; }
    CRegion opaqueRegion() override { return {}; }
    std::vector<UP<IPassElement>> draw() override {
        g_capture = captureBackdropForCurrentMonitor();
        return {};
    }
    static SP<Render::ITexture> g_capture;
};
SP<Render::ITexture> CCapturePass::g_capture = nullptr;

class CGlassPass : public IPassElement {
  public:
    explicit CGlassPass(GlassElement el) : m_el(std::move(el)) {}
    bool needsLiveBlur() override { return false; }
    bool needsPrecomputeBlur() override { return false; }
    const char* passName() override { return "FluidGlassDraw"; }
    ePassElementType type() override { return EK_CUSTOM; }
    std::optional<CBox> boundingBox() override {
        if (!g_pHyprRenderer || !g_pHyprRenderer->m_renderData.pMonitor) return std::nullopt;
        return CBox(m_el.x, m_el.y, m_el.w, m_el.h).round();
    }
    CRegion opaqueRegion() override { return {}; }
    std::vector<UP<IPassElement>> draw() override {
        drawElement(m_el, CCapturePass::g_capture);
        return {};
    }
  private:
    GlassElement m_el;
};

// Resolve a window-anchored element to a live monitor + monitor-local position from
// the target window's server-side geometry. Returns false if the window isn't found
// or isn't mapped (caller skips it). Tracks the window as it moves/changes monitor.
bool resolveAnchor(GlassElement& el) {
    if (!g_pCompositor) return false;
    PHLWINDOW win = g_pCompositor->getWindowByRegex(el.anchorWindow);
    if (!win || !win->m_isMapped) return false;
    const auto mon = win->m_monitor.lock();
    if (!mon) return false;
    const Vector2D wpos = win->m_realPosition->value();   // global logical top-left
    el.monitor = mon->m_name;
    el.x = (wpos.x - mon->m_position.x) + el.offsetX;
    el.y = (wpos.y - mon->m_position.y) + el.offsetY;
    return true;
}

void renderFluidGlass(eRenderStage stage) {
    if (stage != RENDER_POST_WINDOWS) return;
    if (!g_pHyprRenderer) return;

    std::vector<GlassElement> here;
    {
        std::lock_guard guard(g_stateMutex);
        if (!g_enabled || g_elements.empty()) return;
        const auto monitor = g_pHyprRenderer->renderData().pMonitor.lock();
        if (!monitor) return;
        for (const auto& [id, el0] : g_elements) {
            GlassElement el = el0;
            if (!el.anchorWindow.empty() && !resolveAnchor(el))
                continue;
            if (el.monitor == monitor->m_name && el.w > 0 && el.h > 0)
                here.push_back(el);
        }
    }
    if (here.empty()) return;

    const auto mon = g_pHyprRenderer->renderData().pMonitor.lock();
    g_pHyprRenderer->m_renderPass.add(makeUnique<CCapturePass>());
    for (auto& el : here) {
        g_pHyprRenderer->m_renderPass.add(makeUnique<CGlassPass>(el));
        // Keep the glass live: re-damage the whole surface each frame so its full
        // area re-presents. Otherwise the frost/refraction spread past the damaged
        // strip leaves stale window-edge ghosts until a full repaint. (global-logical)
        if (mon)
            g_pHyprRenderer->damageBox(CBox(mon->m_position.x + el.x, mon->m_position.y + el.y, el.w, el.h));
    }
    g_lastRenderStatus = "ok";
}

void damageAllMonitors() {
    if (!g_pHyprRenderer) return;
    const auto& ms = State::monitorState();
    if (!ms) return;
    for (const auto& m : ms->monitors())
        if (m) g_pHyprRenderer->damageMonitor(m);
}

// ── IPC ───────────────────────────────────────────────────────────────────
//  apply payload: {"enabled":bool,
//                  "elements":[{id,monitor,x,y,w,h,radius,glassLevel,
//                               tintEnabled,tintColor}, ...]}
std::string applyPayload(std::string payload) {
    payload = trim(std::move(payload));
    if (payload.empty() || payload.front() != '{') {
        std::lock_guard g(g_stateMutex);
        g_lastApplyStatus = "rejected";
        g_lastError = "payload must be a JSON object";
        return "error: payload must be a JSON object\n";
    }
    json doc;
    try { doc = json::parse(payload); }
    catch (const std::exception& e) {
        std::lock_guard g(g_stateMutex);
        g_lastApplyStatus = "rejected";
        g_lastError = std::string("json parse: ") + e.what();
        return "error: " + g_lastError + "\n";
    }

    std::map<std::string, GlassElement> parsed;
    if (auto it = doc.find("elements"); it != doc.end() && it->is_array()) {
        for (const auto& e : *it) {
            if (!e.is_object()) continue;
            GlassElement el;
            el.id      = jstr(e, "id");
            el.monitor = jstr(e, "monitor");
            el.x = jnum(e, "x"); el.y = jnum(e, "y");
            el.w = jnum(e, "w"); el.h = jnum(e, "h");
            el.radius      = jnum(e, "radius");
            el.glassLevel  = jnum(e, "glassLevel", 0.5);
            el.tintEnabled = jbool(e, "tintEnabled", false);
            parseHex(jstr(e, "tintColor"), el.tintR, el.tintG, el.tintB);
            el.refraction  = jnum(e, "refraction", el.refraction);
            el.rimBand     = jnum(e, "rimBand",    el.rimBand);
            el.bevel       = jnum(e, "bevel",      el.bevel);
            el.rimWidth    = jnum(e, "rimWidth",   el.rimWidth);
            el.highlight   = jnum(e, "highlight",  el.highlight);
            el.shadow      = jnum(e, "shadow",     el.shadow);
            el.lightDeg    = jnum(e, "lightAngle", el.lightDeg);
            el.specular    = jnum(e, "specular",   el.specular);
            el.rimWrap     = jnum(e, "rimWrap",    el.rimWrap);
            el.lightFollowsMouse = jbool(e, "lightFollowsMouse", false);
            el.falloffReach = jnum(e, "falloffReach", el.falloffReach);
            el.falloffFloor = jnum(e, "falloffFloor", el.falloffFloor);
            el.lightTightness = jnum(e, "lightTightness", el.lightTightness);
            el.blurLevel    = jnum(e, "blurLevel", -1.0);
            el.tintLevel    = jnum(e, "tintLevel", -1.0);
            el.anchorWindow = jstr(e, "anchorWindow");
            el.offsetX      = jnum(e, "offsetX", 0.0);
            el.offsetY      = jnum(e, "offsetY", 0.0);
            if (el.id.empty()) el.id = (el.monitor.empty() ? std::string("anchor") : el.monitor) + ":" + std::to_string(parsed.size());
            if (!el.monitor.empty() || !el.anchorWindow.empty()) parsed[el.id] = el;
        }
    }
    const bool enabled = jbool(doc, "enabled", true);

    {
        std::lock_guard g(g_stateMutex);
        g_elements = std::move(parsed);
        g_enabled  = enabled;
        g_lastApplyStatus = "accepted";
        g_lastError.clear();
        ++g_generation;
    }
    damageAllMonitors();
    return "ok\n";
}

std::string clearElements() {
    { std::lock_guard g(g_stateMutex); g_elements.clear(); g_lastApplyStatus = "cleared"; ++g_generation; }
    damageAllMonitors();
    return "ok\n";
}

std::string setEnabled(bool on) {
    { std::lock_guard g(g_stateMutex); g_enabled = on; g_lastRenderStatus = on ? "pending" : "disabled"; }
    damageAllMonitors();
    return std::string("fluidglass: ") + (on ? "on" : "off") + "\n";
}

std::string statusString(eHyprCtlOutputFormat format) {
    std::lock_guard g(g_stateMutex);
    if (format == FORMAT_JSON) {
        json j = {
            {"pluginLoaded", true},
            {"available", true},
            {"enabled", g_enabled},
            {"elements", g_elements.size()},
            {"descriptorCount", static_cast<int>(g_elements.size())},
            {"renderHookInstalled", static_cast<bool>(g_renderStageListener)},
            {"shaderCompiled", g_shaderCompiled},
            {"shaderError", g_shaderError},
            {"lastApplyStatus", g_lastApplyStatus},
            {"lastError", g_lastError},
            {"lastRenderStatus", g_lastRenderStatus},
            {"generation", g_generation},
        };
        return j.dump();
    }
    return std::string("fluidglass enabled=") + (g_enabled ? "yes" : "no") +
           " elements=" + std::to_string(g_elements.size()) +
           " render=" + g_lastRenderStatus + "\n";
}

std::string onStatus(eHyprCtlOutputFormat format, std::string) { return statusString(format); }
std::string onApply(eHyprCtlOutputFormat, std::string req) { return applyPayload(removePrefix(std::move(req), "fluidglass-apply-json")); }
std::string onClear(eHyprCtlOutputFormat, std::string) { return clearElements(); }
std::string onMaterial(eHyprCtlOutputFormat format, std::string req) {
    const std::string m = lower(removePrefix(std::move(req), "fluidglass-material"));
    if (m.empty() || m == "status") return statusString(format);
    if (m == "off" || m == "disable" || m == "disabled" || m == "false" || m == "0") return setEnabled(false);
    if (m == "fluid-glass" || m == "on" || m == "enable" || m == "enabled" || m == "true" || m == "1") return setEnabled(true);
    return "error: expected on/off/fluid-glass or status\n";
}

} // namespace

// ── Plugin lifecycle ──────────────────────────────────────────────────────
APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;

    SHyprCtlCommand status; status.name = "fluidglass-status";     status.exact = true;  status.fn = onStatus;
    g_statusCommand = HyprlandAPI::registerHyprCtlCommand(g_handle, status);
    SHyprCtlCommand apply;  apply.name  = "fluidglass-apply-json"; apply.exact  = false; apply.fn  = onApply;
    g_applyCommand  = HyprlandAPI::registerHyprCtlCommand(g_handle, apply);
    SHyprCtlCommand clear;  clear.name  = "fluidglass-clear";      clear.exact  = true;  clear.fn  = onClear;
    g_clearCommand  = HyprlandAPI::registerHyprCtlCommand(g_handle, clear);
    SHyprCtlCommand mat;    mat.name    = "fluidglass-material";   mat.exact    = false; mat.fn    = onMaterial;
    g_materialCommand = HyprlandAPI::registerHyprCtlCommand(g_handle, mat);

    if (Event::bus())
        g_renderStageListener = Event::bus()->m_events.render.stage.listen(renderFluidGlass);

    return {"fluidglass", "Live fluid-glass compositor material for Hyprland", "CoastLineSec", FLUIDGLASS_PLUGIN_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_renderStageListener.reset();
    { std::lock_guard g(g_stateMutex); g_enabled = false; g_elements.clear(); g_lastRenderStatus = "disabled"; }
    g_captureFBs.clear();
    CCapturePass::g_capture.reset();
    g_shader.reset();
    g_shaderCompiled = g_shaderAttempted = false;
    if (g_materialCommand) HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_materialCommand);
    if (g_clearCommand)    HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_clearCommand);
    if (g_applyCommand)    HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_applyCommand);
    if (g_statusCommand)   HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_statusCommand);
    g_materialCommand.reset(); g_clearCommand.reset(); g_applyCommand.reset(); g_statusCommand.reset();
    g_handle = nullptr;
}
