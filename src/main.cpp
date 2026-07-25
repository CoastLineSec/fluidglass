// ─────────────────────────────────────────────────────────────────────────
//  HyprFluidGlass — live fluid-glass compositor material for Hyprland
//
//  A client (e.g. a shell/bar) sends element geometry over hyprctl; this plugin
//  captures the real framebuffer behind each element at RENDER_POST_WINDOWS and
//  runs the fluid-glass shader over it — refraction, frost, bevel, specular rim
//  and an optional cursor-tracked point light. No coordinate guessing: the
//  client provides the exact rect (or binds it to a surface) to render.
//
//  Surface binding: an element may carry bind{type,selector,relX,relY}. Bound
//  elements resolve a LIVE compositor surface every frame — a layer-shell
//  surface by namespace ("layer") or a window by class:/title: regex ("window")
//  — and take their position, visibility and fade alpha from it. Glass then
//  follows the surface wherever it goes, fades exactly when the compositor
//  fades it (e.g. top layers during fullscreen), and vanishes the moment the
//  surface dies (client crash/reload). Unbound elements are raw geometry.
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
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>

#include <nlohmann/json.hpp>

#include "v2/core/OpaqueId.hpp"
#include "v2/runtime/Runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <deque>
#include <exception>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef HYPRFLUIDGLASS_PLUGIN_VERSION
#define HYPRFLUIDGLASS_PLUGIN_VERSION "0.1.0"
#endif

namespace {

using json = nlohmann::json;

// ── Globals ───────────────────────────────────────────────────────────────
HANDLE              g_handle = nullptr;
SP<SHyprCtlCommand> g_statusCommand;
SP<SHyprCtlCommand> g_applyCommand;
SP<SHyprCtlCommand> g_clearCommand;
SP<SHyprCtlCommand> g_materialCommand;
SP<SHyprCtlCommand> g_debugCommand;
SP<SHyprCtlCommand> g_debugVerboseCommand;
SP<SHyprCtlCommand> g_v2Command;
CHyprSignalListener g_renderStageListener;
std::unique_ptr<hfg::v2::RuntimeService> g_v2Runtime;

// ── P4 readiness contract (hgsglass event + per-descriptor readiness) ─────────
// The shell proves, per descriptor and per REVISION, that HyprFluidGlass accepted
// and actually drew the current geometry before it may make QML transparent (P5).
// See .internal/bar-rewrite/01-architecture.md "P4" for the durable schema.
SP<CEventLoopTimer> g_glassTimer;        // coalesced change events + ~1s heartbeat
std::string         g_glassLastSig = ""; // readiness signature; event only on change
bool                g_glassDirty   = false; // a readiness-relevant transition since last emit
uint64_t            g_glassHbCounter = 0; // monotonic heartbeat counter (liveness proof)
double              g_glassLastHbMs  = 0; // steady ms of the last heartbeat emit
std::set<std::string> g_glassLastMons;      // monitors emitted last change (for clearing snapshots)
// Plugin GENERATION = a unique-per-load nonce (init time). The shell keys reload
// detection on this changing: a reloaded plugin starts empty, so a changed gen
// forces descriptor republish. Distinct from the internal change counter below.
uint64_t            g_pluginGen = 0;
// Draw epoch — bumped whenever the GL program/shader is (re)created. A descriptor
// is draw-confirmed only when it drew at BOTH the current rev and this epoch, so
// a shader/capture recreation voids stale confirmations without touching g_elements
// off-lock. Compiles once per session in practice (recompile ⇒ post-reload reset).
uint64_t            g_drawEpoch = 0;
// Coordinate agreement tolerance (LOGICAL px): position within this of the bound
// surface = aligned; within 4× = pending/near; beyond = divergent. Sizes compare
// with max(2px, 1%) to absorb legitimate scaling/rounding. Documented in the ledger.
static constexpr double COORD_ALIGN_PX = 2.0;

std::mutex   g_stateMutex;
bool         g_enabled = false;          // global on/off
std::string  g_lastApplyStatus = "none";
std::string  g_lastError;
std::string  g_lastRenderStatus = "disabled";
uint64_t     g_generation = 0;           // internal change counter (damage/coalesce)
double       g_animMs = 240.0;        // enter/exit animation duration (ms); shell may override per apply
int          g_debugField = 0;        // hyprfluidglass-material debug:<n> — 1=depth bands, 2=normals; 0=off

SP<CShader> g_shader;
bool        g_shaderAttempted = false;
bool        g_shaderCompiled  = false;
int         g_shaderRetries   = 0;      // allow a few retries — GL may not be ready at first apply
std::string g_shaderError;

// Custom-uniform locations, resolved ONCE at link (they're fixed for the program's life). The old
// path called glGetUniformLocation for ~20 uniforms per element per frame — a driver string lookup
// each time. Cached here after compile; drawElement sets them directly.
struct GlassUniforms {
    GLint srcTL = -1, srcTR = -1, srcBR = -1, srcBL = -1, destSize = -1, radiusPx = -1,
          refractPx = -1, edgeBandPx = -1, bevelPx = -1, highlight = -1, shadow = -1, lightDir = -1,
          specular = -1, rimWidthPx = -1,
          tint = -1, alpha = -1, chroma = -1, edgeDepth = -1, lens = -1, lensBandPx = -1, gloss = -1,
          blurTex = -1, blurOff = -1, blurScale = -1, useBlur = -1,
          cutRect = -1, cutRadius = -1, hasCutout = -1, part0 = -1, part1 = -1, part2 = -1, part3 = -1,
          partC0 = -1, partC1 = -1, partC2 = -1, partC3 = -1,
          partK0 = -1, partK1 = -1, partK2 = -1, partK3 = -1,
          plug0 = -1, plug1 = -1, plug2 = -1, plug3 = -1,
          partE0 = -1, partE1 = -1, partE2 = -1, partE3 = -1, ringFx = -1, curvePx = -1, veilSat = -1, debugField = -1,
          partAlpha = -1;
};
GlassUniforms g_uni;

// Separable-blur pass program (H/V selected by uDirTexel) + its clip-space quad.
SP<CShader> g_blurShader;
bool        g_blurCompiled = false;
struct BlurUniforms {
    GLint tex = -1, uvOff = -1, uvScale = -1, dirTexel = -1, radius = -1;
};
BlurUniforms g_blurUni;
GLuint g_quadVao = 0, g_quadVbo = 0;

// Per-element half-res framebuffer pair for the two blur passes. Reused across
// frames (alloc is a no-op at unchanged size); dropped when the element goes.
struct ElemBlurFBs {
    SP<Render::IFramebuffer> a, b;
};
std::map<std::string, ElemBlurFBs> g_elemFBs;

// Damage we caused ourselves recently, per monitor (scaled monitor-local px).
// Every glass redraw damage-rings its box so older swapchain buffers repaint it;
// those ring echoes come back as frame damage for a few frames and must not
// re-trigger the redraw gate, or the loop would never idle. The gate subtracts
// this region; the TTL covers the swapchain depth.
struct SelfDamage {
    CRegion region;
    int     ttl = 0;
};
std::map<std::string, SelfDamage> g_selfDamage;

// ── Diagnostics ───────────────────────────────────────────────────────────
// Bounded lifecycle event ring, dumped by the hyprfluidglass-debug-json command
// for tools/hyprfluidglass-debug (bug reports, live watching). Records TRANSITIONS
// (apply/bind/draw/exit/purge/shader), not per-frame noise. Own mutex so any
// context may log, including code already holding g_stateMutex.
struct DbgEvent {
    uint64_t    seq;
    double      tMs;
    std::string id, event, info;
};
std::mutex           g_dbgMutex;
std::deque<DbgEvent> g_dbgEvents;
uint64_t             g_dbgSeq = 0;

void dbgLog(const std::string& id, const std::string& event, const std::string& info = "") {
    static const auto t0 = std::chrono::steady_clock::now();
    const double      t  = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::lock_guard   g(g_dbgMutex);
    g_dbgEvents.push_back({++g_dbgSeq, t, id, event, info});
    if (g_dbgEvents.size() > 1024)
        g_dbgEvents.pop_front();
}

void recordBoundaryFailure(std::string_view boundary, const char* detail) noexcept {
    try {
        std::lock_guard guard(g_stateMutex);
        g_lastError        = std::string(boundary) + " callback failed";
        g_lastRenderStatus = "error";
    } catch (...) {
    }
    try {
        dbgLog("", "callback.error", std::string(boundary) + ": " + (detail ? detail : "unknown exception"));
    } catch (...) {
    }
}

template <typename F>
std::string runCommandBoundary(std::string_view boundary, F&& command) {
    try {
        return std::forward<F>(command)();
    } catch (const std::exception& error) {
        recordBoundaryFailure(boundary, error.what());
    } catch (...) {
        recordBoundaryFailure(boundary, "non-standard exception");
    }
    return "error: internal plugin failure\n";
}

// One glass surface requested by the client.
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
    bool        lightMode = false;   // explicit theme mode from the shell — table selection
                                     // must NOT infer from tint luma (bright accents misread)
    float       tintR = 1.0F, tintG = 1.0F, tintB = 1.0F;
    // Locked material (design-px at the 200px ref); overridable per element for live tuning.
    double      refraction = 45.0, rimBand = 30.0, bevel = 30.0, rimWidth = 3.0;
    double      highlight = 0.10, shadow = 0.10, lightDeg = 90.0, specular = 0.21;
    double      chroma = 0.15, edgeDepth = 0.14;  // chromatic aberration (subtle, like Apple) + inner glass-wall shadow
    double      lens = 0.12;                       // convex magnification strength — thick-slab lensing, the dominant Apple cue
    double      lensBand = 40.0;                   // lens/refraction reach = glass THICKNESS (design px, clamped) — see resolveParams
    double      gloss = 0.14;                      // broad surface sheen across the FACE — clean shiny-glass reflection (0 = off)
    // Surface binding — the element renders only while a live compositor surface backs it:
    // position, visibility and fade alpha are taken from the bound surface each frame.
    //   "layer"  — a layer-shell surface matched by namespace on the element's monitor
    //   "window" — a window matched by class:/title: regex (legacy anchorWindow payloads map here;
    //              the Labs preview uses this — its floating window can't self-locate)
    // Empty type = raw geometry (monitor + x/y as sent, always treated as visible).
    std::string bindType;
    PHLWINDOWREF boundWindow; // resolved by resolveWindowBind each frame
    // Render beneath the window stack (RENDER_POST_WALLPAPER) instead of over
    // it — for window-anchored glass that must sit UNDER its own window's
    // transparent regions (e.g. the Settings ribbon), keeping the window's
    // text crisp above the glass exactly like top-layer bars.
    bool        under = false;
    std::string bindSelector;
    double      relX = 0.0, relY = 0.0;   // glass-rect offset inside the bound surface (logical px)
    bool        everBound = false;        // first successful resolve starts the enter animation
    bool        bound     = false;        // last resolve result (surfaced in hyprfluidglass-status)
    std::chrono::steady_clock::time_point lastBindOk{};  // unresolvable for >60s → element purged
    // Damage bookkeeping: where the glass last drew (global logical), so losing the
    // bound surface can clear the leftover pixels with one targeted damage.
    bool        wasDrawn = false;
    double      lastGX = 0, lastGY = 0, lastGW = 0, lastGH = 0;
    // ── P4 per-descriptor readiness ───────────────────────────────────────────
    // rev: shell-owned monotonic descriptor revision, echoed in status + events.
    //   Readiness for rev N never authorizes rev N+1. Carried across re-applies;
    //   a geometry/shape/bind/material change publishes a NEW rev from the shell.
    // drawnRev: the rev value at the most recent SUCCESSFUL draw (0 = never drawn
    //   at any rev). Draw-confirmed-at-current = (drawnRev == rev && rev != 0).
    //   Reset to 0 on bind loss, shader/capture recreate, and (implicitly) on rev
    //   change (drawnRev keeps the old value → not confirmed until it redraws).
    uint64_t    rev = 0;
    uint64_t    drawnRev = 0;
    uint64_t    drawnEpoch = 0;   // g_drawEpoch at the last successful draw
    // Observed bound-surface geometry (logical px) + position distance from the
    // descriptor's expected top-left. <0 = unknown (unbound / raw geometry).
    double      obsW = -1, obsH = -1, coordDist = -1;
    // Parametric ELEMENT transition (glass-first redesign — bar hide/reveal):
    // the whole element slides along its screen edge; the plugin derives the
    // offset each frame from the same bezier the shell's content Translate
    // runs, and erases the element itself when a close completes (law 4 —
    // no exit fade, no shell round-trip).
    int    elTrKind = 0;          // 0 none, 1 open, 2 close
    double elTrDurMs = 0, elTrTravel = 0;
    int    elTrSide = 0;          // 0 top, 1 bottom, 2 left, 3 right
    std::vector<double> elTrBezier;
    std::chrono::steady_clock::time_point elTrStart{};
    bool   elTrDone = false;
    // Composite shape — for surfaces that must read as ONE piece of glass.
    // An optional CUTOUT turns the element into a ring (frame: outer rect minus
    // rounded inner cutout); up to four attached PARTS carry the shell's chrome
    // slot data VERBATIM — per-corner radii and per-corner junction-fillet k —
    // so the glass silhouette is pixel-identical to the painted connected
    // chrome. PLUGS are hidden rects unioned sharply to deepen the throat where
    // a part touches the ring (touching boundaries stay shallow even under a
    // fillet union, and glass edge-bands would paint lines across them).
    // All rects are element-local logical px; corner order is TL,TR,BR,BL.
    struct PartRect {
        double x = 0, y = 0, w = 0, h = 0;
        double a = 1.0;   // payload visibility target (0 while the surface is closing)
        // Parametric transition (glass-first redesign): the plugin animates the
        // reveal itself from the resting geometry — same bezier the chrome runs.
        int    trKind = 0;          // 0 none, 1 open, 2 close
        double trDurMs = 0, trProtrusion = 0, trTravel = 0;
        int    trSide = 0;          // 0 top, 1 bottom, 2 left, 3 right
        std::vector<double> trBezier;
        std::chrono::steady_clock::time_point trStart{};
        bool   trDone = false;
        double c[4] = {0, 0, 0, 0};   // per-corner radii
        double k[4] = {0, 0, 0, 0};   // per-corner junction fillets
        // Material extent: the part's rect extended through the ring to the
        // screen edge on its bar side. Not part of the SDF — it defines the
        // REGION that runs the part's full material, so the throat column
        // (bar strip included) is one piece with the body instead of a
        // half-blended band at the old cutout line.
        double ex = 0, ey = 0, ew = -1, eh = 0;
    };
    struct PlugRect {
        double x = 0, y = 0, w = 0, h = 0;
    };
    bool                  hasCutout = false;
    double                cutX = 0, cutY = 0, cutW = 0, cutH = 0, cutR = 0;
    std::vector<PartRect> parts;
    std::vector<PlugRect> plugs;
    // Effect-field sweep at the junctions: the SILHOUETTE bends at the parts'
    // raw k (the user's radius), but the hole field driving lensing/refraction
    // sweeps as if the arc were this wide — tight arcs pinch the field and
    // smear bright backdrop content into streaks. 0 = follow the raw k.
    double connectorCurve = 0;
    // Diagnostics (surfaced via hyprfluidglass-debug-json).
    uint64_t    drawCount = 0;
    std::string lastDrawCause;
    // Geometry smoothing — payload geometry is a TARGET; rendering uses these
    // smoothed values (exponential settle, ~35ms time constant) so throttled
    // IPC updates read as continuous motion at any refresh rate. Bind-derived
    // POSITION is never smoothed (the compositor moves surfaces itself); the
    // rel offsets, raw-geometry x/y and w/h/radius are.
    double sx = 0, sy = 0, sw = -1, sh = -1, sr = 0, srx = 0, sry = 0;   // sw<0 = uninitialised
    // Smoothed PART rects (same settle): the shell streams the connected
    // reveal per frame; without this the parts stairstep between updates.
    std::vector<PartRect> sParts;
    // Per-part visibility fade: parts no longer POP on count change — new
    // parts ramp 0→1, removed parts linger as "dying" entries ramping 1→0
    // (the launcher exit-flash / bar-peek strobe fix). Index-matched to
    // sParts (slot order is stable in practice).
    std::vector<double>   sPartAlpha;
    std::vector<PartRect> dyingParts;
    std::vector<double>   dyingAlpha;
    double partAlphaArr[4] = {1.0, 1.0, 1.0, 1.0};   // render-copy, per slot
    std::chrono::steady_clock::time_point smoothT{};
    // Enter/exit animation — universal, driven purely by elements appearing/disappearing.
    std::chrono::steady_clock::time_point birth{};      // enter-animation start
    std::chrono::steady_clock::time_point exitStart{};  // exit-animation start
    bool        exiting = false;
    double      animScale = 1.0;    // transient, per-frame: 0.9..1 grow
    double      renderAlpha = 1.0;  // transient, per-frame: 0..1 fade
};
std::map<std::string, GlassElement> g_elements;

// Windows carrying an under-glass element get the compositor's OWN blur
// disabled for as long as the glass is bound: Hyprland auto-blurs every
// translucent window when decoration:blur is on, and that blur composites
// AFTER (over) our pre-window glass — from a source that doesn't contain it
// (the precomputed background blur FB when nothing sits between window and
// wallpaper) — replacing the material entirely on tiled windows and
// re-processing it into mush on floating ones. One glass per surface: the
// engine claims the window via the rule applicator (PRIORITY_SET_PROP, the
// highest, reversible) and releases it when the glass leaves. Keyed by
// element id; a window stays claimed while ANY under element binds it.
std::map<std::string, PHLWINDOWREF> g_noBlurApplied;

bool anyOtherNoBlurRef(const std::string& exceptId, const PHLWINDOW& win) {
    for (const auto& [oid, ref] : g_noBlurApplied) {
        if (oid == exceptId)
            continue;
        if (ref.lock() == win)
            return true;
    }
    return false;
}

void setUnderNoBlur(const std::string& id, const PHLWINDOW& win) {
    if (!win)
        return;
    if (auto it = g_noBlurApplied.find(id); it != g_noBlurApplied.end()) {
        const auto prev = it->second.lock();
        if (prev == win)
            return;                       // already claimed by this element
        // The bind resolved to a DIFFERENT window (selector re-match) — release
        // the old one first (unless another element still holds it).
        if (prev && !anyOtherNoBlurRef(id, prev)) {
            prev->m_ruleApplicator->noBlur().unset(Desktop::Types::PRIORITY_SET_PROP);
            if (g_pHyprRenderer)
                g_pHyprRenderer->damageWindow(prev);
            dbgLog(id, "noblur.unset", "rebind");
        }
    }
    if (!anyOtherNoBlurRef(id, win)) {
        win->m_ruleApplicator->noBlur().set(true, Desktop::Types::PRIORITY_SET_PROP);
        if (g_pHyprRenderer)
            g_pHyprRenderer->damageWindow(win);
        dbgLog(id, "noblur.set", win->m_title);
    }
    g_noBlurApplied[id] = win;
}

void clearUnderNoBlur(const std::string& id) {
    auto it = g_noBlurApplied.find(id);
    if (it == g_noBlurApplied.end())
        return;
    const auto win = it->second.lock();
    g_noBlurApplied.erase(it);
    if (!win || anyOtherNoBlurRef(id, win))
        return;                           // window gone, or still claimed by another element
    win->m_ruleApplicator->noBlur().unset(Desktop::Types::PRIORITY_SET_PROP);
    if (g_pHyprRenderer)
        g_pHyprRenderer->damageWindow(win);
    dbgLog(id, "noblur.unset", win->m_title);
}

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
// Evaluate a QML BezierSpline easing curve (flat [c1x,c1y,c2x,c2y,ex,ey]*N,
// implicit start (0,0), final end expected (1,1)) at x in [0,1] -> y.
double evalBezierSpline(const std::vector<double>& c, double x) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    if (c.size() < 6 || c.size() % 6 != 0) return x;   // fallback linear
    double sx = 0.0, sy = 0.0;
    for (size_t seg = 0; seg < c.size(); seg += 6) {
        const double c1x = c[seg], c1y = c[seg + 1], c2x = c[seg + 2], c2y = c[seg + 3], ex = c[seg + 4], ey = c[seg + 5];
        if (x <= ex + 1e-9 || seg + 6 >= c.size()) {
            // cubic from (sx,sy) with controls; solve t for x by bisection
            auto bez = [&](double t, double p0, double p1, double p2, double p3) {
                const double u = 1.0 - t;
                return u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t * p3;
            };
            double lo = 0.0, hi = 1.0;
            for (int i = 0; i < 24; i++) {
                const double mid = (lo + hi) * 0.5;
                if (bez(mid, sx, c1x, c2x, ex) < x) lo = mid; else hi = mid;
            }
            const double t = (lo + hi) * 0.5;
            return bez(t, sy, c1y, c2y, ey);
        }
        sx = ex; sy = ey;
    }
    return x;
}

double jnum(const json& o, std::string_view k, double def = 0.0) {
    if (auto it = o.find(k); it != o.end() && it->is_number()) {
        const double value = it->get<double>();
        if (std::isfinite(value))
            return value;
    }
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

std::chrono::milliseconds boundedElapsedMilliseconds(double value) {
    constexpr double MAX_ELAPSED_MS = 24.0 * 60.0 * 60.0 * 1000.0;
    if (!std::isfinite(value))
        return std::chrono::milliseconds{0};
    const double bounded = std::clamp(value, 0.0, MAX_ELAPSED_MS);
    return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(std::llround(bounded))};
}
void parseHex(const std::string& hex, float& r, float& g, float& b) {
    if (hex.size() < 4 || hex[0] != '#') return;
    auto byte = [&](int i, int n) { return static_cast<float>(std::stoi(hex.substr(i, n), nullptr, 16)) / (n == 2 ? 255.0F : 15.0F); };
    try {
        if (hex.size() >= 7) { r = byte(1, 2); g = byte(3, 2); b = byte(5, 2); }   // #RRGGBB
        else                 { r = byte(1, 1); g = byte(2, 1); b = byte(3, 1); }   // #RGB
    } catch (...) {}
}

