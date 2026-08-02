<div align="center">

# HyprFluidGlass

**Live fluid-glass compositor material for Hyprland.**

A Hyprland plugin that renders real, refractive "liquid glass" over the actual
framebuffer behind any rectangle a client asks for — frost, edge lensing, a
convex bevel, a specular rim, and an optional cursor-tracked point light.

</div>

---

## What it is

`hyprfluidglass` is the rendering half of a glass UI. It does **not** decide *what*
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
- **Tunable material** — separable Gaussian frost, edge lensing/refraction,
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
- A C++23 toolchain + CMake ≥ 3.16 (for manual builds; `hyprpm` brings its own
  build environment).
- `nlohmann/json` is **vendored** (`include/`), so it is *not* a system
  dependency.

## Install

### Via hyprpm (recommended)

[`hyprpm`](https://wiki.hyprland.org/Plugins/Using-Plugins/) is Hyprland's
official plugin manager. It builds the plugin against your exact Hyprland and
rebuilds it on Hyprland updates.

```sh
hyprpm add https://github.com/CoastLineSec/HyprFluidGlass
hyprpm enable hyprfluidglass
```

Then add to your Hyprland config so it loads (and re-syncs after updates) on
launch:

```ini
exec-once = hyprpm reload
```

After a Hyprland upgrade: `hyprpm update` (rebuilds), then `hyprpm reload`.

### Manual build

```sh
git clone https://github.com/CoastLineSec/HyprFluidGlass
cd HyprFluidGlass
make                      # → build/hyprfluidglass.so
hyprctl plugin load "$PWD/build/hyprfluidglass.so"
```

`make dev-artifact` produces a uniquely-named copy (timestamp + commit) — useful
when iterating, since Hyprland's loader caches plugins by path.

## Usage

The plugin exposes one command: the versioned v2 runtime protocol.

| Command | Purpose |
|---|---|
| `hyprfluidglass <json>` | Versioned v2 runtime protocol. |

See the [runtime protocol reference](docs/reference/runtime-protocol.md) for
session-based v2 requests. Clients must query `capabilities` before submitting
v2 render targets. Durable v2 materials and application/layer attachment rules
use the [native Lua configuration interface](docs/reference/lua-configuration.md).

### Material model

Pixel parameters (blur, refraction, bevel, rim) are treated as ratios of the
target's smaller dimension, capped at a **200px design reference**, then scaled
by the monitor scale. So a small target and a large one read as the "same
glass," and the look is resolution-independent. `glass_level` maps to blur
(≈6–22px @ the 200px ref) and tint (≈0.04–0.30).

## How it works

The v2 renderer resolves each target against compositor-owned window, layer, or
output state. It maps the resolved geometry into the output's physical render
space, allocates a bounded capture around the material's sampling footprint,
and inserts capture and glass passes at the requested render stage. Window
targets render from a compositor decoration below the application surface.

Captures are keyed by output generation, render format, color-state identity,
stage, and stage object. A draw is skipped unless its capture succeeded in the
same frame. Outputs carrying active glass targets receive a scoped
direct-scanout inhibition lease so Hyprland cannot bypass the compositor render
path while the effect is needed.

Everything maps through the output transform so rotated and flipped outputs
render correctly.

## Versioning

`hyprfluidglass` follows the Hyprland plugin model: it is pinned to a Hyprland ABI
and must be rebuilt when Hyprland updates — `hyprpm` automates this. The plugin
reports its version to Hyprland (shown in `hyprctl plugins list`); build time and
source commit are compiled in for diagnostics.

## Credits & license

- Glass material and plugin: **CoastLineSec**, [MIT](LICENSE).
- Bundled [nlohmann/json](https://github.com/nlohmann/json) (`include/`), MIT.
- Built on the [Hyprland](https://github.com/hyprwm/Hyprland) plugin API.
