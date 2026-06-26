<div align="center">

# fluidglass

**Live fluid-glass compositor material for Hyprland.**

A Hyprland plugin that renders real, refractive "liquid glass" over the actual
framebuffer behind any rectangle a client asks for — frost, edge lensing, a
convex bevel, a specular rim, and an optional cursor-tracked point light.

</div>

---

## What it is

`fluidglass` is the rendering half of a glass UI. It does **not** decide *what*
should be glass — a client (a shell, a bar, a widget) sends it element geometry
over `hyprctl`, and the plugin captures the real pixels behind each element at
`RENDER_POST_WINDOWS` and runs the fluid-glass shader over them. Because the
capture is the live framebuffer, anything that scrolls or animates behind the
glass refracts through it in real time.

It is the companion compositor plugin for
[HyprGlassShell](https://github.com/CoastLineSec/HyprGlassShell), but the IPC
contract is generic — anything that can call `hyprctl` can drive it.

### Features

- **Real backdrop capture** — refracts the actual content behind each element,
  not a static blur.
- **Tunable material** — frost (Gaussian, dithered), edge lensing/refraction,
  convex bevel (inner highlight + shadow), specular rim.
- **Independent blur & tint** — drive them together from a single `glassLevel`,
  or set `blurLevel` / `tintLevel` separately.
- **Optional color tint** — neutral clear glass, or a tinted "stained glass".
- **Cursor light** — a point light that tracks the mouse, with distance falloff,
  for an Apple-style tilt/gyroscope highlight. Or a fixed light angle.
- **Window anchoring** — anchor an element to a window (by regex); it tracks the
  window's live position across moves and monitors.
- **Rotated/flipped displays** — correct on transformed monitors.

## Requirements

- **Hyprland** (a matching dev/header package for manual builds).
- A C++26 toolchain + CMake ≥ 3.16 (for manual builds; `hyprpm` brings its own
  build environment).
- `nlohmann/json` is **vendored** (`include/`), so it is *not* a system
  dependency.

## Install

### Via hyprpm (recommended)

[`hyprpm`](https://wiki.hyprland.org/Plugins/Using-Plugins/) is Hyprland's
official plugin manager. It builds the plugin against your exact Hyprland and
rebuilds it on Hyprland updates.

```sh
hyprpm add https://github.com/CoastLineSec/fluidglass
hyprpm enable fluidglass
```

Then add to your Hyprland config so it loads (and re-syncs after updates) on
launch:

```ini
exec-once = hyprpm reload
```

After a Hyprland upgrade: `hyprpm update` (rebuilds), then `hyprpm reload`.

### Manual build

```sh
git clone https://github.com/CoastLineSec/fluidglass
cd fluidglass
make                      # → build/fluidglass.so
hyprctl plugin load "$PWD/build/fluidglass.so"
```

`make dev-artifact` produces a uniquely-named copy (timestamp + commit) — useful
when iterating, since Hyprland's loader caches plugins by path.

## Usage

The plugin registers four `hyprctl` dispatchers. It does nothing until a client
sends it elements and enables it.

| Dispatcher | Purpose |
|---|---|
| `fluidglass-apply-json <json>` | Replace the element set + global enable state. |
| `fluidglass-status` | Plugin/render status. Use `hyprctl -j fluidglass-status` for JSON. |
| `fluidglass-clear` | Remove all elements. |
| `fluidglass-material <on\|off\|status>` | Global enable/disable, or status. |

### Apply payload

```sh
hyprctl fluidglass-apply-json '{
  "enabled": true,
  "elements": [
    { "id": "bar", "monitor": "DP-1",
      "x": 16, "y": 12, "w": 1888, "h": 40, "radius": 20,
      "glassLevel": 0.5, "tintEnabled": false }
  ]
}'
```

`enabled` is the global on/off. `elements` fully replaces the current set. All
geometry is **monitor-local logical pixels** (top-left origin).

### Element fields

| Field | Type | Default | Meaning |
|---|---|---|---|
| `id` | string | auto | Stable identifier (auto-generated if omitted). |
| `monitor` | string | — | Monitor name (e.g. `DP-1`). Required unless `anchorWindow` is set. |
| `x`, `y` | number | 0 | Top-left, monitor-local logical px. |
| `w`, `h` | number | 0 | Size, logical px. |
| `radius` | number | 0 | Corner radius, logical px. |
| `glassLevel` | 0–1 | 0.5 | Combined frost + tint amount. |
| `blurLevel` | 0–1 | −1 | Frost only; `−1` = derive from `glassLevel`. |
| `tintLevel` | 0–1 | −1 | Tint only; `−1` = derive from `glassLevel`. |
| `tintEnabled` | bool | false | Apply the color tint. |
| `tintColor` | `#RRGGBB` | white | Tint color. |
| `refraction` | number | 45 | Edge-lensing strength (design px @ 200 ref). |
| `rimBand` | number | 40 | Refraction band width. |
| `bevel` | number | 46 | Convex bevel band width. |
| `rimWidth` | number | 3 | Specular rim width. |
| `highlight` | number | 0.10 | Bevel highlight strength. |
| `shadow` | number | 0.10 | Bevel shadow strength. |
| `lightAngle` | degrees | 90 | Fixed light direction. |
| `specular` | number | 0.21 | Specular rim strength. |
| `rimWrap` | number | 0.45 | How far the rim wraps away from the light. |
| `lightFollowsMouse` | bool | false | Cursor point-light instead of a fixed angle. |
| `falloffReach` | px | 1000 | Distance over which the cursor light fades. |
| `falloffFloor` | number | 0.05 | Light strength far from the element. |
| `lightTightness` | number | 1.0 | Point-light spot exponent (higher = tighter). |
| `anchorWindow` | string | — | `getWindowByRegex` selector; anchors the element to a window. |
| `offsetX`, `offsetY` | number | 0 | Offset of the rect within the anchored window. |

When `anchorWindow` is set, `monitor`/`x`/`y` are recomputed each frame from the
target window's live server-side position plus `offsetX`/`offsetY`, so the glass
tracks the window as it moves between monitors.

### Material model

Pixel parameters (blur, refraction, bevel, rim) are treated as ratios of the
element's smaller dimension, capped at a **200px design reference**, then scaled
by the monitor scale. So a small element and a large one read as the "same
glass," and the look is resolution-independent. `glassLevel` maps to blur
(≈6–22px @ the 200px ref) and tint (≈0.04–0.30).

## How it works

1. A `CCapturePass` copies the monitor's current framebuffer into a feedback-safe
   texture at `RENDER_POST_WINDOWS`.
2. For each element on that monitor, a `CGlassPass` runs the fluid-glass fragment
   shader over the captured backdrop within the element's rounded rect.
3. The element's area is re-damaged each frame so frost/refraction recompute
   cleanly without leaving stale window edges.

Element corners are mapped through the inverse monitor transform, so rotated and
flipped outputs render correctly.

## Versioning

`fluidglass` follows the Hyprland plugin model: it is pinned to a Hyprland ABI
and must be rebuilt when Hyprland updates — `hyprpm` automates this. The plugin
reports its version to Hyprland (shown in `hyprctl plugins list`); build time and
source commit are compiled in for diagnostics.

## Credits & license

- Glass material and plugin: **CoastLineSec**, [MIT](LICENSE).
- Bundled [nlohmann/json](https://github.com/nlohmann/json) (`include/`), MIT.
- Built on the [Hyprland](https://github.com/hyprwm/Hyprland) plugin API.