// ── Material constants + size scaling ─────────────────────────────────────
// FROST (blur) + strengths/tint/angle are size-INDEPENDENT: frost is a
// readability function, not an aesthetic, so it must not shrink on small
// surfaces (dock/bar) — it renders the same regardless of element size.
namespace mat {
    // Frost range: the clear end stays transparent enough that the backdrop reads through;
    // the high end is a genuinely heavy frosted-pane blur. Presets sample along this range.
    // VEIL model (P3) — calibrated to the iOS reference pixels (rainbow A/B):
    // frosted glass = BLUR + strong DESATURATION + a translucent veil. The
    // level drives veil opacity + blur + how far saturation is crushed; the
    // backdrop stays visible even at max (measured dark-max alpha ~0.7).
    // THREE calibrated stops (min / med / max); the slider interpolates
    // piecewise between them. iOS curve is non-linear: sat + brightness hold
    // through min & med, then swing at max. MAX is locked (James-approved);
    // MIN/MED seeded from the iOS targets, tuned by comparison.
    constexpr double BLUR_MIN = 2.0,   BLUR_MED = 18.0,  BLUR_MAX = 88.0;   // px — min near-clear (text legible), med eased
    constexpr double VEIL_MIN = 0.43,  VEIL_MED = 0.43,  VEIL_MAX = 0.72;   // 1-transmittance (measured: min/med 0.57, max 0.28)
    constexpr double SAT_MIN  = 0.95,  SAT_MED  = 0.95,  SAT_MAX  = 0.95;   // chroma KEEP dark (measured ~95%)
    constexpr double LVEIL_MIN= 0.54,  LVEIL_MED= 0.53,  LVEIL_MAX= 0.71;   // light alpha (measured)
    constexpr double LSAT_MIN = 0.61,  LSAT_MED = 0.49,  LSAT_MAX = 0.37;   // light chroma KEEP (measured: light desats hard)
}

struct ResolvedParams {
    double blurPx, refractPx, rimBandPx, bevelPx, rimWidthPx, radiusPx;
    double highlight, shadow, specular, tintStrength;
    double chroma, edgeDepth, lens, lensBandPx, gloss, veilSat;
    double lightX, lightY;
    float  tintR, tintG, tintB;
};

ResolvedParams resolveParams(const GlassElement& el, double scale) {
    const double t       = clampd(el.glassLevel, 0.0, 1.0);
    const double bt      = (el.blurLevel >= 0.0) ? clampd(el.blurLevel, 0.0, 1.0) : t;  // custom or preset frost
    const double tt      = (el.tintLevel >= 0.0) ? clampd(el.tintLevel, 0.0, 1.0) : t;  // custom or preset tint
    // Most parameters apply directly: design px × monitor scale. The edge bands (rim/bevel/lens)
    // are the exception — they are clamped against the surface's short axis below.
    auto direct = [&](double designPx) { return designPx * scale; };

    // Piecewise interp across the three calibrated stops (0=min, .5=med, 1=max).
    auto stop3 = [](double x, double lo, double mid, double hi) {
        return x < 0.5 ? lo + (mid - lo) * (x / 0.5) : mid + (hi - mid) * ((x - 0.5) / 0.5);
    };
    ResolvedParams r;
    r.blurPx     = direct(stop3(bt, mat::BLUR_MIN, mat::BLUR_MED, mat::BLUR_MAX));
    // Edge bands model the glass THICKNESS: a fixed design-px depth (× DPI), so a bar edge and a
    // panel edge refract over the same physical depth and read as the same material. A percentage
    // of the surface would make identical glass look different at every size — an enormous band on
    // a near-fullscreen panel, a thin bar swallowed whole with no clean centre. The clamp against
    // the short half-axis guarantees even very thin surfaces keep an unbent middle.
    const double halfShort = std::min(el.w, el.h) * scale * 0.5;     // physical half short-axis
    auto bandPx = [&](double designPx, double maxFrac) { return std::min(designPx * scale, halfShort * maxFrac); };
    // Refraction displacement: fixed design-px on normal/large surfaces, but capped
    // to the short axis on SMALL ones — a 40px pill must not displace the full 45px
    // (more than itself). Same principle as the bands above.
    r.refractPx  = std::min(direct(el.refraction), halfShort * 0.8);
    r.rimBandPx   = bandPx(el.rimBand, 0.6);
    r.bevelPx     = bandPx(el.bevel,   0.6);
    r.lensBandPx  = bandPx(el.lensBand, 0.55);
    r.rimWidthPx = std::max(1.0, direct(el.rimWidth));
    r.radiusPx   = direct(el.radius);
    r.highlight  = el.highlight;
    r.shadow     = el.shadow;
    r.specular   = el.specular;
    r.chroma     = el.chroma;       // strengths — size-independent, like the other aesthetics
    r.edgeDepth    = el.edgeDepth;
    r.lens         = el.lens;
    r.gloss        = el.gloss * (1.0 - 0.90 * t);  // glossy at min -> matte at max (iOS texture cue)
    const double a = el.lightDeg * M_PI / 180.0;
    r.lightX = std::cos(a);
    r.lightY = std::sin(a);
    // Veil is always present (that is what makes it read as frosted glass, not
    // just blur); tintEnabled/Stained is handled shell-side as the colour choice.
    // Mode-aware material: the shell says which theme mode is active EXPLICITLY —
    // inferring from the veil colour's luma misread bright stained accents as
    // light mode and hard-desaturated them to grey.
    const bool isLight = el.lightMode;
    r.tintStrength = isLight ? stop3(tt, mat::LVEIL_MIN, mat::LVEIL_MED, mat::LVEIL_MAX)
                             : stop3(tt, mat::VEIL_MIN,  mat::VEIL_MED,  mat::VEIL_MAX);
    r.veilSat      = isLight ? stop3(t, mat::LSAT_MIN, mat::LSAT_MED, mat::LSAT_MAX)
                             : stop3(t, mat::SAT_MIN,  mat::SAT_MED,  mat::SAT_MAX);
    r.tintR = el.tintR; r.tintG = el.tintG; r.tintB = el.tintB;
    return r;
}

// ── Transform map (PRESERVED — handles rotated/flipped displays) ───────────
// The captured backdrop lives in the monitor's PHYSICAL pixel basis (raw GPU
// framebuffer, no transform applied). Element corners arrive in transformed
// DISPLAY space. On a rotated/flipped monitor those differ, so each corner is
// mapped into the physical texture basis via the INVERSE monitor transform.
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
uniform float uRefractPx;   // edge lensing
uniform float uEdgeBandPx;  // rim band
uniform float uBevelPx;     // bevel band
uniform float uHighlight;
uniform float uShadow;
uniform vec2  uLightDir;    // direction toward light (directional mode)
uniform float uSpecular;
uniform float uRimWidthPx;
uniform vec4  uTint;        // rgb + strength
uniform float uVeilSat;     // backdrop saturation kept under the veil (frost)
uniform float uAlpha;
uniform sampler2D uBlurTexS; // pre-blurred backdrop (separable two-pass, half-res)
uniform vec2  uBlurOff;      // capture-UV origin of the blurred subrect
uniform vec2  uBlurScale;    // capture-UV size of the blurred subrect
uniform float uUseBlur;      // 0 = sample the sharp capture (blur radius ~0)
uniform float uHasCutout;    // composite shape: ring = outer minus rounded cutout
uniform vec4  uCutRect;      // cutout x,y,w,h (element px)
uniform float uCutRadius;
uniform vec4  uPart0;        // chrome-slot body rects (element px); w<=0 = unused
uniform vec4  uPart1;
uniform vec4  uPart2;
uniform vec4  uPart3;
uniform vec4  uPartC0;       // per-corner radii (TL,TR,BR,BL) — chrome slot translation
uniform vec4  uPartC1;
uniform vec4  uPartC2;
uniform vec4  uPartC3;
uniform vec4  uPartK0;       // per-corner junction-fillet k (a corner fillets into the ring where k > 0)
uniform vec4  uPartK1;
uniform vec4  uPartK2;
uniform vec4  uPartK3;
uniform vec4  uPlug0;        // hidden throat fillers, sharp union; w<=0 = unused
uniform vec4  uPlug1;
uniform vec4  uPlug2;
uniform vec4  uPlug3;
uniform vec4  uPartAlpha;    // per-part visibility fade (1 = solid); scoped by extent/rect
uniform vec4  uPartExt0;     // material extents: part rect extended to the screen edge —
uniform vec4  uPartExt1;     // the region that runs the part's FULL material (throat column
uniform vec4  uPartExt2;     // included); a weight field only, not part of the SDF
uniform vec4  uPartExt3;
uniform float uCurvePx;      // effect-field junction sweep (physical px); 0 = follow raw k
uniform float uRingFx;       // effect scale on the RING region (thin strips can't carry full bands);
                             // parts run at 1.0, blended through the region weight
uniform float uChroma;      // chromatic aberration strength (edge RGB split)
uniform float uEdgeDepth;   // inner glass-wall shadow strength (depth, light-independent)
uniform float uLens;        // convex magnification strength (thick-slab lensing)
uniform float uLensBandPx;  // how far in the lens reaches = glass thickness (px, clamped CPU-side)
uniform float uGloss;       // broad surface sheen across the face (clean shiny-glass reflection)
uniform float uDebugField;  // 1 = depth contour bands, 2 = normal as color; 0 = off

float sdRoundRect(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - (b - vec2(r));
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// Crease-free outward normal of the rounded rect. A central-difference gradient of this SDF
// flips direction across the box's 45° medial-axis diagonals (where max(q.x,q.y) switches),
// putting a hard line through every normal-driven effect near the corners. The analytic form
// avoids that: radial from the corner centre on the arc/edge, and a smooth diagonal blend of
// the two axis normals in the interior wedge — continuous everywhere.
vec2 rrNormal(vec2 p, vec2 b, float r) {
    vec2 s  = sign(p + vec2(1e-6));
    vec2 q  = abs(p) - (b - vec2(r));
    vec2 qc = max(q, vec2(0.0));
    if (qc.x + qc.y > 1e-5)
        return normalize(s * qc + vec2(1e-6));            // straight edge + corner arc
    // interior wedge: blend axis normals across the diagonal instead of hard-switching
    float t = 0.5 + 0.5 * (q.x - q.y) / (abs(q.x) + abs(q.y) + 1e-4);
    t = smoothstep(0.12, 0.88, t);
    return normalize(mix(vec2(0.0, s.y), vec2(s.x, 0.0), t) + vec2(1e-6));
}

// A rounded rect given as (x, y, w, h) in element px — used by the cutout.
float sdRectAt(vec2 px, vec4 rect, float rad) {
    vec2 hb = rect.zw * 0.5;
    rad = min(rad, min(hb.x, hb.y));
    vec2 q = abs(px - (rect.xy + hb)) - (hb - vec2(rad));
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rad;
}

// ── Chrome-slot math, ported verbatim from the shell's connected_arc.frag so
// the glass silhouette is pixel-identical to the painted connected chrome. ──
// Per-corner rounded box; radius selected by pixel quadrant (TL,TR,BR,BL).
float sdRoundBox4(vec2 p, vec2 c, vec2 hs, vec4 r) {
    p -= c;
    float rr = (p.x >= 0.0) ? (p.y >= 0.0 ? r.z : r.y) : (p.y >= 0.0 ? r.w : r.x);
    rr = min(rr, min(hs.x, hs.y));
    vec2 q = abs(p) - hs + rr;
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - rr;
}

// Circular-arc smooth-min: k = 0 is a plain union; k > 0 forms a true arc
// fillet — the connector throat geometry of the connected chrome.
float sminArc(float a, float b, float k) {
    if (k <= 0.0)
        return min(a, b);
    return max(k, min(a, b)) - length(max(vec2(k) - vec2(a, b), vec2(0.0)));
}

float partDist(vec2 px, vec4 rect, vec4 corner) {
    vec2 c = rect.xy + rect.zw * 0.5;
    return sdRoundBox4(px, c, rect.zw * 0.5, corner);
}

// Junction-fillet strength selected by pixel quadrant relative to the part.
float partK(vec2 px, vec4 rect, vec4 ks) {
    vec2 p = px - (rect.xy + rect.zw * 0.5);
    return (p.x >= 0.0) ? (p.y >= 0.0 ? ks.z : ks.y) : (p.y >= 0.0 ? ks.w : ks.x);
}

// Effect-field variant: junction fillets swept for the DIRECTION field only
// (sdHole → holeNormal). The silhouette and the depth field keep the raw k.
// Floored at 64px regardless of the connector curve: the direction field's
// gradient crease must stay OUTSIDE the effect bands (~40px + margin) at any
// design radius, or displacement direction flips inside the bands and bright
// backdrop smears into streaks — the original "mangled" throat.
float partKE(vec2 px, vec4 rect, vec4 ks) {
    float k = partK(px, rect, ks);
    return k > 0.0 ? max(k, max(uCurvePx, 64.0)) : 0.0;
}

float plugDist(vec2 px, vec4 rect) {
    vec2 c = rect.xy + rect.zw * 0.5;
    vec2 q = abs(px - c) - rect.zw * 0.5;
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0)));
}

// The assembly's HOLE: the cutout minus the attached parts/plugs, with the
// same fillet arcs. A frame assembly has exactly ONE visible glass edge — its
// INNER OUTLINE (cutout line → connector arcs → around the body and back);
// the screen-perimeter boundary sits at the monitor bezel and carries no
// lensing. This field is positive inside the glass, growing away from the
// inner outline, and drives all edge effects for cutout composites.
float sdHole(vec2 px) {
    float d = sdRectAt(px, uCutRect, uCutRadius);
    if (uPart0.z > 0.5) d = -sminArc(-d, partDist(px, uPart0, uPartC0), partKE(px, uPart0, uPartK0));
    if (uPart1.z > 0.5) d = -sminArc(-d, partDist(px, uPart1, uPartC1), partKE(px, uPart1, uPartK1));
    if (uPart2.z > 0.5) d = -sminArc(-d, partDist(px, uPart2, uPartC2), partKE(px, uPart2, uPartK2));
    if (uPart3.z > 0.5) d = -sminArc(-d, partDist(px, uPart3, uPartC3), partKE(px, uPart3, uPartK3));
    if (uPlug0.z > 0.5) d = max(d, -plugDist(px, uPlug0));
    if (uPlug1.z > 0.5) d = max(d, -plugDist(px, uPlug1));
    if (uPlug2.z > 0.5) d = max(d, -plugDist(px, uPlug2));
    if (uPlug3.z > 0.5) d = max(d, -plugDist(px, uPlug3));
    return d;
}

// The TRUE inner outline: the same hole field with the RAW junction fillets
// (partK, no sweep) — matches the visible silhouette bend exactly. Effects are
// POSITIONED by this field (depth), while the swept sdHole above only supplies
// their smooth DIRECTION (normal). One field cannot do both jobs: tight arcs
// put its gradient crease inside the effect bands (the old "mangled" streaks),
// and swept arcs detach the bands from the visible edge (effects dropped off
// before the real bend). Plugs stay in both fields — they are geometry
// correction (no seam lines across the throat), not smoothing.
float sdHoleRaw(vec2 px) {
    float d = sdRectAt(px, uCutRect, uCutRadius);
    if (uPart0.z > 0.5) d = -sminArc(-d, partDist(px, uPart0, uPartC0), partK(px, uPart0, uPartK0));
    if (uPart1.z > 0.5) d = -sminArc(-d, partDist(px, uPart1, uPartC1), partK(px, uPart1, uPartK1));
    if (uPart2.z > 0.5) d = -sminArc(-d, partDist(px, uPart2, uPartC2), partK(px, uPart2, uPartK2));
    if (uPart3.z > 0.5) d = -sminArc(-d, partDist(px, uPart3, uPartC3), partK(px, uPart3, uPartK3));
    if (uPlug0.z > 0.5) d = max(d, -plugDist(px, uPlug0));
    if (uPlug1.z > 0.5) d = max(d, -plugDist(px, uPlug1));
    if (uPlug2.z > 0.5) d = max(d, -plugDist(px, uPlug2));
    if (uPlug3.z > 0.5) d = max(d, -plugDist(px, uPlug3));
    return d;
}

vec2 holeNormal(vec2 px) {
    const float e = 2.5;
    float dx = sdHole(px + vec2(e, 0.0)) - sdHole(px - vec2(e, 0.0));
    float dy = sdHole(px + vec2(0.0, e)) - sdHole(px - vec2(0.0, e));
    return normalize(vec2(dx, dy) + 1e-6);
}

// Composite scene SDF: ring (outer minus rounded cutout) → each chrome-slot
// part unioned with its per-corner fillet → hidden plugs unioned sharply.
float sdScene(vec2 px) {
    vec2  half_ = uDestSize * 0.5;
    float rr    = min(uRadiusPx, min(half_.x, half_.y));
    float d     = sdRoundRect(px - half_, half_, rr);
    if (uHasCutout > 0.5)
        d = max(d, -sdRectAt(px, uCutRect, uCutRadius));
    if (uPart0.z > 0.5) d = sminArc(d, partDist(px, uPart0, uPartC0), partK(px, uPart0, uPartK0));
    if (uPart1.z > 0.5) d = sminArc(d, partDist(px, uPart1, uPartC1), partK(px, uPart1, uPartK1));
    if (uPart2.z > 0.5) d = sminArc(d, partDist(px, uPart2, uPartC2), partK(px, uPart2, uPartK2));
    if (uPart3.z > 0.5) d = sminArc(d, partDist(px, uPart3, uPartC3), partK(px, uPart3, uPartK3));
    if (uPlug0.z > 0.5) d = min(d, plugDist(px, uPlug0));
    if (uPlug1.z > 0.5) d = min(d, plugDist(px, uPlug1));
    if (uPlug2.z > 0.5) d = min(d, plugDist(px, uPlug2));
    if (uPlug3.z > 0.5) d = min(d, plugDist(px, uPlug3));
    return d;
}

vec2 sceneNormal(vec2 px) {
    const float e = 2.5;   // wider stencil reads smoother around fillets and cutout arcs
    float dx = sdScene(px + vec2(e, 0.0)) - sdScene(px - vec2(e, 0.0));
    float dy = sdScene(px + vec2(0.0, e)) - sdScene(px - vec2(0.0, e));
    return normalize(vec2(dx, dy) + 1e-6);
}

// local 0..1 surface uv -> capture-texture uv (bilinear across the 4 corners,
// so the transform-mapped corners carry rotated/flipped displays correctly).
vec2 srcUv(vec2 local) {
    // NOT clamped to the element's own rect: near the edges the frost + refraction must be able to
    // reach the SURROUNDING backdrop (the capture is the whole screen). Clamping here made the edge
    // sample a smeared repeat of itself → un-frosted edges + content that "pulls back" instead of
    // flowing through to the boundary. Extrapolating the corner-UV bilinear samples the real
    // surroundings; CLAMP_TO_EDGE on the texture covers anything past the screen.
    vec2 t = mix(uSrcTL, uSrcTR, local.x);
    vec2 b = mix(uSrcBL, uSrcBR, local.x);
    return mix(t, b, local.y);
}

// Frosted backdrop lookup. The heavy lifting happened before this draw: the
// capture subrect behind the element was blurred CPU-side into uBlurTexS by a
// separable two-pass Gaussian at half resolution — dense, smooth, and paid once
// per element instead of per fragment. Here frost is a single texture tap (the
// in-shader 121-tap kernel this replaces shimmered at large radii: its sparse
// jittered sampling decorrelated frame-to-frame as content moved).
vec3 backdrop(vec2 local) {
    vec2 cuv = srcUv(local);
    if (uUseBlur < 0.5)
        return texture(tex, cuv).rgb;
    return texture(uBlurTexS, (cuv - uBlurOff) / uBlurScale).rgb;
}

void main() {
    vec2  localUv = clamp(v_texcoord, 0.0, 1.0);
    vec2  localPx = localUv * uDestSize;
    vec2  half_   = uDestSize * 0.5;
    float r       = min(uRadiusPx, min(half_.x, half_.y));
    vec2  p       = localPx - half_;
    bool  composite = uHasCutout > 0.5 || uPart0.z > 0.5;
    float d;
    float fxScale = 1.0;
    if (composite) {
        d = sdScene(localPx);
        // PER-SIDE band fit: each ring strip fits the bands to ITS OWN local
        // thickness — a 40px bar strip carries ~24px bands (visible gradient)
        // while a 10px side strip wears a 6px hairline. The old global-min
        // scaling crushed the bars to the sides' scale; and a band WIDER than
        // its strip has no gradient at all, which reads as no effect (James's
        // "nothing on the top and bottom bars"). Pixels not in any strip
        // (popout bodies inside the cutout) keep full bands. uRingFx remains
        // as a neutral global hook (CPU sends 1).
        if (uHasCutout > 0.5) {
            float t = 0.0;
            if (localPx.y < uCutRect.y)                        t = max(t, uCutRect.y);
            if (localPx.y > uCutRect.y + uCutRect.w)           t = max(t, uDestSize.y - (uCutRect.y + uCutRect.w));
            if (localPx.x < uCutRect.x)                        t = max(t, uCutRect.x);
            if (localPx.x > uCutRect.x + uCutRect.z)           t = max(t, uDestSize.x - (uCutRect.x + uCutRect.z));
            if (t > 0.5) {
                float maxB = max(uEdgeBandPx, max(uBevelPx, uLensBandPx));
                if (maxB > 1.0)
                    fxScale = clamp((t * 0.6) / maxB, min(1.0, 3.0 / maxB), 1.0);
            }
        }
        fxScale *= uRingFx;
    } else {
        d = sdRoundRect(p, half_, r);
    }

    float aa     = max(fwidth(d), 0.001);
    float inside = 1.0 - smoothstep(-aa, aa, d);
    if (inside <= 0.001)
        discard;

    // Edge lensing — crease-free analytic outward normal for the simple shape,
    // numeric gradient for composites (the field is smooth near its boundary,
    // which is the only place normal-driven effects act).
    vec2  n;
    float depth;
    if (composite && uHasCutout > 0.5) {
        // Edge effects follow the INNER OUTLINE only (screen-perimeter boundary
        // gets coverage but no lensing) — with the two jobs SPLIT:
        //   depth ← the RAW field (true silhouette): bands/rim/lens hug the
        //           visible bend at the user's radius, all the way around;
        //   n     ← the SWEPT field: crease-free flow direction through the
        //           junction (what killed the "mangled" streaks at tight arcs).
        depth = max(sdHoleRaw(localPx), 0.0);
        n     = -holeNormal(localPx);
    } else {
        n     = composite ? sceneNormal(localPx) : rrNormal(p, half_, r);
        depth = -d;
    }
    // Field diagnostics — visualize what drives the effects instead of arguing
    // about it: depth as 10px contour bands (red = shallow), normals as RG.
    if (uDebugField > 0.5) {
        if (uDebugField < 1.5) {
            float band10 = fract(depth / 10.0);
            float shallow = 1.0 - smoothstep(0.0, 40.0, depth);
            fragColor = vec4(shallow, band10 * 0.8, 1.0 - shallow, 1.0) * inside;
        } else if (uDebugField < 2.5) {
            fragColor = vec4(n * 0.5 + 0.5, 0.5, 1.0) * inside;
        } else {
            // debug:3 — plug uniform sanity: green inside plug rects, red
            // outside-but-near; magenta if the uniform is empty.
            if (uPlug0.z <= 0.5) {
                fragColor = vec4(1.0, 0.0, 1.0, 1.0) * inside;
            } else {
                float pd = plugDist(localPx, uPlug0);
                float ins = pd < 0.0 ? 1.0 : 0.0;
                float near_ = 1.0 - smoothstep(0.0, 60.0, abs(pd));
                fragColor = vec4(near_ * (1.0 - ins), ins, 0.0, 1.0) * inside;
            }
        }
        return;
    }

    // The refraction must reach the edge, anti-aliased only by a ~3px ease so the boundary does
    // not hard-seam. A wider soft-in would leave a dead zone where the edge does not bend at all
    // and the effect starts visibly inside the surface; because srcUv samples the surrounding
    // backdrop, the edge can refract right up to the border like a real glass slab.
    // Floors keep thin ring strips wearing a hairline of the same material
    // instead of going matte-flat next to their fully-treated parts.
    float edgeBandFx = max(uEdgeBandPx * fxScale, composite ? 2.0 : 0.001);
    // Refraction keeps FULL amplitude everywhere — the ring scale narrows the
    // BANDS so effects fit a thin strip, but attenuating displacement too made
    // a 10px frame read matte (8% of ~45px = under a pixel of bend). The
    // composite depth cap below already stops a thin strip from pulling
    // content further than the glass behind it.
    float refractFx  = uRefractPx;
    float lensBandFx = max(uLensBandPx * fxScale, 1.0);
    float bevelFx    = max(uBevelPx * fxScale, composite ? 2.0 : 0.001);
    float edge   = 1.0 - smoothstep(0.0, edgeBandFx, depth);
    float softIn = smoothstep(0.0, 3.0, depth);
    float bend   = edge * edge * softIn;
    // Refraction — displace the sample inward along the normal, eased in over the edge band.
    // Composite assemblies cap the displacement by the LOCAL glass depth: a
    // 16px frame strip must not "see" a window 45px away — an edge only pulls
    // content from just beyond its boundary, proportional to how much glass
    // is actually behind the pixel. Simple elements keep their exact optics.
    float dispPx = bend * refractFx;
    if (composite)
        dispPx = min(dispPx, depth * 1.2 + 2.0);
    vec2  uv = localUv - n * (dispPx / max(uDestSize, vec2(1.0)));

    // Convex-slab lens — magnify + displace the backdrop over the glass-THICKNESS band from each
    // edge (a real slab bends light most at its periphery). The band is a fixed px depth (clamped
    // CPU-side), so a bar and a big panel refract over the same depth and thin bars keep a clear
    // centre. Same soft-in from the edge so it doesn't seam either.
    float lensSoftIn = smoothstep(0.0, 3.0, depth);   // tiny AA only — the lens must reach the edge too
    float lensR      = (1.0 - smoothstep(0.0, lensBandFx, depth)) * lensSoftIn;   // ~edge → up → 0 inner
    // Boundary-relative for EVERY element: a fixed physical inward pull along the
    // edge normal, bounded by the (CPU-clamped) lens band — identical lens depth on
    // a small pill and a large panel. The old simple-element center-pull
    // (0.5 - localUv) was UV-proportional and blew up on big surfaces (the settings
    // ribbon sucked its backdrop in by tens of px); composites already did this.
    uv += (-n) * (lensR * lensR) * uLens * (lensBandFx / max(uDestSize, vec2(1.0)));

    // Whole-surface convex centre lens — adapted from OverShifted/LiquidGlass (see THIRD_PARTY_NOTICES.md).
    // Radial magnification of the backdrop toward centre — the "glassy centre" current lacked. Reads on
    // square-ish glass; faded out by aspect on wide shapes (a centre pull mirrors on a wide bar). Simple
    // elements only; composite frames keep the edge optics. Strength 0.5 (the look James locked).
    if (!composite) {
        float shortH2   = min(half_.x, half_.y);
        float aspect    = shortH2 / max(max(half_.x, half_.y), 1.0);    // 1 square → 0 wide
        float centerAmt = 0.5 * smoothstep(0.35, 0.9, aspect);
        if (centerAmt > 0.0001) {
            float distIn      = clamp(depth / max(shortH2, 1.0), 0.0, 1.0);
            float fc          = 1.0 - 2.3 * pow(5.2 * 2.718281828459045, -6.9 * distIn - 0.7);   // upstream f(dist)
            float contraction = pow(clamp(fc, 0.0, 1.0), 3.0);
            vec2  dpx = (localUv - 0.5) * uDestSize * (1.0 - contraction) * centerAmt;
            float cap = shortH2 * 0.8;
            float dl  = length(dpx);
            if (dl > cap) dpx *= cap / dl;
            uv -= dpx / max(uDestSize, vec2(1.0));
        }
    }

    vec3 glass = backdrop(uv);

    // Chromatic aberration — real glass disperses light at its refracting edges, splitting the
    // backdrop into faint R/G/B fringes. The channel taps read the SAME pre-blurred texture
    // (sharp taps would re-sharpen the R/B channels across the edge band, undoing the frost
    // exactly where it is most visible), so dispersion costs two extra taps, not two blurs.
    // Composite elements are MONITOR-sized: the min-dimension normalization
    // over-scales the UV offset (dest/minDest ≈ aspect) and the corners paint
    // wide rainbow splitting. Compute the dispersion in PHYSICAL px like the
    // refraction displacement; simple elements keep their tuned look.
    if (composite) {
        float caPx = bend * uChroma * refractFx;
        if (caPx > 0.05) {
            vec2 cd = n * (caPx / max(uDestSize, vec2(1.0)));
            glass.r = mix(glass.r, backdrop(uv + cd).r, 0.7);
            glass.b = mix(glass.b, backdrop(uv - cd).b, 0.7);
        }
    } else {
        float caAmt = bend * uChroma * (refractFx / max(min(uDestSize.x, uDestSize.y), 1.0));
        if (caAmt > 0.0008) {
            vec2 cd = n * caAmt;   // dispersion along the refraction direction
            glass.r = mix(glass.r, backdrop(uv + cd).r, 0.7);
            glass.b = mix(glass.b, backdrop(uv - cd).b, 0.7);
        }
    }

    // Frosted-glass veil = luminance COMPRESSION toward the veil midtone (lifts
    // blacks + lowers whites — Apple's "filters out the darks", low contrast)
    // with CHROMA preserved SEPARATELY so colours stay saturated. Decoupling the
    // two is what a mix-toward-grey can't do (it greys the colour to calm the
    // contrast). uTint.a = compression amount; uTint.rgb luma = target midtone;
    // uVeilSat = how much chroma survives.
    float srcLuma  = dot(glass, vec3(0.299, 0.587, 0.114));
    vec3  srcChroma = glass - vec3(srcLuma);
    float veilLuma = dot(uTint.rgb, vec3(0.299, 0.587, 0.114));
    float bgCurve  = clamp(srcLuma + 0.7*srcLuma*(1.0-srcLuma), 0.0, 1.0);  // midtone lift, dark toe anchored
    float newLuma  = mix(bgCurve, veilLuma, clamp(uTint.a, 0.0, 1.0));
    // MEASURED MODEL: glass_luma = transmittance·backdrop + scatter, where
    // transmittance = 1-uTint.a and scatter = uTint.a·veilLuma (linear, no
    // floor — the intercept IS the scatter). Chroma kept ~95% (Apple barely
    // desaturates; the pastel look comes from the luminance compression).
    // STAINED: the veil colour's own CHROMA rides in at veil strength — zero
    // for the neutral (system) veil, so the calibrated material is untouched;
    // an accent veil colours the glass without changing its measured luminance
    // (the shell sends accents luma-normalised to the neutral stops).
    vec3 tintChroma = uTint.rgb - vec3(veilLuma);
    glass = clamp(vec3(newLuma) + srcChroma * clamp(uVeilSat, 0.0, 1.0) + tintChroma * clamp(uTint.a, 0.0, 1.0), 0.0, 1.0);

    // ── Directional edge finish (calibrated to the iOS dark-on-dark refs) ──
    // The edge reads as a lit glass rim: WHITE where it faces up/down (top &
    // bottom) and BLACK where it faces left/right (the sides). n is the outward
    // edge normal, so n.y^2 peaks at top/bottom and n.x^2 peaks at the sides.
    float vert  = n.y * n.y;   // ~1 at the top & bottom edges
    float horiz = n.x * n.x;   // ~1 at the left & right edges

    // Interior highlight (top/bottom) + shadow (sides), fading fast inward.
    // MEASURED (iOS cards 2 & 4): the interior highlight is a NARROW edge-hugging
    // band (~5px), not the 30px bevel — Apple's centre stays at the veil value
    // while only a thin lip near the top/bottom edge lifts. Driving this off the
    // wide bevel band held the highlight far into the surface and washed the
    // centre out. Narrow band = thin lip, clean centre (bevel still caps it).
    float finishBandPx = min(bevelFx, max(uRimWidthPx * 1.5, 3.0));
    float band = 1.0 - smoothstep(0.0, finishBandPx, depth);
    float hi = band * vert  * uHighlight;
    float sh = band * horiz * uShadow;
    glass = mix(glass, vec3(1.0), hi);
    glass *= (1.0 - sh);

    // Inner glass-wall depth — soft light-independent darkening from the edge.
    float wall = pow(edge, 2.2) * uEdgeDepth;
    glass *= (1.0 - wall);

    // Surface gloss — broad soft face sheen; r.gloss is ramped OUT on the CPU as
    // the veil thickens (glossy at low tint -> matte at high = the iOS texture).
    vec3  hiTint = mix(vec3(1.0), min(glass * 1.7, vec3(1.0)), 0.6);
    // Border-hugging sheen: the lit side glows at the edge and falls off FAST
    // into the interior (edge^2), so the light never washes across the face —
    // the centre is free to darken with the backdrop (Apple's border-drop-off).
    float along = clamp(dot(n, uLightDir) * 0.5 + 0.5, 0.0, 1.0) * edge * edge;
    float sheen = along * uGloss;
    glass = mix(glass, hiTint, sheen);

    // Specular rim — thin glint at the very edge: white top/bottom, black sides.
    float rim = 1.0 - smoothstep(0.0, max(uRimWidthPx, 0.001), depth);
    glass = mix(glass, vec3(1.0), rim * vert  * uSpecular);
    glass = mix(glass, vec3(0.0), rim * horiz * uSpecular);

    float a = clamp(uAlpha, 0.0, 1.0) * inside;
    // Per-part fade: appearing/vanishing parts dissolve instead of popping.
    // CLIPPED TO INSIDE THE CUTOUT: parts are sent extended THROUGH the ring,
    // so an unclipped fade also erased the bar strip's own glass in the
    // part's column (James's vanishing bottom bar). The ring band is
    // permanent chrome — it never fades with a guest surface.
    float pA = 1.0;
    float insideHole = 1.0;
    if (uHasCutout > 0.5)
        insideHole = 1.0 - smoothstep(-12.0, 0.0, sdRectAt(localPx, uCutRect, uCutRadius));
    if (uPart0.z > 0.5 && uPartAlpha.x < 0.999) {
        vec4 r0 = uPartExt0.z > 0.5 ? uPartExt0 : uPart0;
        pA = min(pA, mix(1.0, uPartAlpha.x, (1.0 - smoothstep(0.0, 24.0, max(plugDist(localPx, r0), 0.0))) * insideHole));
    }
    if (uPart1.z > 0.5 && uPartAlpha.y < 0.999) {
        vec4 r1 = uPartExt1.z > 0.5 ? uPartExt1 : uPart1;
        pA = min(pA, mix(1.0, uPartAlpha.y, (1.0 - smoothstep(0.0, 24.0, max(plugDist(localPx, r1), 0.0))) * insideHole));
    }
    if (uPart2.z > 0.5 && uPartAlpha.z < 0.999) {
        vec4 r2 = uPartExt2.z > 0.5 ? uPartExt2 : uPart2;
        pA = min(pA, mix(1.0, uPartAlpha.z, (1.0 - smoothstep(0.0, 24.0, max(plugDist(localPx, r2), 0.0))) * insideHole));
    }
    if (uPart3.z > 0.5 && uPartAlpha.w < 0.999) {
        vec4 r3 = uPartExt3.z > 0.5 ? uPartExt3 : uPart3;
        pA = min(pA, mix(1.0, uPartAlpha.w, (1.0 - smoothstep(0.0, 24.0, max(plugDist(localPx, r3), 0.0))) * insideHole));
    }
    fragColor = vec4(glass * a, a) * pA;   // premultiplied
}
)GLSL";
}

// ── Separable blur pass ────────────────────────────────────────────────────
// One 1-D Gaussian pass over a source rect, into a half-res FB. Run twice
// (horizontal from the capture subrect, then vertical) it produces the frosted
// backdrop the glass shader samples with a single tap. Dense, smooth and cheap:
// the cost is per element-pixel at half resolution, not per screen fragment.
const char* blurVertexSource() {
    return R"GLSL(#version 320 es
in vec2 pos;
in vec2 texcoord;
out vec2 v_uv;
void main() {
    gl_Position = vec4(pos, 0.0, 1.0);
    v_uv = texcoord;
}
)GLSL";
}

const char* blurFragmentSource() {
    return R"GLSL(#version 320 es
precision highp float;
in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
uniform sampler2D tex;
uniform vec2  uUvOff;    // source-UV origin of the rect being blurred
uniform vec2  uUvScale;  // source-UV size of that rect
uniform vec2  uDirTexel; // blur direction × source texel step (per dest px)
uniform float uRadius;   // radius in dest px
void main() {
    vec2  base  = uUvOff + v_uv * uUvScale;
    float r     = max(uRadius, 0.001);
    int   K     = int(clamp(ceil(r), 1.0, 24.0));
    float step_ = r / float(K);
    float sigma = max(r * 0.5, 0.35);
    vec3  sum   = texture(tex, base).rgb;
    float wsum  = 1.0;
    for (int i = 1; i <= K; ++i) {
        float o = float(i) * step_;
        float w = exp(-(o * o) / (2.0 * sigma * sigma));
        vec2  d = uDirTexel * o;
        sum  += (texture(tex, base + d).rgb + texture(tex, base - d).rgb) * w;
        wsum += 2.0 * w;
    }
    fragColor = vec4(sum / wsum, 1.0);
}
)GLSL";
}

bool ensureShader() {
    if (g_shader && g_shaderCompiled) return true;
    // Retry a few times instead of latching off forever: the GL renderer isn't always ready at the
    // first apply, and a permanent disable-until-reload from one transient miss is a bad failure mode.
    if (g_shaderAttempted && !g_shaderCompiled && g_shaderRetries >= 5) return false;
    g_shaderAttempted = true;
    if (!Render::GL::g_pHyprOpenGL) { g_shaderError = "OpenGL renderer unavailable"; ++g_shaderRetries; return false; }

    auto shader = makeShared<CShader>();
    if (!shader || !shader->createProgram(vertexSource(), fragmentSource(), true, true)) {
        g_shader.reset();
        g_shaderError    = "shader compile/link failed";
        g_shaderCompiled = false;
        ++g_shaderRetries;
        dbgLog("", "shader.fail", "retry " + std::to_string(g_shaderRetries));
        return false;
    }
    shader->setUsesCustomUV(true);
    g_shader         = shader;
    g_shaderCompiled = true;
    g_shaderError.clear();
    // P4: the shader/GL program was (re)created — bump the draw epoch so every
    // prior draw-confirm is void (a descriptor is confirmed only when it has
    // drawn at BOTH the current rev AND the current epoch). Lock-free: written
    // and read on the render thread; the status build reads it under the lock.
    // NOTE this runs in the draw phase (outside the collection lock), so it must
    // NOT touch g_elements directly — the epoch counter is the safe indirection.
    ++g_drawEpoch;
    g_glassDirty = true;

    // Resolve + cache custom-uniform locations once.
    const GLuint prog = shader->program();
    auto L = [&](const char* n) { return glGetUniformLocation(prog, n); };
    g_uni.srcTL = L("uSrcTL");   g_uni.srcTR = L("uSrcTR");   g_uni.srcBR = L("uSrcBR");   g_uni.srcBL = L("uSrcBL");
    g_uni.destSize = L("uDestSize"); g_uni.radiusPx = L("uRadiusPx"); g_uni.refractPx = L("uRefractPx");
    g_uni.edgeBandPx = L("uEdgeBandPx"); g_uni.bevelPx = L("uBevelPx"); g_uni.highlight = L("uHighlight"); g_uni.shadow = L("uShadow");
    g_uni.lightDir = L("uLightDir");
    g_uni.specular = L("uSpecular"); g_uni.rimWidthPx = L("uRimWidthPx"); g_uni.tint = L("uTint"); g_uni.veilSat = L("uVeilSat");
    g_uni.alpha = L("uAlpha"); g_uni.chroma = L("uChroma"); g_uni.edgeDepth = L("uEdgeDepth"); g_uni.lens = L("uLens"); g_uni.lensBandPx = L("uLensBandPx"); g_uni.gloss = L("uGloss");
    g_uni.blurTex = L("uBlurTexS"); g_uni.blurOff = L("uBlurOff"); g_uni.blurScale = L("uBlurScale"); g_uni.useBlur = L("uUseBlur");
    g_uni.cutRect = L("uCutRect"); g_uni.cutRadius = L("uCutRadius"); g_uni.hasCutout = L("uHasCutout");
    g_uni.part0 = L("uPart0"); g_uni.part1 = L("uPart1"); g_uni.part2 = L("uPart2"); g_uni.part3 = L("uPart3");
    g_uni.partC0 = L("uPartC0"); g_uni.partC1 = L("uPartC1"); g_uni.partC2 = L("uPartC2"); g_uni.partC3 = L("uPartC3");
    g_uni.partK0 = L("uPartK0"); g_uni.partK1 = L("uPartK1"); g_uni.partK2 = L("uPartK2"); g_uni.partK3 = L("uPartK3");
    g_uni.plug0 = L("uPlug0"); g_uni.plug1 = L("uPlug1"); g_uni.plug2 = L("uPlug2"); g_uni.plug3 = L("uPlug3");
    g_uni.partE0 = L("uPartExt0"); g_uni.partE1 = L("uPartExt1"); g_uni.partE2 = L("uPartExt2"); g_uni.partE3 = L("uPartExt3");
    g_uni.ringFx = L("uRingFx"); g_uni.curvePx = L("uCurvePx"); g_uni.debugField = L("uDebugField");
    g_uni.partAlpha = L("uPartAlpha");

    // The quad never changes — upload it once instead of per element per frame.
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    glBindBuffer(GL_ARRAY_BUFFER, shader->getUniformLocation(SHADER_SHADER_VBO));
    auto verts = Render::GL::fullVerts;
    verts[0].u = 0.0F; verts[0].v = 0.0F;
    verts[1].u = 0.0F; verts[1].v = 1.0F;
    verts[2].u = 1.0F; verts[2].v = 0.0F;
    verts[3].u = 1.0F; verts[3].v = 1.0F;
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Blur pass program — failure degrades to sharp glass (uUseBlur 0), never fatal.
    auto blur = makeShared<CShader>();
    if (blur && blur->createProgram(blurVertexSource(), blurFragmentSource(), true, true)) {
        g_blurShader  = blur;
        g_blurCompiled = true;
        const GLuint bprog = blur->program();
        g_blurUni.tex      = glGetUniformLocation(bprog, "tex");
        g_blurUni.uvOff    = glGetUniformLocation(bprog, "uUvOff");
        g_blurUni.uvScale  = glGetUniformLocation(bprog, "uUvScale");
        g_blurUni.dirTexel = glGetUniformLocation(bprog, "uDirTexel");
        g_blurUni.radius   = glGetUniformLocation(bprog, "uRadius");
        if (!g_quadVao) {
            const GLint posLoc = glGetAttribLocation(bprog, "pos");
            const GLint uvLoc  = glGetAttribLocation(bprog, "texcoord");
            // Clip-space quad; uv (0,0) rides NDC (-1,-1) so FB texel (0,0) holds the
            // rect origin — the sampling math in blurBackdropForElement relies on it.
            const GLfloat quad[16] = {
                -1.F, -1.F, 0.F, 0.F,
                -1.F,  1.F, 0.F, 1.F,
                 1.F, -1.F, 1.F, 0.F,
                 1.F,  1.F, 1.F, 1.F,
            };
            glGenVertexArrays(1, &g_quadVao);
            glGenBuffers(1, &g_quadVbo);
            glBindVertexArray(g_quadVao);
            glBindBuffer(GL_ARRAY_BUFFER, g_quadVbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
            if (posLoc >= 0) {
                glEnableVertexAttribArray(posLoc);
                glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
            }
            if (uvLoc >= 0) {
                glEnableVertexAttribArray(uvLoc);
                glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void*>(2 * sizeof(GLfloat)));
            }
            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
    } else {
        g_blurShader.reset();
        g_blurCompiled = false;
    }
    dbgLog("", "shader.ok", g_blurCompiled ? "main + blur" : "main only (blur FAILED, sharp fallback)");
    return true;
}

// ── Raw backdrop capture (PRESERVED — feedback-safe currentFB copy) ────────
std::map<std::string, Vector2D> g_captureKnownSize;

SP<Render::IFramebuffer> captureFBFor(const PHLMONITOR& monitor, const SP<Render::IFramebuffer>& sourceFB, const std::string& stageKey, bool& fullCopy) {
    if (!g_pHyprRenderer || !monitor || !sourceFB) return nullptr;
    // Keyed per monitor AND render stage: PRE_WINDOW under-glass must never
    // reuse the POST_WINDOWS composite (and vice versa) — the persistent FB is
    // a per-stage clean-backdrop accumulator now, not a per-frame scratch.
    const std::string key = monitor->m_name + "/" + stageKey;
    auto& fb = g_captureFBs[key];
    fullCopy = false;
    if (!fb) {
        fb       = g_pHyprRenderer->createFB("hyprfluidglass backdrop " + key);
        fullCopy = true;
    }
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
    const Vector2D sz{static_cast<double>(width), static_cast<double>(height)};
    if (g_captureKnownSize[key] != sz) {
        g_captureKnownSize[key] = sz;
        fullCopy                = true;
    }
    return fb;
}

SP<Render::ITexture> captureBackdropForCurrentMonitor(const std::string& stageKey) {
    if (!g_pHyprRenderer) return nullptr;
    const auto monitor = g_pHyprRenderer->renderData().pMonitor.lock();
    if (!monitor) return nullptr;

    const auto sourceFB = g_pHyprRenderer->renderData().currentFB;
    if (!sourceFB || !sourceFB->isAllocated() || !sourceFB->getTexture()) return nullptr;
    bool       fullCopy  = false;
    const auto captureFB = captureFBFor(monitor, sourceFB, stageKey, fullCopy);
    if (!captureFB || !captureFB->isAllocated()) return nullptr;
    const auto srcTex = sourceFB->getTexture();
    if (!srcTex || !srcTex->ok()) return nullptr;

    const CBox srcBox = {0, 0, srcTex->m_size.x, srcTex->m_size.y};
    if (srcBox.width <= 0 || srcBox.height <= 0) return nullptr;

    // DAMAGE-SCOPED ACCUMULATION — the flicker fix. Only pixels rendered fresh
    // THIS frame (the damage region) are copied; everything else keeps its
    // previously-accumulated clean backdrop. A full-FB copy here grabbed stale
    // composite (including our own drawn glass) outside the damage region, so
    // cursor-trail / focus-burst frames fed the glass its own pixels back —
    // the on/off-monitor strobe James reported. Full copy only on alloc/resize.
    CRegion copyRegion = CRegion{CBox(0, 0, srcBox.width, srcBox.height)};
    if (!fullCopy) {
        copyRegion = g_pHyprRenderer->m_renderData.damage;
        copyRegion.intersect(CBox(0, 0, srcBox.width, srcBox.height));
        if (copyRegion.empty()) {
            const auto cached = captureFB->getTexture();
            return (cached && cached->ok()) ? cached : nullptr;
        }
    }

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
        copy.damage        = copyRegion;
        copy.allowCustomUV = false;
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

// ── Per-element separable frost ────────────────────────────────────────────
// The sampling apron around an element: frost radius + refraction reach + a
// margin, in logical px. Used both for the capture subrect being blurred and
// for the pass bounding box (damage this close can change what the glass shows).
constexpr double GLASS_APRON_LOGICAL = 120.0;

struct BlurResult {
    SP<Render::ITexture> tex;
    double offU = 0, offV = 0, sclU = 1, sclV = 1;   // blurred rect in capture-UV terms
    bool   ok = false;
};

// Blur the capture subrect behind an element (box + apron) into the element's
// cached half-res FB pair: horizontal pass from the capture, then vertical over
// the intermediate. The glass shader then frosts with a single tap. Runs inside
// our pass draw (GL active); caller restores the viewport.
BlurResult blurElementBackdrop(const GlassElement& el, const SP<Render::ITexture>& capture,
                               const Pt& uvTL, const Pt& uvTR, const Pt& uvBR, const Pt& uvBL,
                               double blurPhys, double apronPhys) {
    BlurResult res;
    if (!g_blurCompiled || !g_blurShader || !g_quadVao || !g_pHyprRenderer)
        return res;
    const double cw = capture->m_size.x, ch = capture->m_size.y;
    if (cw <= 1 || ch <= 1)
        return res;

    const double minU = std::min(std::min(uvTL.x, uvTR.x), std::min(uvBR.x, uvBL.x));
    const double maxU = std::max(std::max(uvTL.x, uvTR.x), std::max(uvBR.x, uvBL.x));
    const double minV = std::min(std::min(uvTL.y, uvTR.y), std::min(uvBR.y, uvBL.y));
    const double maxV = std::max(std::max(uvTL.y, uvTR.y), std::max(uvBR.y, uvBL.y));
    const double x0   = clampd(minU * cw - apronPhys, 0.0, cw);
    const double x1   = clampd(maxU * cw + apronPhys, 0.0, cw);
    const double y0   = clampd(minV * ch - apronPhys, 0.0, ch);
    const double y1   = clampd(maxV * ch + apronPhys, 0.0, ch);
    const int    srcW = static_cast<int>(std::ceil(x1 - x0));
    const int    srcH = static_cast<int>(std::ceil(y1 - y0));
    if (srcW < 2 || srcH < 2)
        return res;
    const int halfW = std::max(1, srcW / 2);
    const int halfH = std::max(1, srcH / 2);

    auto& fbs = g_elemFBs[el.id];
    if (!fbs.a) fbs.a = g_pHyprRenderer->createFB("hyprfluidglass blur A " + el.id);
    if (!fbs.b) fbs.b = g_pHyprRenderer->createFB("hyprfluidglass blur B " + el.id);
    if (!fbs.a || !fbs.b)
        return res;
    fbs.a->alloc(halfW, halfH, DRM_FORMAT_ABGR8888);
    fbs.b->alloc(halfW, halfH, DRM_FORMAT_ABGR8888);
    if (!fbs.a->isAllocated() || !fbs.b->isAllocated())
        return res;

    // Radii are in destination (half-res) pixels; one dest px spans 2 capture px.
    const double rHalf = std::max(blurPhys * 0.5, 0.25);

    auto runPass = [&](const SP<Render::IFramebuffer>& dst, const SP<Render::ITexture>& src,
                       double offU, double offV, double sclU, double sclV, double dirX, double dirY) {
        auto guard = g_pHyprRenderer->bindTempFB(dst);
        // Tracked variants only: the renderer caches the current viewport and
        // program, and raw gl* calls here would desync those caches — the next
        // tracked set would no-op and the whole frame would render wrong.
        g_pHyprRenderer->setViewport(0, 0, halfW, halfH);
        Render::GL::g_pHyprOpenGL->useShader(g_blurShader);
        glActiveTexture(GL_TEXTURE0);
        src->bind();
        src->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        src->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        src->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        src->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        if (g_blurUni.tex >= 0)      glUniform1i(g_blurUni.tex, 0);
        if (g_blurUni.uvOff >= 0)    glUniform2f(g_blurUni.uvOff, static_cast<float>(offU), static_cast<float>(offV));
        if (g_blurUni.uvScale >= 0)  glUniform2f(g_blurUni.uvScale, static_cast<float>(sclU), static_cast<float>(sclV));
        if (g_blurUni.dirTexel >= 0) glUniform2f(g_blurUni.dirTexel, static_cast<float>(dirX), static_cast<float>(dirY));
        if (g_blurUni.radius >= 0)   glUniform1f(g_blurUni.radius, static_cast<float>(rHalf));
        glBindVertexArray(g_quadVao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        src->unbind();
    };

    Render::GL::g_pHyprOpenGL->scissor(nullptr);
    g_pHyprRenderer->blend(false);
    runPass(fbs.a, capture, x0 / cw, y0 / ch, (x1 - x0) / cw, (y1 - y0) / ch, 2.0 / cw, 0.0);
    if (const auto texA = fbs.a->getTexture(); texA && texA->ok())
        runPass(fbs.b, texA, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0 / halfH);
    g_pHyprRenderer->blend(true);

    const auto out = fbs.b->getTexture();
    if (!out || !out->ok())
        return res;
    res.tex  = out;
    res.offU = x0 / cw;
    res.offV = y0 / ch;
    res.sclU = (x1 - x0) / cw;
    res.sclV = (y1 - y0) / ch;
    res.ok   = true;
    return res;
}

// ── Draw one glass element ────────────────────────────────────────────────
bool drawElement(const GlassElement& el, const SP<Render::ITexture>& capture) {
    if (!g_pHyprRenderer || !capture || !capture->ok()) return false;
    if (!ensureShader()) return false;
    const auto monitor = g_pHyprRenderer->renderData().pMonitor.lock();
    if (!monitor || monitor->m_scale <= 0) return false;

    const double scale = monitor->m_scale;
    const int    tf    = static_cast<int>(monitor->m_transform);
    const double cw    = capture->m_size.x;
    const double ch    = capture->m_size.y;
    if (cw <= 0 || ch <= 0 || el.w <= 0 || el.h <= 0) return false;

    // Enter/exit animation: shrink around the centre by animScale (radius + material
    // scale with it, so the whole glass grows/shrinks coherently); fade via uAlpha.
    const double as = clampd(el.animScale, 0.0, 1.0);
    const double ew = el.w * as, eh = el.h * as;
    const double ex = el.x + (el.w - ew) * 0.5;
    const double ey = el.y + (el.h - eh) * 0.5;

    // Element rect in display-space physical px (monitor-local).
    const double dx = ex * scale, dy = ey * scale;
    const double dw = ew * scale, dh = eh * scale;

    // Source-texture UV of the 4 corners (transform-mapped for rotated displays).
    // NOT clamped to [0,1]: when the window is taller/wider than the monitor, the off-screen
    // corners must keep their true (out-of-range) UV so the VISIBLE part still maps 1:1. Clamping
    // here squashes the whole backdrop into the visible area -> the height-linked vertical stretch.
    // CLAMP_TO_EDGE on the texture covers any sample that lands outside the captured frame.
    auto cornerUv = [&](double px, double py) -> Pt {
        Pt phys = (tf == 0) ? Pt{px, py} : inverseTransformPoint({px, py}, tf, cw, ch);
        return {phys.x / cw, phys.y / ch};
    };
    const Pt uvTL = cornerUv(dx, dy);
    const Pt uvTR = cornerUv(dx + dw, dy);
    const Pt uvBR = cornerUv(dx + dw, dy + dh);
    const Pt uvBL = cornerUv(dx, dy + dh);

    CBox box(dx, dy, dw, dh);
    CRegion overlap{g_pHyprRenderer->m_renderData.damage};
    overlap.intersect(box.x, box.y, box.width, box.height);
    if (overlap.empty()) return false; // backdrop under the glass didn't change
    CRegion boxRegion{box};

    CBox projected = box;
    g_pHyprRenderer->m_renderData.renderModif.applyToBox(projected);

    auto transform = capture->m_transform;
    if (g_pHyprRenderer->monitorTransformEnabled()) {
        const auto inv = Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform));
        transform = Math::composeTransform(inv, transform);
    }
    const auto glMatrix = g_pHyprRenderer->projectBoxToTarget(projected, transform);

    GlassElement es = el;
    es.w = ew; es.h = eh; es.radius = el.radius * as;
    const ResolvedParams rp = resolveParams(es, scale);

    // Frost the backdrop subrect first (separable two-pass at half res); the
    // glass shader then reads it with one tap per fragment. The apron covers
    // everything the shader can reach: blur spread, refraction and CA offsets.
    BlurResult blur;
    if (rp.blurPx >= 0.5) {
        const double apronPhys = rp.blurPx * 2.0 + rp.refractPx + 12.0;
        blur = blurElementBackdrop(el, capture, uvTL, uvTR, uvBR, uvBL, rp.blurPx, apronPhys);
        g_pHyprRenderer->setViewport(0, 0, static_cast<int>(monitor->m_pixelSize.x), static_cast<int>(monitor->m_pixelSize.y));
    }

    auto shader = Render::GL::g_pHyprOpenGL->useShader(g_shader);
    if (!shader || shader->program() == 0) return false;

    glActiveTexture(GL_TEXTURE0);
    capture->bind();
    capture->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    capture->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    capture->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    capture->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    if (blur.ok) {
        glActiveTexture(GL_TEXTURE1);
        blur.tex->bind();
        blur.tex->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        blur.tex->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        blur.tex->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        blur.tex->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glActiveTexture(GL_TEXTURE0);
    }

    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, glMatrix.getMatrix());
    shader->setUniformInt(SHADER_TEX, 0);
    shader->setUniformFloat(SHADER_ALPHA, 1.0F);

    // Cached uniform locations (resolved once at link) — set directly, no per-frame lookups.
    auto u1 = [&](GLint l, float v) { if (l >= 0) glUniform1f(l, v); };
    auto u2 = [&](GLint l, float x, float y) { if (l >= 0) glUniform2f(l, x, y); };
    auto u4 = [&](GLint l, float x, float y, float z, float w) { if (l >= 0) glUniform4f(l, x, y, z, w); };

    u2(g_uni.srcTL, static_cast<float>(uvTL.x), static_cast<float>(uvTL.y));
    u2(g_uni.srcTR, static_cast<float>(uvTR.x), static_cast<float>(uvTR.y));
    u2(g_uni.srcBR, static_cast<float>(uvBR.x), static_cast<float>(uvBR.y));
    u2(g_uni.srcBL, static_cast<float>(uvBL.x), static_cast<float>(uvBL.y));
    u2(g_uni.destSize, static_cast<float>(dw), static_cast<float>(dh));
    u1(g_uni.radiusPx, static_cast<float>(rp.radiusPx));
    u1(g_uni.refractPx, static_cast<float>(rp.refractPx));
    if (g_uni.blurTex >= 0) glUniform1i(g_uni.blurTex, 1);
    u1(g_uni.useBlur, blur.ok ? 1.0F : 0.0F);
    u2(g_uni.blurOff, static_cast<float>(blur.offU), static_cast<float>(blur.offV));
    u2(g_uni.blurScale, static_cast<float>(std::max(blur.sclU, 1e-6)), static_cast<float>(std::max(blur.sclV, 1e-6)));
    u1(g_uni.edgeBandPx, static_cast<float>(rp.rimBandPx));
    u1(g_uni.bevelPx, static_cast<float>(rp.bevelPx));
    u1(g_uni.highlight, static_cast<float>(rp.highlight));
    u1(g_uni.shadow,    static_cast<float>(rp.shadow));
    // Shader Y is mirrored vs the lab → flip light-Y so 90° = top (matches the lab).
    u2(g_uni.lightDir, static_cast<float>(rp.lightX), static_cast<float>(-rp.lightY));
    u1(g_uni.specular, static_cast<float>(rp.specular));
    u1(g_uni.rimWidthPx, static_cast<float>(rp.rimWidthPx));
    u4(g_uni.tint, rp.tintR, rp.tintG, rp.tintB, static_cast<float>(rp.tintStrength));
    u1(g_uni.veilSat, static_cast<float>(rp.veilSat));
    u1(g_uni.alpha, static_cast<float>(clampd(el.renderAlpha, 0.0, 1.0)));
    u1(g_uni.chroma, static_cast<float>(rp.chroma));
    u1(g_uni.edgeDepth, static_cast<float>(rp.edgeDepth));
    u1(g_uni.lens, static_cast<float>(rp.lens));
    u1(g_uni.lensBandPx, static_cast<float>(rp.lensBandPx));
    u1(g_uni.gloss, static_cast<float>(rp.gloss));

    // Composite shape (element-local logical → physical px, following the
    // enter/exit animation transform, which shrinks geometry around the centre).
    auto mapRect = [&](double x, double y, double w, double h, float out[4]) {
        out[0] = static_cast<float>((x * as + el.w * (1.0 - as) * 0.5) * scale);
        out[1] = static_cast<float>((y * as + el.h * (1.0 - as) * 0.5) * scale);
        out[2] = static_cast<float>(w * as * scale);
        out[3] = static_cast<float>(h * as * scale);
    };
    u1(g_uni.hasCutout, el.hasCutout ? 1.0F : 0.0F);
    if (el.hasCutout && g_uni.cutRect >= 0) {
        float cr[4];
        mapRect(el.cutX, el.cutY, el.cutW, el.cutH, cr);
        glUniform4f(g_uni.cutRect, cr[0], cr[1], cr[2], cr[3]);
        u1(g_uni.cutRadius, static_cast<float>(el.cutR * as * scale));
    }
    {
        const GLint partLoc[4]  = {g_uni.part0, g_uni.part1, g_uni.part2, g_uni.part3};
        const GLint partCLoc[4] = {g_uni.partC0, g_uni.partC1, g_uni.partC2, g_uni.partC3};
        const GLint partKLoc[4] = {g_uni.partK0, g_uni.partK1, g_uni.partK2, g_uni.partK3};
        const GLint plugLoc[4]  = {g_uni.plug0, g_uni.plug1, g_uni.plug2, g_uni.plug3};
        const GLint partELoc[4] = {g_uni.partE0, g_uni.partE1, g_uni.partE2, g_uni.partE3};
        const float rk          = static_cast<float>(as * scale);   // radius-like values follow the anim/scale transform
        for (int i = 0; i < 4; ++i) {
            float pr[4] = {0, 0, 0, 0};
            float pc[4] = {0, 0, 0, 0};
            float pk[4] = {0, 0, 0, 0};
            if (i < static_cast<int>(el.parts.size())) {
                const auto& part = el.parts[i];
                mapRect(part.x, part.y, part.w, part.h, pr);
                // POSITION-LOCKED THROAT (James's law): the junction arcs can
                // never exceed the body's remaining protrusion through the
                // ring — they bloom over the first k px of emergence and seal
                // exactly as the retreating end passes back through the curve
                // zone. Locked to the SMOOTHED geometry, so it is invariant to
                // animation speed by construction.
                double prot = 1e9;
                if (el.hasCutout) {
                    const double cx2 = el.cutX + el.cutW, cy2 = el.cutY + el.cutH;
                    if (part.y <= el.cutY + 0.5 && part.y + part.h > el.cutY)
                        prot = (part.y + part.h) - el.cutY;          // enters via TOP strip
                    else if (part.y + part.h >= cy2 - 0.5 && part.y < cy2)
                        prot = cy2 - part.y;                          // BOTTOM strip
                    else if (part.x <= el.cutX + 0.5 && part.x + part.w > el.cutX)
                        prot = (part.x + part.w) - el.cutX;           // LEFT strip
                    else if (part.x + part.w >= cx2 - 0.5 && part.x < cx2)
                        prot = cx2 - part.x;                          // RIGHT strip
                    prot = std::max(0.0, prot);
                }
                for (int j = 0; j < 4; ++j) {
                    pc[j] = static_cast<float>(part.c[j]) * rk;
                    pk[j] = static_cast<float>(std::min(static_cast<double>(part.k[j]), prot)) * rk;
                }
            }
            if (partLoc[i] >= 0)
                glUniform4f(partLoc[i], pr[0], pr[1], pr[2], pr[3]);
            if (partCLoc[i] >= 0)
                glUniform4f(partCLoc[i], pc[0], pc[1], pc[2], pc[3]);
            if (partKLoc[i] >= 0)
                glUniform4f(partKLoc[i], pk[0], pk[1], pk[2], pk[3]);
            float pg[4] = {0, 0, 0, 0};
            if (i < static_cast<int>(el.plugs.size()))
                mapRect(el.plugs[i].x, el.plugs[i].y, el.plugs[i].w, el.plugs[i].h, pg);
            if (plugLoc[i] >= 0)
                glUniform4f(plugLoc[i], pg[0], pg[1], pg[2], pg[3]);
            float pe[4] = {0, 0, 0, 0};
            if (i < static_cast<int>(el.parts.size()))
                mapRect(el.parts[i].ex, el.parts[i].ey, el.parts[i].ew, el.parts[i].eh, pe);
            if (partELoc[i] >= 0)
                glUniform4f(partELoc[i], pe[0], pe[1], pe[2], pe[3]);
        }
        // The ring's edge treatment scales to its LOCAL THICKNESS, exactly like
        // every simple element clamps its bands to its short axis (a standalone
        // 40px bar carries ~12px bands; "uniform material" made the same strip
        // carry the full 30–45px stack — the whole strip became edge: a warped
        // fold band at low tint, a wall-darkened tint shade at high tint,
        // hugging the frame's inner outline). Parts + their throat columns keep
        // FULL material through the extents weighting; the junction arcs stay
        // untouched. 0.6 × half-thickness mirrors the simple-element clamp.
        // Band-fit moved INTO the shader (per-side local thickness — see the
        // fxScale block): a 40px bar strip and a 10px side strip each fit
        // their own bands. This stays as a neutral global multiplier hook.
        u1(g_uni.ringFx, 1.0f);
        u1(g_uni.curvePx, static_cast<float>(el.connectorCurve * as * scale));
        u4(g_uni.partAlpha, static_cast<float>(el.partAlphaArr[0]), static_cast<float>(el.partAlphaArr[1]),
           static_cast<float>(el.partAlphaArr[2]), static_cast<float>(el.partAlphaArr[3]));
    }
    u1(g_uni.debugField, static_cast<float>(g_debugField));
    {
    }

    // Quad data was uploaded once at shader link — just bind and draw.
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));

    // Redraw the WHOLE element whenever any of its backdrop changed, so the
    // frost/refraction recompute consistently (no left-behind window edges).
    Render::GL::g_pHyprOpenGL->blend(true);
    bool drew = false;
    boxRegion.forEachRect([&drew](const auto& rect) {
        Render::GL::g_pHyprOpenGL->scissor(&rect, g_pHyprRenderer->m_renderData.transformDamage);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        drew = true;
    });
    Render::GL::g_pHyprOpenGL->scissor(nullptr);
    glBindVertexArray(0);
    if (blur.ok) {
        glActiveTexture(GL_TEXTURE1);
        blur.tex->unbind();
        glActiveTexture(GL_TEXTURE0);
    }
    capture->unbind();
    return drew;
}

// ── Render-pass injection ─────────────────────────────────────────────────
// A capture group's backdrop, filled by its CCapturePass and read by the
// CGlassPasses added alongside it. PER-GROUP, not a shared static: each
// renderFluidGlass call (POST_WALLPAPER / PRE_WINDOW / POST_WINDOWS) makes its
// own holder, so the PRE_WINDOW under-glass (the settings ribbon) reads ITS
// pre-window backdrop and never the POST_WINDOWS capture the bars write. The
// old shared static let the two stages overwrite each other, so the ribbon
// intermittently sampled the FINAL frame — its own window's sidebar content —
// and read as blocky, flickering feedback.
struct CaptureHolder {
    SP<Render::ITexture> tex;
};

class CCapturePass : public IPassElement {
  public:
    CCapturePass(SP<CaptureHolder> holder, std::string stageKey) : m_holder(std::move(holder)), m_stageKey(std::move(stageKey)) {}
    bool needsLiveBlur() override { return false; }
    bool needsPrecomputeBlur() override { return false; }
    const char* passName() override { return "HyprFluidGlassCapture"; }
    ePassElementType type() override { return EK_CUSTOM; }
    std::optional<CBox> boundingBox() override { return std::nullopt; }
    CRegion opaqueRegion() override { return {}; }
    // Must NOT be occlusion-culled: simplify() walks passes in reverse and discards
    // boundingbox-less elements once damage is consumed. The backdrop capture is first
    // in the list (reached last), so without this it gets dropped when opaque windows
    // cover the screen — leaving the holder empty and skipping the glass that frame.
    bool undiscardable() override { return true; }
    std::vector<UP<IPassElement>> draw() override {
        try {
            if (m_holder)
                m_holder->tex = captureBackdropForCurrentMonitor(m_stageKey);
        } catch (const std::exception& error) {
            if (m_holder)
                m_holder->tex.reset();
            recordBoundaryFailure("capture-pass", error.what());
        } catch (...) {
            if (m_holder)
                m_holder->tex.reset();
            recordBoundaryFailure("capture-pass", "non-standard exception");
        }
        return {};
    }
  private:
    SP<CaptureHolder> m_holder;
    std::string       m_stageKey;
};

class CGlassPass : public IPassElement {
  public:
    CGlassPass(GlassElement el, SP<CaptureHolder> holder) : m_el(std::move(el)), m_holder(std::move(holder)) {}
    // Live-blur participation is what makes damage work without a self-damage
    // loop: the pass system adds this element's bounding box to its blur region,
    // and when any damage touches it, expands THIS frame's damage (render +
    // presentation) to cover it — the whole glass re-renders coherently the same
    // frame something under it changed, and costs nothing when nothing did.
    bool needsLiveBlur() override { return true; }
    bool needsPrecomputeBlur() override { return false; }
    const char* passName() override { return "HyprFluidGlassDraw"; }
    ePassElementType type() override { return EK_CUSTOM; }
    std::optional<CBox> boundingBox() override {
        if (!g_pHyprRenderer || !g_pHyprRenderer->m_renderData.pMonitor) return std::nullopt;
        // Inflated by the sampling apron: damage NEAR the glass changes what the
        // frost/refraction show, so it must trigger a redraw too.
        return CBox(m_el.x - GLASS_APRON_LOGICAL, m_el.y - GLASS_APRON_LOGICAL,
                    m_el.w + 2.0 * GLASS_APRON_LOGICAL, m_el.h + 2.0 * GLASS_APRON_LOGICAL)
            .round();
    }
    CRegion opaqueRegion() override { return {}; }
    std::vector<UP<IPassElement>> draw() override {
        try {
            if (!drawElement(m_el, m_holder ? m_holder->tex : nullptr))
                return {};

            const auto monitor = g_pHyprRenderer ? g_pHyprRenderer->renderData().pMonitor.lock() : nullptr;
            if (!monitor)
                return {};

            std::lock_guard guard(g_stateMutex);
            const auto      it = g_elements.find(m_el.id);
            if (it == g_elements.end() || it->second.rev != m_el.rev)
                return {};

            auto& live = it->second;
            if (!live.wasDrawn)
                dbgLog(m_el.id, "draw.start", m_el.lastDrawCause + " on " + monitor->m_name);
            if (live.drawnRev != live.rev || live.drawnEpoch != g_drawEpoch)
                g_glassDirty = true;
            live.wasDrawn      = true;
            live.drawnRev      = live.rev;
            live.drawnEpoch    = g_drawEpoch;
            live.lastGX        = monitor->m_position.x + m_el.x;
            live.lastGY        = monitor->m_position.y + m_el.y;
            live.lastGW        = m_el.w;
            live.lastGH        = m_el.h;
            live.lastDrawCause = m_el.lastDrawCause;
            ++live.drawCount;
        } catch (const std::exception& error) {
            recordBoundaryFailure("glass-pass", error.what());
        } catch (...) {
            recordBoundaryFailure("glass-pass", "non-standard exception");
        }
        return {};
    }
  private:
    GlassElement      m_el;
    SP<CaptureHolder> m_holder;
};

// Resolve a window-bound element against the monitor CURRENTLY BEING RENDERED.
// Returns false only when no live window matches (real bind loss). A straddling
// window renders on every monitor it overlaps and its glass must too, so the
// element is placed in the RENDER monitor's space — not the window's "owning"
// monitor, which flips at the halfway point and left the other half glassless.
// onThisMonitor comes back false when the window is alive but entirely on other
// outputs (caller keeps the bind fresh and draws nothing here).
bool resolveWindowBind(GlassElement& el, const PHLMONITOR& renderMonitor, bool& onThisMonitor) {
    onThisMonitor = false;
    if (!g_pCompositor || el.bindSelector.empty() || !renderMonitor) return false;
    // Hyprland 0.55 removed CCompositor::getWindowByRegex — resolve the selector ourselves against the
    // live window list. Honour Hyprland-style "title:<re>" / "class:<re>" prefixes (bare = match either
    // class or title); first mapped hit wins. Bad regex → no match (skip element).
    std::string sel   = el.bindSelector;
    int         field = 0; // 0 = class OR title, 1 = title only, 2 = class only, 3 = exact address
    if (sel.rfind("title:", 0) == 0) {
        field = 1;
        sel   = sel.substr(6);
    } else if (sel.rfind("class:", 0) == 0) {
        field = 2;
        sel   = sel.substr(6);
    } else if (sel.rfind("address:", 0) == 0) {
        // Exact window identity — the app-glass registry binds each toplevel
        // by its compositor address (hyprctl format, "0x" optional).
        field = 3;
        sel   = sel.substr(8);
        if (sel.rfind("0x", 0) == 0)
            sel = sel.substr(2);
    }
    if (sel.empty() || sel.size() > 256)
        return false;
    if (field == 3 && !std::ranges::all_of(sel, [](const unsigned char c) {
            return std::isxdigit(c);
        }))
        return false;
    // Compile the selector once per unique string (std::regex construction is slow, and anchored
    // elements resolve every frame). A nullptr entry marks a pattern that failed to compile. This runs
    // only on the render thread (under g_stateMutex in renderFluidGlass), so the static is safe.
    PHLWINDOW win;
    if (field == 3) {
        // Address match: exact pointer identity, no regex machinery.
        for (const auto& w : g_pCompositor->m_windows) {
            if (!w || !w->m_isMapped)
                continue;
            if (std::format("{:x}", reinterpret_cast<uintptr_t>(w.get())) == sel) {
                win = w;
                el.boundWindow = w;
                break;
            }
        }
    } else {
        static std::map<std::string, std::shared_ptr<std::regex>> s_reCache;
        std::shared_ptr<std::regex> rePtr;
        if (auto it = s_reCache.find(sel); it != s_reCache.end()) {
            rePtr = it->second;
        } else {
            try { rePtr = std::make_shared<std::regex>(sel); }
            catch (...) { rePtr = nullptr; }
            if (s_reCache.size() < 256)
                s_reCache[sel] = rePtr;
        }
        if (!rePtr) return false;

        try {
            const std::regex& re = *rePtr;
            for (const auto& w : g_pCompositor->m_windows) {
                if (!w || !w->m_isMapped)
                    continue;
                const bool hit = (field == 1) ? std::regex_search(w->m_title, re)
                    : (field == 2)            ? std::regex_search(w->m_class, re)
                                              : (std::regex_search(w->m_class, re) || std::regex_search(w->m_title, re));
                if (hit) {
                    win = w;
                    el.boundWindow = w;
                    break;
                }
            }
        } catch (const std::exception&) {
            return false;
        }
    }
    if (!win || !win->m_isMapped) return false;
    const Vector2D wpos  = win->m_realPosition->value();   // global logical top-left
    const Vector2D wsize = win->m_realSize->value();
    // Auto-geometry: a window-bound element sent WITHOUT a size takes the live
    // window's size and actual Hyprland corner rounding each frame — foreign
    // app windows (the app-glass registry) never need shell-side geometry.
    if (el.w <= 0.5 || el.h <= 0.5) {
        el.w      = wsize.x;
        el.h      = wsize.y;
        el.radius = win->rounding();
    }
    const double   mx = renderMonitor->m_position.x, my = renderMonitor->m_position.y;
    const double   mw = renderMonitor->m_size.x,     mh = renderMonitor->m_size.y;
    if (wpos.x + wsize.x <= mx || wpos.x >= mx + mw || wpos.y + wsize.y <= my || wpos.y >= my + mh)
        return true;   // bind alive; window entirely on other outputs
    el.monitor = renderMonitor->m_name;
    el.x = (wpos.x - mx) + el.relX;
    el.y = (wpos.y - my) + el.relY;
    onThisMonitor = true;
    return true;
}

// Resolve a layer-bound element against the monitor being rendered: find a mapped,
// non-fading layer-shell surface with the element's namespace, take its live position
// (global logical, animated — so glass tracks reveal/auto-hide slides) plus the
// element-relative offset, and inherit its fade alpha. The alpha is how glass leaves
// with the surface: Hyprland fades non-aboveFullscreen top layers to 0 when a window
// goes fullscreen, and the glass fades in lockstep instead of floating over the video.
//
// Namespaces are NOT unique per window (a monitor with two bars has two "hgs:bar"
// surfaces), so among matches the one whose position best fits the element's
// expected window origin wins. The expectation comes from the payload's fallback
// rect: monitor-local x/y minus the window-relative offset = where the client
// last believed the window origin to be.
bool resolveLayerBind(GlassElement& el, const PHLMONITOR& monitor, double& alphaOut, int& layerOut) {
    if (el.bindSelector.empty()) return false;
    const double expX  = el.x - el.relX;
    const double expY  = el.y - el.relY;
    bool         found = false;
    double       bestD = 0, bestX = 0, bestY = 0, bestA = 1.0, bestW = -1, bestH = -1;
    int          bestL = 2;
    for (auto const& level : monitor->m_layerSurfaceLayers) {
        for (auto const& lsr : level) {
            const auto ls = lsr.lock();
            // A dying layer unmaps immediately (its fade-out is a detached snapshot
            // object on this Hyprland), so the mapped check alone covers teardown.
            if (!ls || !ls->m_mapped) continue;
            if (ls->m_namespace != el.bindSelector) continue;
            const Vector2D lp = ls->m_realPosition->value();   // global logical top-left
            const double   lx = lp.x - monitor->m_position.x;
            const double   ly = lp.y - monitor->m_position.y;
            const double   d  = (lx - expX) * (lx - expX) + (ly - expY) * (ly - expY);
            if (!found || d < bestD) {
                found = true;
                bestD = d;
                bestX = lx;
                bestY = ly;
                bestA = clampd(ls->m_alpha->value(), 0.0, 1.0);
                bestL = static_cast<int>(ls->m_layer);
                bestW = ls->m_geometry.w;   // P4: observed surface size (logical)
                bestH = ls->m_geometry.h;
            }
        }
    }
    if (!found) return false;
    el.x     = bestX + el.relX;
    el.y     = bestY + el.relY;
    alphaOut = bestA;
    layerOut = bestL;
    // P4 coordinate agreement: distance of the bound surface's top-left from where
    // the descriptor expected it (logical px), plus the observed surface size.
    el.coordDist = std::sqrt(bestD);
    el.obsW      = bestW;
    el.obsH      = bestH;
    return true;
}

// Window-under elements draw once per frame at RENDER_PRE_WINDOW of their own
// window (capture = wallpaper + every lower window). Keyed monitor/id; cleared
// each frame at POST_WALLPAPER, which precedes the window loop.
static std::unordered_set<std::string> g_preWindowDrawn;

void pruneRemovedOutputState() {
    std::set<std::string> activeOutputs;
    if (g_pCompositor) {
        for (const auto& monitor : g_pCompositor->m_monitors)
            if (monitor)
                activeOutputs.insert(monitor->m_name);
    }

    const auto outputFromCompositeKey = [](const std::string& key) {
        return key.substr(0, key.find('/'));
    };
    std::erase_if(g_captureFBs, [&](const auto& entry) {
        return !activeOutputs.contains(outputFromCompositeKey(entry.first));
    });
    std::erase_if(g_captureKnownSize, [&](const auto& entry) {
        return !activeOutputs.contains(outputFromCompositeKey(entry.first));
    });
    std::erase_if(g_selfDamage, [&](const auto& entry) {
        return !activeOutputs.contains(entry.first);
    });
    std::erase_if(g_preWindowDrawn, [&](const auto& key) {
        return !activeOutputs.contains(outputFromCompositeKey(key));
    });
}

void renderFluidGlassImpl(eRenderStage stage) {
    if (stage != RENDER_POST_WINDOWS && stage != RENDER_POST_WALLPAPER && stage != RENDER_PRE_WINDOW) return;
    if (!g_pHyprRenderer) return;
    // Two draw stages: elements bound to BACKGROUND/BOTTOM layer surfaces (e.g.
    // desktop widgets) draw post-wallpaper — before windows — so their glass can
    // never float above application windows, and their capture is the wallpaper,
    // which is exactly what sits behind them. Everything else draws post-windows.
    // Bookkeeping that must run once per frame (exit/purge erasure, cursor
    // memory, self-damage aging) lives in the post-windows pass only.
    const bool isWindowsStage = stage == RENDER_POST_WINDOWS;
    const bool isPreWindow    = stage == RENDER_PRE_WINDOW;

    std::vector<GlassElement> here;
    std::vector<CBox>          finishedBoxes;   // areas to clear (exit done / surface lost)
    std::vector<CBox>          drawnGlobal;     // ring-damage for drawn elements (global logical)
    std::vector<CBox>          drawnScaled;     // same boxes in scaled monitor-local px
    {
        std::lock_guard guard(g_stateMutex);
        if (!g_enabled || g_elements.empty()) return;
        const auto monitor = g_pHyprRenderer->renderData().pMonitor.lock();
        if (!monitor) return;

        if (isWindowsStage)
            pruneRemovedOutputState();

        if (stage == RENDER_POST_WALLPAPER) {
            // New frame for this monitor: reset the pre-window draw guard.
            const std::string pfx = monitor->m_name + "/";
            for (auto it = g_preWindowDrawn.begin(); it != g_preWindowDrawn.end();)
                it = (it->rfind(pfx, 0) == 0) ? g_preWindowDrawn.erase(it) : std::next(it);
        }

        // This frame's damage (before the pass system's blur expansion), in
        // display-oriented buffer px — the same space simplify() tests against.
        const CRegion frameDamage{g_pHyprRenderer->m_renderData.damage};

        auto&         selfDamage = g_selfDamage[monitor->m_name];
        const CRegion priorSelf  = selfDamage.region;

        const auto   now    = std::chrono::steady_clock::now();
        const double durSec = std::max(1.0, g_animMs) / 1000.0;
        auto elapsed = [&](std::chrono::steady_clock::time_point t) {
            return std::chrono::duration<double>(now - t).count();
        };
        // smootherstep — gentle at both ends, reads like settling glass.
        auto ease = [](double t) {
            t = clampd(t, 0.0, 1.0);
            return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
        };

        std::vector<std::string> finished;
        for (auto& [id, el0] : g_elements) {
            double shownT;
            if (el0.exiting) {
                const double t = elapsed(el0.exitStart) / durSec;
                if (t >= 1.0) {
                    if (isWindowsStage) {
                        finished.push_back(id);
                        finishedBoxes.push_back(CBox(monitor->m_position.x + el0.x, monitor->m_position.y + el0.y, el0.w, el0.h));
                        dbgLog(id, "exit.finish", "anim done");
                    }
                    continue;
                }
                shownT = ease(1.0 - t);
            } else {
                shownT = ease(elapsed(el0.birth) / durSec);
            }

            // Parametric element transition: geometry IS the reveal — pin the
            // enter/exit ramp (no scale pop, no fade; law 3) and self-terminate
            // completed closes (law 4: the element is GONE — no exit fade, no
            // waiting on the shell to notice).
            bool elTrActive = false;
            if (el0.elTrKind != 0 && !el0.elTrDone) {
                const double trElapsed = std::chrono::duration<double, std::milli>(now - el0.elTrStart).count();
                const double trTau = el0.elTrDurMs > 1.0 ? clampd(trElapsed / el0.elTrDurMs, 0.0, 1.0) : 1.0;
                if (trTau >= 1.0) {
                    if (el0.elTrKind == 2) {
                        if (isWindowsStage) {
                            finished.push_back(id);
                            if (el0.wasDrawn)
                                finishedBoxes.push_back(CBox(el0.lastGX, el0.lastGY, el0.lastGW, el0.lastGH));
                            dbgLog(id, "el.tr-done", "close complete, element erased");
                        }
                        continue;
                    }
                    el0.elTrDone = true;
                } else
                    elTrActive = true;
            }
            if (el0.elTrKind != 0 && !el0.exiting)
                shownT = 1.0;   // geometry is the reveal — no birth scale-pop/fade (law 3); exiting keeps its dissolve (safety net)

            // Advance geometry smoothing toward the payload targets.
            double frameDt = 1.0 / 60.0;
            {
                double dt = 0.0;
                if (el0.smoothT.time_since_epoch().count() != 0)
                    dt = clampd(std::chrono::duration<double>(now - el0.smoothT).count(), 0.0, 0.05);
                el0.smoothT = now;
                if (dt > 0.0) frameDt = dt;
                if (el0.sw < 0.0) {   // first sight: snap to targets
                    el0.sx = el0.x; el0.sy = el0.y; el0.sw = el0.w; el0.sh = el0.h;
                    el0.sr = el0.radius; el0.srx = el0.relX; el0.sry = el0.relY;
                    el0.sParts = el0.parts;
                    el0.sPartAlpha.assign(el0.parts.size(), 1.0);
                } else {
                    const double k = 1.0 - std::exp(-dt / 0.035);
                    auto toward = [&](double& s, double target) {
                        s += (target - s) * k;
                        if (std::abs(target - s) < 0.25) s = target;
                    };
                    toward(el0.sx, el0.x); toward(el0.sy, el0.y);
                    toward(el0.sw, el0.w); toward(el0.sh, el0.h);
                    toward(el0.sr, el0.radius);
                    toward(el0.srx, el0.relX); toward(el0.sry, el0.relY);
                    // Parts glide with the same settle. Count change = topology
                    // change (reveal start/end) → snap to the new set; the shell
                    // already sends the collapsed first rect, so the growth
                    // itself is what glides.
                    if (el0.sParts.size() != el0.parts.size()) {
                        // Topology change: removed parts become DYING (fade out
                        // at their last geometry); new parts seed at alpha 0 and
                        // fade in. Geometry itself still snaps (positions honest).
                        if (el0.parts.size() < el0.sParts.size()) {
                            for (size_t pi = el0.parts.size(); pi < el0.sParts.size(); ++pi) {
                                el0.dyingParts.push_back(el0.sParts[pi]);
                                el0.dyingAlpha.push_back(pi < el0.sPartAlpha.size() ? el0.sPartAlpha[pi] : 1.0);
                            }
                        }
                        std::vector<double> na(el0.parts.size(), 0.0);
                        for (size_t pi = 0; pi < el0.parts.size() && pi < el0.sPartAlpha.size(); ++pi)
                            na[pi] = el0.sPartAlpha[pi];
                        el0.sPartAlpha = std::move(na);
                        el0.sParts     = el0.parts;
                    } else {
                        for (size_t pi = 0; pi < el0.parts.size(); ++pi) {
                            auto&       sp = el0.sParts[pi];
                            const auto& tp = el0.parts[pi];
                            toward(sp.x, tp.x); toward(sp.y, tp.y);
                            toward(sp.w, tp.w); toward(sp.h, tp.h);
                            toward(sp.ex, tp.ex); toward(sp.ey, tp.ey);
                            toward(sp.ew, tp.ew); toward(sp.eh, tp.eh);
                            for (int ci = 0; ci < 4; ++ci) {
                                sp.c[ci] = tp.c[ci];
                                sp.k[ci] = tp.k[ci];
                            }
                        }
                    }
                }
            }
            // ---- PARAMETRIC TRANSITIONS: the plugin IS the animator ----
            // Each transitioning part carries resting geometry + curve; we
            // evaluate the same bezier the chrome runs and derive the current
            // slot. Close reaching tau>=1 removes the part HERE (self-
            // terminating lifecycle — zombies unrepresentable).
            bool partAnimActive = false;
            for (size_t pi = 0; pi < el0.parts.size();) {
                auto& pp = el0.parts[pi];
                if (pp.trKind == 0 || pp.trDone) { ++pi; continue; }
                const double elapsed = std::chrono::duration<double, std::milli>(now - pp.trStart).count();
                const double tau     = pp.trDurMs > 1.0 ? clampd(elapsed / pp.trDurMs, 0.0, 1.0) : 1.0;
                if (pp.trKind == 2 && tau >= 1.0) {
                    // close complete: the part removes itself
                    el0.parts.erase(el0.parts.begin() + pi);
                    if (pi < el0.sParts.size()) el0.sParts.erase(el0.sParts.begin() + pi);
                    if (pi < el0.sPartAlpha.size()) el0.sPartAlpha.erase(el0.sPartAlpha.begin() + pi);
                    dbgLog(el0.id, "part.tr-done", "close self-terminated");
                    continue;
                }
                if (pp.trKind == 1 && tau >= 1.0) {
                    pp.trDone = true;   // open settled: steady state at resting geometry
                    ++pi;
                    continue;
                }
                partAnimActive = true;
                ++pi;
            }

            // Advance the part fades (~90ms settle) and decay the dying list.
            {
                const double ak = 1.0 - std::exp(-frameDt / 0.06);
                if (el0.sPartAlpha.size() != el0.sParts.size())
                    el0.sPartAlpha.resize(el0.sParts.size(), 0.0);
                for (size_t ai = 0; ai < el0.sPartAlpha.size(); ++ai) {
                    // Chase the PAYLOAD alpha: 1 while presenting, 0 the moment
                    // the surface reports "closing" — the glass dims WITH the
                    // content instead of surviving as a naked slab.
                    const double tgt = ai < el0.parts.size() ? el0.parts[ai].a : 1.0;
                    double&      a2  = el0.sPartAlpha[ai];
                    a2 += (tgt - a2) * ak;
                    if (std::abs(tgt - a2) < 0.005) a2 = tgt;
                }
                for (size_t di = 0; di < el0.dyingParts.size();) {
                    el0.dyingAlpha[di] += (0.0 - el0.dyingAlpha[di]) * ak;
                    if (el0.dyingAlpha[di] < 0.03) {
                        el0.dyingParts.erase(el0.dyingParts.begin() + di);
                        el0.dyingAlpha.erase(el0.dyingAlpha.begin() + di);
                    } else {
                        ++di;
                    }
                }
            }
            double partsDelta = 0.0;
            if (el0.sParts.size() == el0.parts.size()) {
                for (size_t pi = 0; pi < el0.parts.size(); ++pi)
                    partsDelta += std::abs(el0.sParts[pi].x - el0.parts[pi].x) + std::abs(el0.sParts[pi].y - el0.parts[pi].y) +
                        std::abs(el0.sParts[pi].w - el0.parts[pi].w) + std::abs(el0.sParts[pi].h - el0.parts[pi].h);
            }
            bool alphaSettling = !el0.dyingParts.empty() || partAnimActive || elTrActive;
            for (size_t ai = 0; ai < el0.sPartAlpha.size(); ++ai) {
                const double tgt = ai < el0.parts.size() ? el0.parts[ai].a : 1.0;
                if (std::abs(el0.sPartAlpha[ai] - tgt) > 0.001) alphaSettling = true;
            }
            const bool smoothingActive = alphaSettling ||
                                         std::abs(el0.sx - el0.x) + std::abs(el0.sy - el0.y) +
                                         std::abs(el0.sw - el0.w) + std::abs(el0.sh - el0.h) +
                                         std::abs(el0.sr - el0.radius) +
                                         std::abs(el0.srx - el0.relX) + std::abs(el0.sry - el0.relY) + partsDelta > 0.5;

            GlassElement el = el0;
            el.x = el0.sx; el.y = el0.sy;
            el.w = el0.sw; el.h = el0.sh;
            el.radius = el0.sr;
            el.relX = el0.srx; el.relY = el0.sry;
            el.parts = el0.sParts;   // render the GLIDING part rects
            // Parametric parts render at their DERIVED position (resting rect
            // shrunk along the bar side by travel*(1-progress), clamped by
            // protrusion) — bypassing smoothing entirely: the curve IS the truth.
            for (size_t pi = 0; pi < el0.parts.size() && pi < el.parts.size(); ++pi) {
                const auto& pp = el0.parts[pi];
                if (pp.trKind == 0 || pp.trDone) continue;
                const double elapsed = std::chrono::duration<double, std::milli>(now - pp.trStart).count();
                const double tau     = pp.trDurMs > 1.0 ? clampd(elapsed / pp.trDurMs, 0.0, 1.0) : 1.0;
                const double eased   = evalBezierSpline(pp.trBezier, tau);
                const double prog    = pp.trKind == 1 ? eased : 1.0 - eased;
                const double shrink  = std::min(pp.trProtrusion, std::max(0.0, pp.trTravel * (1.0 - prog)));
                GlassElement::PartRect cur = pp;
                switch (pp.trSide) {
                    case 0: cur.h = std::max(0.0, pp.h - shrink); cur.eh = std::max(0.0, pp.eh - shrink); break;               // top
                    case 1: cur.y = pp.y + shrink; cur.h = std::max(0.0, pp.h - shrink); cur.ey = pp.ey + shrink; cur.eh = std::max(0.0, pp.eh - shrink); break; // bottom
                    case 2: cur.w = std::max(0.0, pp.w - shrink); cur.ew = std::max(0.0, pp.ew - shrink); break;               // left
                    default: cur.x = pp.x + shrink; cur.w = std::max(0.0, pp.w - shrink); cur.ex = pp.ex + shrink; cur.ew = std::max(0.0, pp.ew - shrink); break; // right
                }
                el.parts[pi] = cur;
                if (pi < el0.sParts.size()) el0.sParts[pi] = cur;   // keep smoothing state coherent
                el.partAlphaArr[pi] = 1.0;                          // material rides fully (law 3)
                if (pi < el0.sPartAlpha.size()) el0.sPartAlpha[pi] = 1.0;   // seed the smoothed alpha too — the copy block below would otherwise re-fade a newborn part
            }
            {
                for (int i2 = 0; i2 < 4; i2++) el.partAlphaArr[i2] = 1.0;
                for (size_t pi = 0; pi < el.parts.size() && pi < 4; ++pi)
                    el.partAlphaArr[pi] = pi < el0.sPartAlpha.size() ? el0.sPartAlpha[pi] : 1.0;
                for (size_t di = 0; di < el0.dyingParts.size() && el.parts.size() < 4; ++di) {
                    el.partAlphaArr[el.parts.size()] = el0.dyingAlpha[di];
                    el.parts.push_back(el0.dyingParts[di]);
                }
            }
            double bindAlpha = 1.0;
            int bindLayer = 2;   // top-equivalent unless a layer bind says otherwise
            if (el.bindType == "window") {
                bool onThisMonitor = false;
                if (!resolveWindowBind(el, monitor, onThisMonitor)) {
                    // No live window behind it — nothing to glass; clear leftovers once.
                    if (el0.bound) {
                        dbgLog(id, "bind.loss", "window:" + el.bindSelector);
                        g_glassDirty = true;   // P4: readiness dropped
                    }
                    el0.bound = false;
                    el0.drawnRev = 0;          // P4: draw-confirm resets on unbind
                    el0.coordDist = -1; el0.obsW = -1; el0.obsH = -1;
                    clearUnderNoBlur(id);
                    if (el0.wasDrawn) {
                        finishedBoxes.push_back(CBox(el0.lastGX, el0.lastGY, el0.lastGW, el0.lastGH));
                        el0.wasDrawn = false;
                    }
                    if (el0.exiting && isWindowsStage) {
                        finished.push_back(id);   // its exit can never render again — done
                        dbgLog(id, "exit.finish", "surface gone");
                    }
                    continue;
                }
                // The engine claims the bound window while under-glass rides
                // beneath it: compositor blur OFF (see g_noBlurApplied).
                if (el.under)
                    setUnderNoBlur(id, el.boundWindow.lock());
                else
                    clearUnderNoBlur(id);
                if (!onThisMonitor) {
                    // Window alive but not on this output: keep the bind fresh
                    // (no purge, no bound-state flap) and draw nothing here.
                    el0.bound      = true;
                    el0.lastBindOk = now;
                    continue;
                }
            } else if (el.bindType == "layer") {
                if (el.monitor != monitor->m_name)
                    continue;                 // layer binds are monitor-scoped
                if (!resolveLayerBind(el, monitor, bindAlpha, bindLayer)) {
                    // Surface unmapped/gone — glass goes with it, instantly.
                    if (el0.bound) {
                        dbgLog(id, "bind.loss", "layer:" + el.bindSelector);
                        g_glassDirty = true;   // P4: readiness dropped
                    }
                    el0.bound = false;
                    el0.drawnRev = 0;          // P4: draw-confirm resets on unbind
                    el0.coordDist = -1; el0.obsW = -1; el0.obsH = -1;
                    if (el0.wasDrawn) {
                        finishedBoxes.push_back(CBox(el0.lastGX, el0.lastGY, el0.lastGW, el0.lastGH));
                        el0.wasDrawn = false;
                    }
                    if (el0.exiting && isWindowsStage) {
                        finished.push_back(id);   // its exit can never render again — done
                        dbgLog(id, "exit.finish", "surface gone");
                    }
                    continue;
                }
            }
            if (!el.bindType.empty()) {
                if (!el0.bound) {
                    dbgLog(id, "bind.gain", el.bindType + ":" + el.bindSelector + " -> " +
                                                std::to_string(static_cast<int>(el.x)) + "," + std::to_string(static_cast<int>(el.y)) + " on " + monitor->m_name);
                    g_glassDirty = true;   // P4: readiness gained
                }
                el0.bound      = true;
                el0.lastBindOk = now;
                // P4: window binds follow the window (position authoritative) —
                // coordinate is aligned by construction; layer binds already have
                // coordDist/obs set by resolveLayerBind.
                if (el.bindType == "window") {
                    el0.coordDist = 0;
                    el0.obsW = el.w;
                    el0.obsH = el.h;
                } else {
                    el0.coordDist = el.coordDist;   // resolveLayerBind set these on `el`
                    el0.obsW = el.obsW;
                    el0.obsH = el.obsH;
                }
                if (!el0.everBound) {
                    // The surface just appeared for the first time — start the enter
                    // animation here, not at apply time (the descriptor may arrive
                    // before the surface maps).
                    el0.everBound = true;
                    if (!el0.exiting) {
                        el0.birth = now;
                        shownT    = 0.0;
                    }
                }
            }
            // Parametric element derive: slide the WHOLE element along its edge
            // by travel*(1-progress) — the exact offset the shell's content
            // Translate computes, evaluated from the same bezier clock. Applied
            // after bind resolution so it rides on the live surface position.
            if (el0.elTrKind != 0 && !el0.elTrDone && el0.elTrTravel > 0.0) {
                const double trElapsed = std::chrono::duration<double, std::milli>(now - el0.elTrStart).count();
                const double trTau   = el0.elTrDurMs > 1.0 ? clampd(trElapsed / el0.elTrDurMs, 0.0, 1.0) : 1.0;
                const double trEased = evalBezierSpline(el0.elTrBezier, trTau);
                const double trProg  = el0.elTrKind == 1 ? trEased : 1.0 - trEased;
                const double trOff   = el0.elTrTravel * (1.0 - trProg);
                switch (el0.elTrSide) {
                    case 0: el.y -= trOff; break;   // top bar retracts upward
                    case 1: el.y += trOff; break;   // bottom bar retracts downward
                    case 2: el.x -= trOff; break;   // left bar retracts left
                    default: el.x += trOff; break;  // right bar retracts right
                }
            }
            // Route to the matching draw pass: background/bottom-bound elements
            // belong post-wallpaper; window-bound "under" elements draw at
            // RENDER_PRE_WINDOW of their own window (so lower windows are in
            // the capture); everything else post-windows.
            const bool windowUnder    = el.under && el.bindType == "window";
            const bool wallpaperStage = (el.bindType == "layer" && bindLayer <= 1) || (el.under && !windowUnder);
            if (isPreWindow) {
                if (!windowUnder)
                    continue;
                const auto cur = g_pHyprRenderer->m_renderData.currentWindow.lock();
                if (!cur || cur != el.boundWindow.lock())
                    continue;
                if (!g_preWindowDrawn.insert(monitor->m_name + "/" + el.id).second)
                    continue; // second pass of the same window this frame
            } else if (windowUnder) {
                continue;
            } else if (wallpaperStage == isWindowsStage) {
                continue;
            }
            if (el.monitor == monitor->m_name && el.w > 0 && el.h > 0) {
                el.animScale   = 0.9 + 0.1 * shownT;
                el.renderAlpha = shownT * bindAlpha;

                // Redraw only when something can actually change the pixels:
                // an animation frame, the tracked light moving, or damage in the
                // element's sampling reach. Otherwise the previous frame's glass
                // is still exact — skip everything (this is what lets the
                // compositor idle with glass on screen).
                const bool   animActive  = el0.exiting || shownT < 1.0 || elTrActive;
                const double s           = monitor->m_scale;

                // Damage tests and presentation extension run on the element's
                // OCCUPIED sub-rects: for a composite ring that is the four strips
                // plus attached parts — content changing inside the cutout must
                // not redraw (and re-blur) a monitor-sized glass element.
                std::vector<CBox> subLogical;   // element-local logical
                if (el.hasCutout) {
                    auto addSub = [&](double x, double y, double w, double h) {
                        if (w > 0.5 && h > 0.5)
                            subLogical.push_back(CBox(x, y, w, h));
                    };
                    addSub(0, 0, el.w, el.cutY);
                    addSub(0, el.cutY + el.cutH, el.w, el.h - el.cutY - el.cutH);
                    addSub(0, el.cutY, el.cutX, el.cutH);
                    addSub(el.cutX + el.cutW, el.cutY, el.w - el.cutX - el.cutW, el.cutH);
                    for (const auto& part : el.parts)
                        addSub(part.x - 24.0, part.y - 24.0, part.w + 48.0, part.h + 48.0);
                } else {
                    subLogical.push_back(CBox(0, 0, el.w, el.h));
                }

                // Two separate questions, and conflating them caused a visible strobe:
                //  boxTouched — is the element's own area being repainted this frame
                //    (by ANYTHING, including our own ring echoes)? If yes we MUST add
                //    our pass, or the repaint paints the backdrop over the glass and
                //    erases it. Skipping is only safe when the area isn't touched.
                //  external — is there fresh damage (not our echoes) in the sampling
                //    reach? Only that justifies propagating NEW damage; echo-driven
                //    draws must stay damage-neutral so the loop can settle.
                bool boxTouched = false;
                bool external   = false;
                {
                    CRegion fresh = frameDamage.copy();
                    if (!priorSelf.empty())
                        fresh.subtract(priorSelf);
                    for (const auto& sub : subLogical) {
                        if (!boxTouched) {
                            CRegion touched = frameDamage.copy();
                            touched.intersect(CBox((el.x + sub.x) * s - 2.0, (el.y + sub.y) * s - 2.0, sub.width * s + 4.0, sub.height * s + 4.0));
                            boxTouched = !touched.empty();
                        }
                        if (!external) {
                            CRegion f2 = fresh.copy();
                            f2.intersect(CBox((el.x + sub.x - GLASS_APRON_LOGICAL) * s, (el.y + sub.y - GLASS_APRON_LOGICAL) * s,
                                              (sub.width + 2.0 * GLASS_APRON_LOGICAL) * s, (sub.height + 2.0 * GLASS_APRON_LOGICAL) * s));
                            external = !f2.empty();
                        }
                        if (boxTouched && external)
                            break;
                    }
                }

                const bool need      = animActive || smoothingActive || boxTouched || external;
                const bool propagate = animActive || smoothingActive || external;
                const bool drawNow   = need && el.renderAlpha > 0.003;
                if (drawNow) {
                    const char* cause = animActive ? "anim" : smoothingActive ? "smooth" : external ? "external" : "echo";
                    // Geometry in motion: also clear the PREVIOUS footprint, or a
                    // shrinking/moving glass leaves a trail of its old pixels.
                    if (propagate && el0.wasDrawn) {
                        drawnGlobal.push_back(CBox(el0.lastGX, el0.lastGY, el0.lastGW, el0.lastGH));
                        drawnScaled.push_back(CBox((el0.lastGX - monitor->m_position.x) * s - 2.0,
                                                   (el0.lastGY - monitor->m_position.y) * s - 2.0,
                                                   el0.lastGW * s + 4.0, el0.lastGH * s + 4.0));
                    }
                    el.lastDrawCause = cause;
                    here.push_back(el);
                    if (propagate) {
                        const double currentGX = monitor->m_position.x + el.x;
                        const double currentGY = monitor->m_position.y + el.y;
                        for (const auto& sub : subLogical) {
                            drawnGlobal.push_back(CBox(currentGX + sub.x, currentGY + sub.y, sub.width, sub.height));
                            drawnScaled.push_back(CBox((el.x + sub.x) * s - 2.0, (el.y + sub.y) * s - 2.0, sub.width * s + 4.0, sub.height * s + 4.0));
                        }
                    }
                } else if (el0.wasDrawn && el.renderAlpha <= 0.003) {
                    // Fully faded (e.g. fullscreen hid the layer) — leftover pixels
                    // clear with the surface's own fade damage; this is the backstop.
                    finishedBoxes.push_back(CBox(el0.lastGX, el0.lastGY, el0.lastGW, el0.lastGH));
                    el0.wasDrawn = false;
                    dbgLog(id, "draw.fade-out", "alpha 0");
                }
            }
        }
        // Age out the self-damage record (once per frame); refresh it with
        // whatever either pass draws now.
        if (isWindowsStage && selfDamage.ttl > 0 && --selfDamage.ttl == 0)
            selfDamage.region = CRegion{};
        if (!drawnScaled.empty()) {
            for (const auto& b : drawnScaled)
                selfDamage.region.add(b);
            selfDamage.ttl = 4;   // covers the swapchain's buffer-age echo window
        }
        for (const auto& id : finished) {
            clearUnderNoBlur(id);
            g_elements.erase(id);
            g_elemFBs.erase(id);
        }

        // Bound elements whose surface has been gone for a long time can never render
        // again (dead client, renamed namespace) — drop them so the map stays honest.
        // A live client re-applies its elements anyway.
        if (isWindowsStage) {
            for (auto it = g_elements.begin(); it != g_elements.end();) {
                const auto& el = it->second;
                const bool checkedThisPass = el.bindType == "window" || el.monitor == monitor->m_name;
                if (!el.bindType.empty() && checkedThisPass && std::chrono::duration<double>(now - el.lastBindOk).count() > 60.0) {
                    dbgLog(it->first, "purge", "unbound for >60s");
                    clearUnderNoBlur(it->first);
                    g_elemFBs.erase(it->first);
                    it = g_elements.erase(it);
                } else
                    ++it;
            }
        }
    }
    for (const auto& b : finishedBoxes)
        if (g_pHyprRenderer) g_pHyprRenderer->damageBox(b);
    if (here.empty()) return;

    // A redraw needs three damage acts, each for a different consumer:
    //  1. extend THIS frame's damage over the whole element (the pass system and
    //     the presentation read m_renderData.damage at endRender) — otherwise
    //     only the triggering sliver presents and the rest of the glass keeps
    //     stale pixels (the "trailing window artifacts");
    //  2. ring-damage the box so older swapchain buffers repaint it on their
    //     next turn (and animations get their next frame scheduled);
    //  3. remember it as self-damage so its ring echo can't re-trigger the gate.
    for (const auto& b : drawnGlobal)
        g_pHyprRenderer->damageBox(b);
    for (const auto& b : drawnScaled)
        g_pHyprRenderer->m_renderData.damage.add(b);

    auto captureHolder = makeShared<CaptureHolder>();
    g_pHyprRenderer->m_renderPass.add(makeUnique<CCapturePass>(captureHolder, isPreWindow ? "pre" : (isWindowsStage ? "post" : "wall")));
    for (auto& el : here)
        g_pHyprRenderer->m_renderPass.add(makeUnique<CGlassPass>(el, captureHolder));
    g_lastRenderStatus = "ok";
}

void renderFluidGlass(eRenderStage stage) {
    try {
        renderFluidGlassImpl(stage);
    } catch (const std::exception& error) {
        recordBoundaryFailure("render-stage", error.what());
    } catch (...) {
        recordBoundaryFailure("render-stage", "non-standard exception");
    }
}

void damageAllMonitors() {
    if (!g_pHyprRenderer) return;
    for (const auto& m : g_pCompositor->m_monitors)
        if (m) g_pHyprRenderer->damageMonitor(m);
}

bool samePartRects(const std::vector<GlassElement::PartRect>& a, const std::vector<GlassElement::PartRect>& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].w != b[i].w || a[i].h != b[i].h || a[i].a != b[i].a || a[i].trKind != b[i].trKind)
            return false;
        if (a[i].ex != b[i].ex || a[i].ey != b[i].ey || a[i].ew != b[i].ew || a[i].eh != b[i].eh)
            return false;
        for (int j = 0; j < 4; ++j)
            if (a[i].c[j] != b[i].c[j] || a[i].k[j] != b[i].k[j])
                return false;
    }
    return true;
}

bool samePlugRects(const std::vector<GlassElement::PlugRect>& a, const std::vector<GlassElement::PlugRect>& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].w != b[i].w || a[i].h != b[i].h)
            return false;
    return true;
}

// True when two elements would render identically — used to skip damage (and
// the generation bump) for the periodic re-applies clients send unchanged.
bool sameRenderFields(const GlassElement& a, const GlassElement& b) {
    return a.monitor == b.monitor && a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h &&
        a.radius == b.radius && a.glassLevel == b.glassLevel && a.blurLevel == b.blurLevel &&
        a.tintLevel == b.tintLevel && a.tintEnabled == b.tintEnabled && a.lightMode == b.lightMode &&
        a.tintR == b.tintR && a.tintG == b.tintG && a.tintB == b.tintB &&
        a.refraction == b.refraction && a.rimBand == b.rimBand && a.bevel == b.bevel &&
        a.rimWidth == b.rimWidth && a.highlight == b.highlight && a.shadow == b.shadow &&
        a.lightDeg == b.lightDeg && a.specular == b.specular &&
        a.chroma == b.chroma && a.edgeDepth == b.edgeDepth && a.lens == b.lens &&
        a.lensBand == b.lensBand && a.gloss == b.gloss &&
        a.bindType == b.bindType && a.bindSelector == b.bindSelector &&
        a.relX == b.relX && a.relY == b.relY && a.elTrKind == b.elTrKind &&
        a.hasCutout == b.hasCutout && a.cutX == b.cutX && a.cutY == b.cutY &&
        a.cutW == b.cutW && a.cutH == b.cutH && a.cutR == b.cutR &&
        a.connectorCurve == b.connectorCurve && a.under == b.under &&
        samePartRects(a.parts, b.parts) && samePlugRects(a.plugs, b.plugs);
}

// ── IPC ───────────────────────────────────────────────────────────────────
//  apply payload: {"enabled":bool, "animMs":num?, "elements":[{...}]}
//  element fields (all optional except geometry): id, monitor, x, y, w, h, radius,
//    bind {type:"layer"|"window", selector, relX, relY},
//    glassLevel, blurLevel, tintLevel, tintEnabled, tintColor, refraction, rimBand, bevel,
//    rimWidth, highlight, shadow, lightAngle, specular, chroma, edgeDepth.
//  Legacy: anchorWindow/offsetX/offsetY parse as bind{type:"window"}.
//  See README.md for the full schema + semantics.
static constexpr size_t LEGACY_MAX_REQUEST_BYTES = 256 * 1024;
static constexpr size_t LEGACY_MAX_ELEMENTS      = 512;
static constexpr size_t LEGACY_MAX_ID_BYTES      = 128;
static constexpr size_t LEGACY_MAX_MONITOR_BYTES = 128;
static constexpr size_t LEGACY_MAX_SELECTOR_BYTES = 256;

std::string rejectApply(std::string message) {
    const std::string response = message;
    {
        std::lock_guard guard(g_stateMutex);
        g_lastApplyStatus = "rejected";
        g_lastError       = message;
    }
    dbgLog("", "apply.error", response);
    return "error: " + response + "\n";
}

std::string applyPayload(std::string payload) {
    payload = trim(std::move(payload));
    if (payload.size() > LEGACY_MAX_REQUEST_BYTES)
        return rejectApply("payload exceeds 256 KiB");
    if (payload.empty() || payload.front() != '{') {
        return rejectApply("payload must be a JSON object");
    }
    json doc;
    try { doc = json::parse(payload); }
    catch (const std::exception& e) {
        return rejectApply(std::string("json parse: ") + e.what());
    }
    if (!doc.is_object())
        return rejectApply("payload must be a JSON object");

    std::map<std::string, GlassElement> parsed;
    if (auto it = doc.find("elements"); it != doc.end()) {
        if (!it->is_array())
            return rejectApply("elements must be an array");
        if (it->size() > LEGACY_MAX_ELEMENTS)
            return rejectApply("elements exceeds the 512 item limit");
        for (const auto& e : *it) {
            if (!e.is_object())
                return rejectApply("every element must be an object");
            GlassElement el;
            el.id      = jstr(e, "id");
            el.monitor = jstr(e, "monitor");
            if (el.id.size() > LEGACY_MAX_ID_BYTES)
                return rejectApply("element id exceeds 128 bytes");
            if (el.monitor.size() > LEGACY_MAX_MONITOR_BYTES)
                return rejectApply("monitor name exceeds 128 bytes");
            el.x = jnum(e, "x"); el.y = jnum(e, "y");
            el.w = jnum(e, "w"); el.h = jnum(e, "h");
            el.radius      = jnum(e, "radius");
            el.glassLevel  = jnum(e, "glassLevel", 0.5);
            el.tintEnabled = jbool(e, "tintEnabled", false);
            el.lightMode   = jbool(e, "lightMode", false);
            parseHex(jstr(e, "tintColor"), el.tintR, el.tintG, el.tintB);
            el.refraction  = jnum(e, "refraction", el.refraction);
            el.rimBand     = jnum(e, "rimBand",    el.rimBand);
            el.bevel       = jnum(e, "bevel",      el.bevel);
            el.rimWidth    = jnum(e, "rimWidth",   el.rimWidth);
            el.highlight   = jnum(e, "highlight",  el.highlight);
            el.shadow      = jnum(e, "shadow",     el.shadow);
            el.lightDeg    = jnum(e, "lightAngle", el.lightDeg);
            el.specular    = jnum(e, "specular",   el.specular);
            el.chroma      = jnum(e, "chroma",     el.chroma);
            el.edgeDepth   = jnum(e, "edgeDepth",  el.edgeDepth);
            el.lens        = jnum(e, "lens",       el.lens);
            el.lensBand    = jnum(e, "lensBand",   el.lensBand);
            el.gloss       = jnum(e, "gloss",      el.gloss);
            el.blurLevel    = jnum(e, "blurLevel", -1.0);
            el.tintLevel    = jnum(e, "tintLevel", -1.0);
            if (auto revision = e.find("rev"); revision != e.end()) {
                if (revision->is_number_unsigned()) {
                    el.rev = revision->get<uint64_t>();
                } else if (revision->is_number_integer()) {
                    const auto signedRevision = revision->get<int64_t>();
                    if (signedRevision < 0)
                        return rejectApply("element revision must not be negative");
                    el.rev = static_cast<uint64_t>(signedRevision);
                } else {
                    return rejectApply("element revision must be an integer");
                }
            }
            bool requestedBind = false;
            if (auto b = e.find("bind"); b != e.end()) {
                requestedBind   = true;
                if (!b->is_object())
                    return rejectApply("bind must be an object");
                el.bindType     = jstr(*b, "type");
                el.bindSelector = jstr(*b, "selector");
                el.relX         = jnum(*b, "relX", 0.0);
                el.relY         = jnum(*b, "relY", 0.0);
            } else if (const std::string aw = jstr(e, "anchorWindow"); !aw.empty()) {
                requestedBind   = true;
                // Legacy schema: anchorWindow/offsetX/offsetY is a window bind.
                el.bindType     = "window";
                el.bindSelector = aw;
                el.relX         = jnum(e, "offsetX", 0.0);
                el.relY         = jnum(e, "offsetY", 0.0);
            }
            if (requestedBind && (el.bindType != "layer" && el.bindType != "window"))
                return rejectApply("bind type must be layer or window");
            if (requestedBind && el.bindSelector.empty())
                return rejectApply("bind selector must not be empty");
            if (el.bindSelector.size() > LEGACY_MAX_SELECTOR_BYTES)
                return rejectApply("bind selector exceeds 256 bytes");
            if (auto sd = e.find("side"); sd != e.end() && sd->is_string()) {
                const std::string sv = sd->get<std::string>();
                el.elTrSide = sv == "bottom" ? 1 : sv == "left" ? 2 : sv == "right" ? 3 : 0;
            }
            if (auto tj = e.find("transition"); tj != e.end() && tj->is_object()) {
                const std::string kv = tj->value("kind", "");
                el.elTrKind   = kv == "open" ? 1 : kv == "close" ? 2 : 0;
                el.elTrDurMs  = jnum(*tj, "durationMs", 0.0);
                el.elTrTravel = jnum(*tj, "travelPx", 0.0);
                const double trElapsedMs = jnum(*tj, "elapsedMs", 0.0);
                el.elTrStart = std::chrono::steady_clock::now() - boundedElapsedMilliseconds(trElapsedMs);
                if (auto bz = tj->find("bezier"); bz != tj->end() && bz->is_array()) {
                    el.elTrBezier.clear();
                    for (const auto& v : *bz)
                        if (v.is_number()) el.elTrBezier.push_back(v.get<double>());
                }
            }
            if (auto c = e.find("cutout"); c != e.end() && c->is_object()) {
                el.hasCutout = true;
                el.cutX = jnum(*c, "x");
                el.cutY = jnum(*c, "y");
                el.cutW = jnum(*c, "w");
                el.cutH = jnum(*c, "h");
                el.cutR = jnum(*c, "radius");
            }
            if (auto ps = e.find("parts"); ps != e.end() && ps->is_array()) {
                for (const auto& pr : *ps) {
                    if (!pr.is_object() || el.parts.size() >= 4)
                        continue;
                    GlassElement::PartRect part;
                    part.a = jnum(pr, "alpha", 1.0);
                    if (auto sd = pr.find("side"); sd != pr.end() && sd->is_string()) {
                        const std::string sv = sd->get<std::string>();
                        part.trSide = sv == "bottom" ? 1 : sv == "left" ? 2 : sv == "right" ? 3 : 0;
                    }
                    if (auto tj = pr.find("transition"); tj != pr.end() && tj->is_object()) {
                        const std::string kv = tj->value("kind", "");
                        part.trKind       = kv == "open" ? 1 : kv == "close" ? 2 : 0;
                        part.trDurMs      = jnum(*tj, "durationMs", 0.0);
                        part.trProtrusion = jnum(*tj, "protrusionPx", 0.0);
                        part.trTravel     = jnum(*tj, "travelPx", part.trProtrusion);
                        const double elapsed = jnum(*tj, "elapsedMs", 0.0);
                        part.trStart = std::chrono::steady_clock::now() - boundedElapsedMilliseconds(elapsed);
                        if (auto bz = tj->find("bezier"); bz != tj->end() && bz->is_array()) {
                            part.trBezier.clear();
                            for (const auto& v : *bz)
                                if (v.is_number()) part.trBezier.push_back(v.get<double>());
                        }
                        // A close already past its end must not re-enter the
                        // part list: the shell keeps re-sending through the
                        // +120ms grace and each re-add re-ran the close's last
                        // frame + re-erased (the doubled part.tr-done).
                        if (part.trKind == 2 && elapsed >= part.trDurMs)
                            continue;
                    }
                    part.x = jnum(pr, "x");
                    part.y = jnum(pr, "y");
                    part.w = jnum(pr, "w");
                    part.h = jnum(pr, "h");
                    if (auto c = pr.find("corner"); c != pr.end() && c->is_array())
                        for (size_t ci = 0; ci < 4 && ci < c->size(); ++ci)
                            if ((*c)[ci].is_number())
                                part.c[ci] = (*c)[ci].get<double>();
                    if (auto kk = pr.find("k"); kk != pr.end() && kk->is_array())
                        for (size_t ki = 0; ki < 4 && ki < kk->size(); ++ki)
                            if ((*kk)[ki].is_number())
                                part.k[ki] = (*kk)[ki].get<double>();
                    if (auto ex = pr.find("ext"); ex != pr.end() && ex->is_array() && ex->size() >= 4) {
                        part.ex = (*ex)[0].is_number() ? (*ex)[0].get<double>() : 0.0;
                        part.ey = (*ex)[1].is_number() ? (*ex)[1].get<double>() : 0.0;
                        part.ew = (*ex)[2].is_number() ? (*ex)[2].get<double>() : -1.0;
                        part.eh = (*ex)[3].is_number() ? (*ex)[3].get<double>() : 0.0;
                    }
                    if (part.ew <= 0.5) {   // no extent given → the body rect is the region
                        part.ex = part.x;
                        part.ey = part.y;
                        part.ew = part.w;
                        part.eh = part.h;
                    }
                    if (part.w > 0.5 && part.h > 0.5)
                        el.parts.push_back(part);
                }
            }
            el.connectorCurve = jnum(e, "connectorCurve", el.connectorCurve);
            if (auto u = e.find("under"); u != e.end() && u->is_boolean())
                el.under = u->get<bool>();
            if (auto pl = e.find("plugs"); pl != e.end() && pl->is_array()) {
                for (const auto& pr : *pl) {
                    if (!pr.is_object() || el.plugs.size() >= 4)
                        continue;
                    GlassElement::PlugRect plug;
                    plug.x = jnum(pr, "x");
                    plug.y = jnum(pr, "y");
                    plug.w = jnum(pr, "w");
                    plug.h = jnum(pr, "h");
                    if (plug.w > 0.5 && plug.h > 0.5)
                        el.plugs.push_back(plug);
                }
            }
            if (el.id.empty()) el.id = (el.monitor.empty() ? std::string("anchor") : el.monitor) + ":" + std::to_string(parsed.size());
            if (el.monitor.empty() && el.bindType != "window")
                return rejectApply("element requires a monitor or window bind");
            if (parsed.contains(el.id))
                return rejectApply("element ids must be unique");
            parsed.emplace(el.id, std::move(el));
        }
    }
    const bool enabled = jbool(doc, "enabled", true);
    const double animMs = jnum(doc, "animMs", -1.0);

    bool              anyChange = false;
    bool              damageAll = false;   // fallback when a box's monitor can't be resolved
    std::vector<CBox> changedBoxes;        // global logical
    {
        std::lock_guard g(g_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        if (animMs >= 0.0) g_animMs = animMs;

        auto monitorPos = [](const std::string& name) -> std::optional<Vector2D> {
            for (const auto& m : g_pCompositor->m_monitors)
                if (m && m->m_name == name) return m->m_position;
            return std::nullopt;
        };
        auto addBox = [&](const GlassElement& el) {
            if (el.monitor.empty()) {
                damageAll = true;   // window-bound: position resolves at render time
                return;
            }
            if (const auto p = monitorPos(el.monitor))
                changedBoxes.push_back(CBox(p->x + el.x, p->y + el.y, el.w, el.h));
            else
                damageAll = true;
        };

        auto describe = [](const GlassElement& e) {
            std::string s = (e.monitor.empty() ? std::string("<anchored>") : e.monitor) + " " +
                std::to_string(static_cast<int>(e.x)) + "," + std::to_string(static_cast<int>(e.y)) + " " +
                std::to_string(static_cast<int>(e.w)) + "x" + std::to_string(static_cast<int>(e.h));
            if (!e.bindType.empty())
                s += " bind=" + e.bindType + ":" + e.bindSelector;
            return s;
        };

        // Merge (not replace) so enter/exit animations survive re-applies: new ids
        // animate in; ids that vanished animate out (kept until the exit completes);
        // existing ids keep their animation state but take the new geometry/material.
        // Unchanged elements cause no damage at all — a client re-sending its state
        // on a timer must not repaint anything.
        for (auto& [id, pe] : parsed) {
            pe.lastBindOk = now;   // fresh apply restarts the unbound-purge grace period
            auto it = g_elements.find(id);
            if (it == g_elements.end()) {
                if (pe.elTrKind == 2) {
                    // A close that is already past its end must not REBIRTH an
                    // element this plugin self-terminated (stale re-send race).
                    const double staleMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pe.elTrStart).count();
                    if (staleMs >= pe.elTrDurMs) {
                        dbgLog(id, "el.tr-stale", "expired close on new element, skipped");
                        continue;
                    }
                }
                // Acceptance logging (recv moved out of the parse: it fired on
                // EVERY re-send, ~55/s, and flooded the ring).
                if (pe.elTrKind != 0)
                    dbgLog(id, "el.tr-recv", std::string(pe.elTrKind == 1 ? "open" : "close") + " dur=" + std::to_string((int)pe.elTrDurMs) + " travel=" + std::to_string((int)pe.elTrTravel) + " side=" + std::to_string(pe.elTrSide));
                for (const auto& np : pe.parts)
                    if (np.trKind != 0)
                        dbgLog(id, "part.tr-recv", std::string(np.trKind == 1 ? "open" : "close") + " dur=" + std::to_string((int)np.trDurMs) + " bez=" + std::to_string(np.trBezier.size()));
                pe.birth   = now;
                pe.exiting = false;
                anyChange  = true;
                addBox(pe);
                dbgLog(id, "apply.new", describe(pe));
                g_elements.emplace(id, pe);
            } else {
                const bool changed = !sameRenderFields(it->second, pe) || it->second.exiting;
                // Transient diagnostics: name WHAT changed (hunting a 55/s
                // payload oscillation on the frame ring).
                if (changed && !it->second.exiting) {
                    const auto& o = it->second;
                    std::string why;
                    if (o.x != pe.x || o.y != pe.y || o.w != pe.w || o.h != pe.h) why += "geom ";
                    if (!samePartRects(o.parts, pe.parts)) {
                        why += "parts(";
                        for (size_t i = 0; i < std::max(o.parts.size(), pe.parts.size()); i++) {
                            if (i >= o.parts.size() || i >= pe.parts.size()) { why += "#" + std::to_string(i) + ":count "; continue; }
                            const auto &pa = o.parts[i], &pb = pe.parts[i];
                            if (pa.x != pb.x || pa.y != pb.y || pa.w != pb.w || pa.h != pb.h)
                                why += "#" + std::to_string(i) + ":rect[" + std::to_string(pa.x) + "->" + std::to_string(pb.x) + "," + std::to_string(pa.w) + "->" + std::to_string(pb.w) + "] ";
                            for (int c = 0; c < 4; c++) {
                                if (pa.k[c] != pb.k[c]) why += "#" + std::to_string(i) + ":k" + std::to_string(c) + " ";
                                if (pa.c[c] != pb.c[c]) why += "#" + std::to_string(i) + ":c" + std::to_string(c) + " ";
                            }
                            if (pa.ex != pb.ex || pa.ey != pb.ey || pa.ew != pb.ew || pa.eh != pb.eh) why += "#" + std::to_string(i) + ":ext ";
                        }
                        why += ") ";
                    }
                    if (o.tintR != pe.tintR || o.tintG != pe.tintG || o.tintB != pe.tintB) why += "tint ";
                    if (o.glassLevel != pe.glassLevel || o.blurLevel != pe.blurLevel || o.tintLevel != pe.tintLevel) why += "level ";
                    if (o.elTrKind != pe.elTrKind) why += "eltr ";
                    if (!why.empty()) dbgLog(id, "apply.why", why.substr(0, 200));
                }
                if (changed) {
                    anyChange = true;
                    addBox(it->second);
                    addBox(pe);
                    dbgLog(id, "apply.update", describe(pe));
                }
                pe.birth         = it->second.exiting ? now : it->second.birth;  // re-entering restarts the clock
                pe.exiting       = false;
                pe.everBound     = it->second.everBound;   // keep binding/anim/damage state across re-applies
                pe.bound         = it->second.bound;
                pe.wasDrawn      = it->second.wasDrawn;
                pe.lastGX        = it->second.lastGX;
                pe.lastGY        = it->second.lastGY;
                pe.lastGW        = it->second.lastGW;
                pe.lastGH        = it->second.lastGH;
                pe.drawCount     = it->second.drawCount;
                pe.lastDrawCause = it->second.lastDrawCause;
                // P4: carry the draw-confirm + observed state. pe.rev is the NEW
                // revision; drawnRev keeps the old one, so if the rev changed the
                // descriptor is no longer draw-confirmed until it redraws (readiness
                // for rev N never authorizes rev N+1). A readiness-relevant field
                // change marks the event channel dirty so the shell learns promptly.
                pe.drawnRev   = it->second.drawnRev;
                pe.drawnEpoch = it->second.drawnEpoch;
                pe.obsW      = it->second.obsW;
                pe.obsH      = it->second.obsH;
                pe.coordDist = it->second.coordDist;
                if (pe.rev != it->second.rev || changed)
                    g_glassDirty = true;
                pe.sx  = it->second.sx;   // smoothed geometry keeps gliding toward the new targets
                pe.sy  = it->second.sy;
                pe.sw  = it->second.sw;
                pe.sh  = it->second.sh;
                pe.sr  = it->second.sr;
                pe.srx = it->second.srx;
                pe.sry = it->second.sry;
                pe.sParts  = it->second.sParts;   // forget this and parts snap on every re-apply (no smoothing at all)
                pe.sPartAlpha = it->second.sPartAlpha;
                pe.dyingParts = it->second.dyingParts;
                pe.dyingAlpha = it->second.dyingAlpha;
                for (size_t pi = 0; pi < pe.parts.size(); ++pi) {
                    auto& newp = pe.parts[pi];
                    if (pi < it->second.parts.size()) {
                        const auto& oldp = it->second.parts[pi];
                        if (newp.trKind != 0 && newp.trKind == oldp.trKind) {
                            // FIRST ARRIVAL WINS: the shell re-attaches the transition
                            // on every push (~55/s); re-parsing the clock each time
                            // starved completion (P5 regression). Same-kind re-send =
                            // keep the ORIGINAL transition state wholesale.
                            newp.trStart      = oldp.trStart;
                            newp.trDone       = oldp.trDone;
                            newp.trDurMs      = oldp.trDurMs;
                            newp.trBezier     = oldp.trBezier;
                            newp.trProtrusion = oldp.trProtrusion;
                            newp.trTravel     = oldp.trTravel;
                            continue;
                        }
                    }
                    // Acceptance log: a genuinely NEW transition (kind changed
                    // or a fresh part index) — recv no longer logs at parse.
                    if (newp.trKind != 0)
                        dbgLog(id, "part.tr-recv", std::string(newp.trKind == 1 ? "open" : "close") + " dur=" + std::to_string((int)newp.trDurMs) + " bez=" + std::to_string(newp.trBezier.size()));
                }
                if (pe.elTrKind != 0 && pe.elTrKind == it->second.elTrKind) {
                    // FIRST ARRIVAL WINS at element level too: re-sends during the
                    // slide carry a fresh elapsedMs — keep the original clock.
                    pe.elTrStart  = it->second.elTrStart;
                    pe.elTrDone   = it->second.elTrDone;
                    pe.elTrDurMs  = it->second.elTrDurMs;
                    pe.elTrBezier = it->second.elTrBezier;
                    pe.elTrTravel = it->second.elTrTravel;
                } else if (pe.elTrKind != 0) {
                    dbgLog(id, "el.tr-recv", std::string(pe.elTrKind == 1 ? "open" : "close") + " dur=" + std::to_string((int)pe.elTrDurMs) + " travel=" + std::to_string((int)pe.elTrTravel) + " side=" + std::to_string(pe.elTrSide));
                }
                pe.smoothT = it->second.smoothT;
                it->second = pe;
            }
        }
        for (auto& [id, el] : g_elements) {
            if (!el.exiting && parsed.find(id) == parsed.end()) {
                el.exiting   = true;
                el.exitStart = now;
                anyChange    = true;
                addBox(el);
                dbgLog(id, "apply.remove", "exit-start");
            }
        }
        if (enabled != g_enabled) {
            anyChange = true;
            damageAll = true;
            dbgLog("", "material", enabled ? "on (via apply)" : "off (via apply)");
        }
        g_enabled  = enabled;
        g_lastApplyStatus = "accepted";
        g_lastError.clear();
        if (anyChange) {
            ++g_generation;
            g_glassDirty = true;   // P4: element set / geometry changed → emit readiness
        }
    }
    if (anyChange) {
        if (damageAll)
            damageAllMonitors();
        else if (g_pHyprRenderer)
            for (const auto& b : changedBoxes)
                g_pHyprRenderer->damageBox(b);
    }
    return "ok\n";
}

std::string clearElements() {
    {
        std::lock_guard g(g_stateMutex);
        dbgLog("", "clear", std::to_string(g_elements.size()) + " element(s) dropped");
        while (!g_noBlurApplied.empty())
            clearUnderNoBlur(g_noBlurApplied.begin()->first);
        g_elements.clear();
        g_elemFBs.clear();
        g_lastApplyStatus = "cleared";
        ++g_generation;
    }
    damageAllMonitors();
    return "ok\n";
}

std::string setEnabled(bool on) {
    { std::lock_guard g(g_stateMutex); g_enabled = on; g_lastRenderStatus = on ? "pending" : "disabled"; }
    dbgLog("", "material", on ? "on" : "off");
    damageAllMonitors();
    return std::string("hyprfluidglass: ") + (on ? "on" : "off") + "\n";
}

// Diagnostic dump for tools/hyprfluidglass-debug. Surface selectors and event
// details are redacted unless the caller explicitly uses the verbose command.
std::string debugJson(bool includeIdentities) {
    json state;
    {
        std::lock_guard g(g_stateMutex);
        const auto      now = std::chrono::steady_clock::now();
        json            els = json::array();
        for (const auto& [id, el] : g_elements) {
            json e = {
                {"id", id},
                {"monitor", el.monitor.empty() ? "<from-bind>" : el.monitor},
                {"payloadRect", {el.x, el.y, el.w, el.h}},
                {"radius", el.radius},
                {"glassLevel", el.glassLevel},
                {"exiting", el.exiting},
                {"everBound", el.everBound},
                {"onScreen", el.wasDrawn},
                {"drawCount", el.drawCount},
                {"lastDrawCause", el.lastDrawCause},
            };
            if (!el.bindType.empty()) {
                e["bind"] = {
                    {"type", el.bindType},
                    {"selector", includeIdentities ? el.bindSelector : "<redacted>"},
                    {"relX", el.relX},
                    {"relY", el.relY},
                    {"bound", el.bound},
                    {"sinceBindOkMs", std::chrono::duration<double, std::milli>(now - el.lastBindOk).count()},
                };
            }
            if (el.wasDrawn)
                e["drawnAt"] = {el.lastGX, el.lastGY, el.lastGW, el.lastGH};
            if (el.hasCutout)
                e["cutout"] = {el.cutX, el.cutY, el.cutW, el.cutH, el.cutR};
            if (!el.parts.empty()) {
                json pj = json::array();
                for (const auto& part : el.parts)
                    pj.push_back({{"rect", {part.x, part.y, part.w, part.h}},
                                  {"alpha", part.a},
                                  {"corner", {part.c[0], part.c[1], part.c[2], part.c[3]}},
                                  {"k", {part.k[0], part.k[1], part.k[2], part.k[3]}},
                                  {"ext", {part.ex, part.ey, part.ew, part.eh}}});
                e["parts"] = pj;
            }
            if (!el.plugs.empty()) {
                json gj = json::array();
                for (const auto& plug : el.plugs)
                    gj.push_back({plug.x, plug.y, plug.w, plug.h});
                e["plugs"] = gj;
            }
            if (el.connectorCurve > 0)
                e["connectorCurve"] = el.connectorCurve;
            els.push_back(e);
        }
        json mons = json::object();
        for (const auto& [name, sd] : g_selfDamage)
            mons[name] = {{"selfDamageTtl", sd.ttl}};
        state = {
            {"enabled", g_enabled},
            {"generation", g_generation},
            {"animMs", g_animMs},
            {"shaderCompiled", g_shaderCompiled},
            {"blurShaderCompiled", g_blurCompiled},
            {"shaderError", g_shaderError},
            {"lastRenderStatus", g_lastRenderStatus},
            {"lastApplyStatus", g_lastApplyStatus},
            {"lastError", g_lastError},
            {"elements", els},
            {"monitors", mons},
        };
    }
    json evs = json::array();
    {
        std::lock_guard g(g_dbgMutex);
        for (const auto& e : g_dbgEvents)
            evs.push_back({
                {"seq", e.seq},
                {"tMs", e.tMs},
                {"id", e.id},
                {"event", e.event},
                {"info", includeIdentities || e.info.empty() ? e.info : "<redacted>"},
            });
    }
    // error_handler replace: a single invalid byte in any logged string must
    // never take the whole instrument down (it renders as U+FFFD instead).
    return json{{"state", state}, {"events", evs}}.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Steady-clock milliseconds — the freshness/heartbeat time base.
static double nowSteadyMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// P4: the per-descriptor readiness record — the SINGLE source consumed by both
// hyprfluidglass-status and the hgsglass event, so status and events can never
// disagree. Caller must hold g_stateMutex. Field roles (durable in the ledger):
//   ACTIVATION (strict predicate): rev, accepted, bound, coordinate.status=="aligned",
//     compositorMaterial.drawable, drawConfirmed (drawnRev==rev at this epoch).
//   DIAGNOSTIC: surfaceMatch detail, capture/texture/shaderReady, backendUsed,
//     expected/observed/distancePx, drawnRev, drawCount, at, reason.
//   TRANSIENT PREP: coordinate "pending" (bound, position/size not yet observed),
//     compositorMaterial "not-drawable" while the shader is still compiling.
static json descriptorReadinessJson(const std::string& id, const GlassElement& el) {
    const bool raw          = el.bindType.empty();
    const bool boundOk      = raw ? true : el.bound;
    const bool shaderReady  = g_shaderCompiled && g_shaderError.empty();

    std::string smStatus = raw ? "raw" : (el.bound ? "matched" : "unmatched");

    const double expW = el.w, expH = el.h, obsW = el.obsW, obsH = el.obsH;
    std::string coordStatus, coordReason;
    if (!boundOk) {
        coordStatus = "pending"; coordReason = "surface unbound";
    } else if (raw) {
        coordStatus = "aligned"; // raw geometry defines its own coordinates
    } else if (el.coordDist < 0) {
        coordStatus = "pending"; coordReason = "position not yet observed";
    } else {
        const double sizeTol = std::max(2.0, 0.01 * std::max(expW, expH));
        const bool   posOk   = el.coordDist <= COORD_ALIGN_PX;
        const bool   sizeOk  = (obsW < 0) || (std::abs(obsW - expW) <= sizeTol && std::abs(obsH - expH) <= sizeTol);
        if (posOk && sizeOk)
            coordStatus = "aligned";
        else if (el.coordDist <= COORD_ALIGN_PX * 4.0 && sizeOk) {
            coordStatus = "near"; coordReason = "position within relaxed tolerance";
        } else {
            coordStatus = "divergent";
            coordReason = "dist=" + std::to_string(static_cast<int>(el.coordDist)) +
                          "px exp=" + std::to_string(static_cast<int>(expW)) + "x" + std::to_string(static_cast<int>(expH)) +
                          (obsW < 0 ? "" : (" obs=" + std::to_string(static_cast<int>(obsW)) + "x" + std::to_string(static_cast<int>(obsH))));
        }
    }

    const bool coordAligned  = coordStatus == "aligned";
    const bool drawable      = g_enabled && shaderReady && boundOk && coordAligned;
    const bool drawConfirmed = el.rev != 0 && el.wasDrawn && el.drawnRev == el.rev && el.drawnEpoch == g_drawEpoch;
    const bool ready         = drawable && drawConfirmed;

    std::string reason = "ready";
    if (!g_enabled)            reason = "material disabled";
    else if (el.rev == 0)      reason = "no revision published";
    else if (!boundOk)         reason = "surface unbound";
    else if (!shaderReady)     reason = g_shaderError.empty() ? "shader not compiled" : g_shaderError;
    else if (!coordAligned)    reason = "coordinate " + coordStatus + (coordReason.empty() ? "" : " (" + coordReason + ")");
    else if (!drawConfirmed)   reason = el.drawnRev == el.rev ? "awaiting redraw (epoch)" : "not yet drawn at current rev";

    json d = {
        {"id", id},
        {"mon", el.monitor},   // parity with the event record; shell prunes per-monitor
        {"rev", el.rev},
        {"accepted", true},
        {"bound", boundOk},
        {"surfaceMatch", {{"status", smStatus}, {"matched", boundOk}}},
        {"coordinate", {
            {"status", coordStatus},
            {"expected", {expW, expH}},
            {"observed", {obsW, obsH}},
            {"distancePx", el.coordDist},
            {"reason", coordReason}}},
        {"compositorMaterial", {
            {"status", drawable ? "drawable" : "not-drawable"},
            {"drawable", drawable},
            {"mode", "fluid-glass"},
            {"captureReady", shaderReady},
            {"textureReady", shaderReady},
            {"shaderReady", shaderReady},
            {"shaderError", g_shaderError},
            {"backendUsed", "fluid-glass"}}},
        {"drawnRev", el.drawnRev},
        {"drawConfirmed", drawConfirmed},
        {"drawCount", el.drawCount},
        {"exiting", el.exiting},
        {"ready", ready},
        {"reason", reason},
    };
    if (el.wasDrawn)
        d["at"] = {el.lastGX, el.lastGY};
    return d;
}

std::string statusString(eHyprCtlOutputFormat format) {
    std::lock_guard g(g_stateMutex);
    if (format == FORMAT_JSON) {
        // Per-element binding state. The shell's resync check compares its local ids
        // against this list, so it only re-applies when something is genuinely missing.
        json descriptors = json::array();
        for (const auto& [id, el] : g_elements)
            descriptors.push_back(descriptorReadinessJson(id, el));   // P4: full readiness record
        json j = {
            {"plugin", "hyprfluidglass"},
            {"pluginLoaded", true},
            {"available", true},
            {"enabled", g_enabled},
            // The shell's reconcileMaterialMode compares material.mode against its
            // desired mode; without this it assumed "off" and re-enabled (with a
            // full-monitor damage) every poll, forever.
            {"material", {{"enabled", g_enabled}, {"mode", g_enabled ? "fluid-glass" : "off"}, {"lastRenderStatus", g_lastRenderStatus}}},
            {"dispatcher", true},   // hyprfluidglass-apply is registered — clients may use Hyprland dispatch transport
            {"elements", g_elements.size()},
            {"descriptors", descriptors},
            {"descriptorCount", static_cast<int>(g_elements.size())},
            {"renderHookInstalled", static_cast<bool>(g_renderStageListener)},
            {"shaderCompiled", g_shaderCompiled},
            {"blurShaderCompiled", g_blurCompiled},
            {"shaderError", g_shaderError},
            {"lastApplyStatus", g_lastApplyStatus},
            {"lastError", g_lastError},
            {"lastRenderStatus", g_lastRenderStatus},
            // P4: generation is the unique-per-load nonce (reload detection); the
            // event channel + heartbeat let the shell drop the 2s poll to a net.
            {"generation", g_pluginGen},
            {"eventSchema", 1},
            {"heartbeat", {{"counter", g_glassHbCounter}, {"tMs", g_glassLastHbMs}, {"activeDescriptors", static_cast<int>(g_elements.size())}}},
        };
        return j.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    return std::string("hyprfluidglass enabled=") + (g_enabled ? "yes" : "no") +
           " elements=" + std::to_string(g_elements.size()) +
           " render=" + g_lastRenderStatus + "\n";
}

// ── P4 hgsglass readiness event ───────────────────────────────────────────────
// Compact change signature: only the ACTIVATION-relevant fields (excludes volatile
// diagnostics like drawCount / exact position, which change every frame an element
// draws and would make every frame a "change"). Caller holds g_stateMutex.
static std::string glassSignatureLocked() {
    std::string s = g_enabled ? "E" : "e";
    s += ":g" + std::to_string(g_pluginGen);
    for (const auto& [id, el] : g_elements) {
        const bool raw     = el.bindType.empty();
        const bool boundOk = raw ? true : el.bound;
        const bool shOk    = g_shaderCompiled && g_shaderError.empty();
        const bool drawable = g_enabled && shOk && boundOk && (el.coordDist < 0 ? raw : el.coordDist <= COORD_ALIGN_PX);
        const bool drawn    = el.rev != 0 && el.wasDrawn && el.drawnRev == el.rev && el.drawnEpoch == g_drawEpoch;
        s += "|" + id + ":" + std::to_string(el.rev) + ":" + (boundOk ? "b" : "-") +
             (drawable ? "d" : "-") + (drawn ? "D" : "-") + (el.exiting ? "x" : "-");
    }
    return s;
}

// Compact per-descriptor record for the EVENT channel. Hyprland's socket2 has a
// per-line cap, and the full readiness JSON for several descriptors overruns it
// (the line is silently dropped) — so events carry only the fields the shell's
// strict predicate needs; the full diagnostic detail lives in the status poll.
// Field names match descriptorReadinessJson so the shell parses both identically.
static json descriptorEventJson(const std::string& id, const GlassElement& el) {
    const bool   raw          = el.bindType.empty();
    const bool   boundOk      = raw ? true : el.bound;
    const bool   shaderReady  = g_shaderCompiled && g_shaderError.empty();
    const bool   coordAligned = boundOk && (raw || (el.coordDist >= 0 && el.coordDist <= COORD_ALIGN_PX));
    const bool   drawable     = g_enabled && shaderReady && boundOk && coordAligned;
    const bool   drawn        = el.rev != 0 && el.wasDrawn && el.drawnRev == el.rev && el.drawnEpoch == g_drawEpoch;
    return {
        {"id", id},
        {"mon", el.monitor},   // shell scopes per-monitor slice replacement by this
        {"rev", el.rev},
        {"accepted", true},
        {"bound", boundOk},
        {"coordinate", {{"status", coordAligned ? "aligned" : (boundOk ? "pending" : "unbound")}}},
        {"compositorMaterial", {{"drawable", drawable}}},
        {"drawConfirmed", drawn},
        {"exiting", el.exiting}
    };
}

// Descriptor-free liveness tick. Tiny and constant-size, so it ALWAYS fits under
// the socket2 per-line cap no matter how many descriptors are active — the channel
// stays fresh (proving the plugin still renders) even when a bar has many surfaces.
// Caller holds g_stateMutex.
static std::string glassBuildHeartbeatLocked() {
    json j = {
        {"v", 1},
        {"gen", g_pluginGen},
        {"pluginLoaded", true},
        {"enabled", g_enabled},
        {"render", g_lastRenderStatus},
        {"kind", "heartbeat"},
        {"hb", g_glassHbCounter},
        {"tMs", nowSteadyMs()},
        {"activeDescriptors", static_cast<int>(g_elements.size())},
    };
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Per-MONITOR readiness snapshot: only the descriptors on `mon`. Readiness is
// sharded by monitor because that is the natural, bounded unit (a bar contributes
// at most its body or ≤3 islands per monitor), keeping every event well under the
// socket2 line cap where a single global event with all monitors' descriptors
// overran and was silently dropped. The shell replaces exactly this monitor's slice
// (keyed by each record's "mon"), so removal stays correct: a monitor whose bar
// surfaces are gone emits an empty descriptor list and the shell clears its slice.
// Caller holds g_stateMutex.
static std::string glassBuildMonitorReadinessLocked(const std::string& mon) {
    json descriptors = json::array();
    int  n = 0;
    for (const auto& [id, el] : g_elements)
        if (el.monitor == mon) { descriptors.push_back(descriptorEventJson(id, el)); ++n; }
    json j = {
        {"v", 1},
        {"gen", g_pluginGen},
        {"pluginLoaded", true},
        {"enabled", g_enabled},
        {"render", g_lastRenderStatus},
        {"kind", "readiness"},
        {"hb", g_glassHbCounter},
        {"monitor", mon},
        {"activeDescriptors", n},
        {"descriptors", descriptors},
    };
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Timer body: on a signature change emit a per-monitor readiness event for every
// monitor that has (or just lost) descriptors; otherwise a ~1s descriptor-free
// heartbeat while any descriptor is active. Runs on the event-loop thread; builds
// under the lock, posts after releasing it.
static void glassScan() {
    if (!g_pEventManager)
        return;
    std::vector<std::string> toPost;
    {
        std::lock_guard g(g_stateMutex);
        const std::string sig = glassSignatureLocked();
        const bool changed = (sig != g_glassLastSig) || g_glassDirty;
        const double now = nowSteadyMs();
        const bool active = !g_elements.empty();
        if (changed) {
            g_glassLastSig = sig;
            g_glassDirty   = false;
            g_glassLastHbMs = now;
            // Emit for monitors with descriptors now, PLUS monitors that had them
            // last time (so a monitor that just emptied gets a clearing snapshot).
            std::set<std::string> mons = g_glassLastMons;
            for (const auto& [id, el] : g_elements)
                if (!el.monitor.empty()) mons.insert(el.monitor);
            g_glassLastMons.clear();
            for (const auto& [id, el] : g_elements)
                if (!el.monitor.empty()) g_glassLastMons.insert(el.monitor);
            for (const auto& m : mons)
                toPost.push_back(glassBuildMonitorReadinessLocked(m));
        } else if (active && (now - g_glassLastHbMs) >= 1000.0) {
            ++g_glassHbCounter;
            g_glassLastHbMs = now;
            toPost.push_back(glassBuildHeartbeatLocked());
        }
    }
    for (const auto& s : toPost)
        g_pEventManager->postEvent(SHyprIPCEvent{"hgsglass", s});
}

// Re-arm cadence: brisk while descriptors are active or a change is pending
// (≤200ms change latency, coalescing rapid transitions); slow when idle so an
// empty shell costs nothing. No heartbeat traffic is emitted while idle.
static std::chrono::milliseconds glassNextInterval() {
    std::lock_guard g(g_stateMutex);
    return (!g_elements.empty() || g_glassDirty) ? std::chrono::milliseconds(200)
                                                 : std::chrono::milliseconds(1000);
}

std::string onStatus(eHyprCtlOutputFormat format, std::string) {
    return runCommandBoundary("status", [format] {
        return statusString(format);
    });
}
std::string onApply(eHyprCtlOutputFormat, std::string req) {
    return runCommandBoundary("apply", [request = std::move(req)]() mutable {
        return applyPayload(removePrefix(std::move(request), "hyprfluidglass-apply-json"));
    });
}
std::string onClear(eHyprCtlOutputFormat, std::string) {
    return runCommandBoundary("clear", clearElements);
}
std::string onDebugJson(eHyprCtlOutputFormat, std::string) {
    return runCommandBoundary("debug-json", [] {
        return debugJson(false);
    });
}
std::string onDebugJsonVerbose(eHyprCtlOutputFormat, std::string) {
    return runCommandBoundary("debug-json-verbose", [] {
        return debugJson(true);
    });
}

std::string onV2(eHyprCtlOutputFormat, std::string req) {
    static constexpr std::string_view UNAVAILABLE =
        R"({"ok":false,"version":2,"error":{"code":"internal-error","path":"","message":"runtime is unavailable"}})";
    try {
        if (!g_v2Runtime)
            return std::string(UNAVAILABLE);
        const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        return g_v2Runtime->handle(
            removePrefix(std::move(req), "hyprfluidglass"),
            nowMs);
    } catch (...) {
        return std::string(UNAVAILABLE);
    }
}

// Dispatcher twin of hyprfluidglass-apply-json: reachable over the Hyprland socket's
// `dispatch` request, which clients can send WITHOUT spawning hyprctl (quickshell's
// Hyprland.dispatch). That drops per-update transport cost from a fork+exec to a
// socket write — the difference between ~20 geometry updates/s and full-rate.
SDispatchResult onApplyDispatch(std::string arg) {
    const std::string res = runCommandBoundary("apply-dispatch", [request = std::move(arg)]() mutable {
        return applyPayload(std::move(request));
    });
    const bool        ok  = res.rfind("ok", 0) == 0;
    return SDispatchResult{.success = ok, .error = ok ? "" : res};
}
std::string onMaterialImpl(eHyprCtlOutputFormat format, std::string req) {
    const std::string m = lower(removePrefix(std::move(req), "hyprfluidglass-material"));
    if (m.empty() || m == "status") return statusString(format);
    if (m.rfind("debug:", 0) == 0) {
        try { g_debugField = std::stoi(m.substr(6)); } catch (...) { g_debugField = 0; }
        damageAllMonitors();
        return "debug field " + std::to_string(g_debugField) + "\n";
    }
    if (m == "off" || m == "disable" || m == "disabled" || m == "false" || m == "0") return setEnabled(false);
    if (m == "fluid-glass" || m == "on" || m == "enable" || m == "enabled" || m == "true" || m == "1") return setEnabled(true);
    return "error: expected on/off/fluid-glass or status\n";
}
std::string onMaterial(eHyprCtlOutputFormat format, std::string req) {
    return runCommandBoundary("material", [format, request = std::move(req)]() mutable {
        return onMaterialImpl(format, std::move(request));
    });
}

} // namespace

// ── Plugin lifecycle ──────────────────────────────────────────────────────
APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;
    g_v2Runtime = std::make_unique<hfg::v2::RuntimeService>(hfg::v2::secureOpaqueId);

    // P4: unique-per-load generation nonce (steady-clock ms — kept within JS's
    // safe-integer range so the shell compares it exactly). The shell keys reload
    // detection on this: a returning plugin starts empty, so a changed gen forces
    // descriptor republish.
    g_pluginGen = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    SHyprCtlCommand status; status.name = "hyprfluidglass-status";     status.exact = true;  status.fn = onStatus;
    g_statusCommand = HyprlandAPI::registerHyprCtlCommand(g_handle, status);
    SHyprCtlCommand apply;  apply.name  = "hyprfluidglass-apply-json"; apply.exact  = false; apply.fn  = onApply;
    g_applyCommand  = HyprlandAPI::registerHyprCtlCommand(g_handle, apply);
    SHyprCtlCommand clear;  clear.name  = "hyprfluidglass-clear";      clear.exact  = true;  clear.fn  = onClear;
    g_clearCommand  = HyprlandAPI::registerHyprCtlCommand(g_handle, clear);
    SHyprCtlCommand mat;    mat.name    = "hyprfluidglass-material";   mat.exact    = false; mat.fn    = onMaterial;
    g_materialCommand = HyprlandAPI::registerHyprCtlCommand(g_handle, mat);
    SHyprCtlCommand dbg;    dbg.name    = "hyprfluidglass-debug-json"; dbg.exact    = true;  dbg.fn    = onDebugJson;
    g_debugCommand = HyprlandAPI::registerHyprCtlCommand(g_handle, dbg);
    SHyprCtlCommand dbgVerbose; dbgVerbose.name = "hyprfluidglass-debug-json-verbose"; dbgVerbose.exact = true; dbgVerbose.fn = onDebugJsonVerbose;
    g_debugVerboseCommand = HyprlandAPI::registerHyprCtlCommand(g_handle, dbgVerbose);
    // HyprCtl checks non-exact commands in registration order. Keep this generic
    // prefix after the compatibility commands so their longer names retain priority.
    SHyprCtlCommand v2; v2.name = "hyprfluidglass"; v2.exact = false; v2.fn = onV2;
    g_v2Command = HyprlandAPI::registerHyprCtlCommand(g_handle, v2);
    HyprlandAPI::addDispatcherV2(g_handle, "hyprfluidglass-apply", onApplyDispatch);

    if (g_pEventLoopManager) {
        // P4: hgsglass readiness/health timer — coalesced change events + ~1s
        // heartbeat while descriptors are active; self-re-arms at a variable cadence.
        g_glassTimer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(200), [](SP<CEventLoopTimer> self, void*) {
            try {
                glassScan();
                if (self)
                    self->updateTimeout(glassNextInterval());
            } catch (const std::exception& error) {
                recordBoundaryFailure("readiness-timer", error.what());
                if (self)
                    self->updateTimeout(std::chrono::seconds(1));
            } catch (...) {
                recordBoundaryFailure("readiness-timer", "non-standard exception");
                if (self)
                    self->updateTimeout(std::chrono::seconds(1));
            }
        }, nullptr);
        g_pEventLoopManager->addTimer(g_glassTimer);
    }

    if (Event::bus())
        g_renderStageListener = Event::bus()->m_events.render.stage.listen(renderFluidGlass);

    // P4: the initial readiness snapshot is emitted by the FIRST glassScan tick
    // (~200ms) rather than a synchronous postEvent here — a direct emit at
    // PLUGIN_INIT lands outside the event-loop dispatch window and is dropped
    // (observed live). Leaving g_glassLastSig empty makes that first tick see a
    // signature change and emit the snapshot through the proven timer path.

    return {"hyprfluidglass", "Live fluid-glass compositor material for Hyprland", "CoastLineSec", HYPRFLUIDGLASS_PLUGIN_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_renderStageListener.reset();
    // P4: farewell event so the shell learns of an orderly teardown immediately
    // (its strict readiness must drop; visual recovery must not await the poll).
    if (g_pEventManager) {
        json bye = {{"v", 1}, {"gen", g_pluginGen}, {"pluginLoaded", false}, {"kind", "farewell"},
                    {"enabled", false}, {"tMs", nowSteadyMs()}, {"activeDescriptors", 0}, {"descriptors", json::array()}};
        g_pEventManager->postEvent(SHyprIPCEvent{"hgsglass", bye.dump(-1, ' ', false, json::error_handler_t::replace)});
    }
    if (g_glassTimer) {
        g_glassTimer->cancel();
        if (g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(g_glassTimer);
        g_glassTimer.reset();
    }
    {
        std::lock_guard g(g_stateMutex);
        g_enabled = false;
        while (!g_noBlurApplied.empty())
            clearUnderNoBlur(g_noBlurApplied.begin()->first);   // release claimed windows before we go
        g_elements.clear();
        g_lastRenderStatus = "disabled";
    }
    g_captureFBs.clear();
    g_captureKnownSize.clear();
    g_elemFBs.clear();
    g_selfDamage.clear();
    g_preWindowDrawn.clear();
    g_shader.reset();
    g_blurShader.reset();
    g_blurCompiled   = false;
    g_shaderCompiled = g_shaderAttempted = false;
    g_shaderRetries  = 0;
    if (g_quadVbo) { glDeleteBuffers(1, &g_quadVbo); g_quadVbo = 0; }
    if (g_quadVao) { glDeleteVertexArrays(1, &g_quadVao); g_quadVao = 0; }
    HyprlandAPI::removeDispatcher(g_handle, "hyprfluidglass-apply");
    if (g_v2Command)       HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_v2Command);
    if (g_debugVerboseCommand) HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_debugVerboseCommand);
    if (g_debugCommand)    HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_debugCommand);
    if (g_materialCommand) HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_materialCommand);
    if (g_clearCommand)    HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_clearCommand);
    if (g_applyCommand)    HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_applyCommand);
    if (g_statusCommand)   HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_statusCommand);
    g_v2Command.reset(); g_debugVerboseCommand.reset(); g_debugCommand.reset(); g_materialCommand.reset(); g_clearCommand.reset(); g_applyCommand.reset(); g_statusCommand.reset();
    g_v2Runtime.reset();
    {
        std::lock_guard g(g_dbgMutex);
        g_dbgEvents.clear();
    }
    g_handle = nullptr;
}
