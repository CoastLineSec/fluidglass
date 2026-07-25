# Rendering and geometry contract

Availability: applies to the HyprFluidGlass v2 renderer.

This contract defines how target geometry reaches the compositor render pass.
It is shared by window, layer, and region targets.

## Coordinate spaces

HyprFluidGlass distinguishes three coordinate spaces:

| Space | Unit | Origin |
|---|---|---|
| Client geometry | Logical pixels | Target surface or selected output |
| Compositor geometry | Global logical pixels | Hyprland layout origin |
| Render geometry | Output-buffer pixels | Output framebuffer origin |

A target is defined in exactly one client geometry space. It is resolved to
global logical geometry, clipped per output, then mapped once into that output's
render space.

Material dimensions such as radius, blur reach, bevel width, and refraction
reach are logical design pixels. They are scaled exactly once for the output
where the presentation is drawn.

## Output identity and generations

The stable public output identity is the connector name reported by Hyprland,
for example `DP-1`.

Render resources additionally use an internal output generation. A new
generation is created when any render-relevant property changes:

- output object replacement or hotplug;
- mode identity or buffer size;
- logical position or size;
- scale;
- transform;
- render format or color-management state.

Capture textures, framebuffers, cached geometry, and readiness belong to one
generation. Retiring a generation releases its resources and detaches its
presentations before a replacement is considered capture-ready.

Generations increase independently for each connector name and are not reused
when an output is removed and later re-added. The internal output snapshot
contains an adapter-provided output-object token and mode token in addition to
the public connector name, so a replacement object or same-size mode change
cannot accidentally inherit old GPU resources.

## Fractional scaling

Logical target edges are converted to buffer coverage with an
outside-preserving rule:

```text
left   = floor(mapped left edge)
top    = floor(mapped top edge)
right  = ceil(mapped right edge)
bottom = ceil(mapped bottom edge)
```

This prevents a fractional-scale target from losing its outermost pixel row or
column. Shader-local coordinates retain floating-point precision; the coverage
rectangle is integer only where the renderer requires an integer damage or
scissor region.

Repeated mapping must be stable. Position, size, radius, capture bounds, damage,
and shader sampling derive from the same unrounded logical geometry rather than
feeding rounded values back into later calculations.

## Output transforms

All eight Wayland output transforms are supported:

- normal;
- 90 degrees;
- 180 degrees;
- 270 degrees;
- flipped;
- flipped 90 degrees;
- flipped 180 degrees;
- flipped 270 degrees.

Wayland defines the transform from framebuffer space to output-oriented space.
Client geometry starts in output-oriented logical space, so buffer coverage
uses the inverse transform. Geometry and backdrop UVs use that one
authoritative path. A transform is not applied manually to UVs when the
compositor projection has already applied the equivalent mapping.

The mapping order is:

1. Clip the global logical rectangle to the output's global logical bounds.
2. Subtract the output origin.
3. Apply the output scale once.
4. Apply the inverse Wayland output transform.
5. Clamp to the framebuffer and derive outside-preserving integer coverage.

The mapper retains the transformed positions of the target's semantic
top-left, top-right, bottom-right, and bottom-left corners. Rotation or
reflection therefore cannot silently exchange corner radii or lighting
orientation.

For every transform:

- the target's logical corners map to the same framebuffer coverage used for
  capture;
- the shader samples the corresponding backdrop pixels;
- rounded corners stay attached to their semantic target corners;
- pointer-relative lighting uses the same transformed presentation geometry.

## Targets spanning outputs

A resolved target may produce more than one presentation. Each presentation is
keyed by:

```text
target identity + output generation + render stage
```

The target is clipped in global logical space before each output mapping.
Capture and readiness are independent per presentation. One output cannot reuse
another output's scale, transform, capture texture, or draw acknowledgement.

## Window attachments

Window glass follows the compositor's authoritative window geometry and render
modifiers. The same effective geometry drives:

- decoration extents;
- output intersection;
- backdrop sampling;
- the shape mask;
- damage.

Animated movement, workspace transitions, transforms, and other render
modifiers cannot update only the drawn box while leaving backdrop UVs mapped to
the unmodified position.

Application glass is attached below its exact window as an
`IHyprWindowDecoration` in `DECORATION_LAYER_UNDER`. It is removed when the
identity guard no longer matches, the window is unmapped, or the owner expires.

## Layer attachments

Layer geometry uses one coherent compositor state for both position and size.
During animations, the attachment cannot combine an animated position with a
stale static size.

The layer namespace is exact and non-empty. Unmapping or destroying the layer
detaches its presentations before any cached surface state is released.

Layer popups are separate compositor presentation objects. They are not
silently treated as part of the parent layer target. A client that needs glass
on a popup provides a distinct target for the popup's stable identity.

## Region attachments

Region targets use monitor-local logical geometry. A region is invalid when:

- its selected output does not exist;
- its width or height is not finite and greater than zero;
- any coordinate is non-finite;
- its requested render stage is unavailable.

Off-output portions are clipped. A fully clipped region is resolved but not
drawn and reports an appropriate readiness state.

## Capture bounds

Backdrop capture includes the target plus the maximum sampling apron required
by its material:

- blur kernel reach;
- refraction displacement;
- chromatic offset;
- bevel and rim antialiasing.

The apron is clipped to the output buffer. Capture sizing uses checked
arithmetic and is rejected before allocation if it exceeds reported resource
limits.

Capture compatibility is exact:

```text
output connector + output generation + render stage + render format + color state
+ exact window object for a pre-window capture
```

Targets with compatible overlapping bounds share the smallest useful capture
union. Distant targets remain separate when merging them would exceed the
adapter's width, height, pixel, or byte limits. A thin target therefore does
not require allocating or shading a full-output intermediate when bounded
capture is sufficient.

`pre-window` is object-relative rather than output-global. Its capture identity
includes the exact internal window token, so glass below one application cannot
reuse backdrop captured at another application's position in the window stack.
Region targets cannot request `pre-window` because they do not identify a
window.

## Render hooks

The v2 stages map to Hyprland's render hooks as follows:

| v2 stage | Hyprland hook |
|---|---|
| `post-wallpaper` | `RENDER_POST_WALLPAPER` |
| `pre-window` | `RENDER_PRE_WINDOW` for the exact attached window |
| `post-windows` | `RENDER_POST_WINDOWS` |
| `post-layer` | `RENDER_LAST_MOMENT` |

Capture scheduling is tracked independently for every output frame and output
generation. If Hyprland emits the same hook more than once for a window pass,
the same capture resource is copied only once in that frame.

The renderer adapter reports bytes per pixel and allocation limits for the
actual DRM format. The planner does not infer a memory layout from a fourcc or
substitute a different format. Capture resources are framebuffer-tested before
use and are indexed by the full compatibility key. Retiring an output
generation returns every resource owned by that generation for immediate
release.

Compositor coverage uses a top-left origin. OpenGL framebuffer copies use a
bottom-left origin. The bounded copy converts that y axis exactly once and
copies into an equally sized texture with nearest sampling; it does not scale
or color-convert the backdrop during capture.

## Damage

Damage includes both the previous and current presentation bounds plus the
material sampling apron. This clears stale pixels after movement, resizing,
detachment, or material changes.

Continuous damage is limited to presentations whose material or backdrop
actually requires it. Static, occluded, disabled, unresolved, and detached
targets do not keep an output repainting.

## Color and HDR

Capture preserves the output render format and color-management path whenever
the renderer supports it. The plugin does not silently force an HDR or
wide-gamut output through an 8-bit `ABGR8888` intermediate.

If a required format cannot be captured or sampled correctly, the presentation
reports `unsupported` or `capture-failed`. It does not claim `drawn` with a
known color-space downgrade.

## GL state

Every render pass saves and restores the exact state it modifies, including:

- draw and read framebuffer bindings;
- viewport;
- scissor state and rectangle;
- blend state and functions;
- active texture unit and texture bindings;
- shader program;
- vertex-array and buffer bindings when used.

The viewport is restored to its previous value, not reconstructed from the
current monitor size.

Hyprland internally caches viewport, capability, and shader selections.
Restoration uses Hyprland's tracked renderer calls for those states. A pass
that changes the shader begins only when the current program can be mapped to
a tracked shader object and restored through the same API. A shader-neutral
framebuffer copy leaves the active program untouched. The plugin does not use
raw `glUseProgram`, `glViewport`, or capability toggles behind Hyprland's
cache.

## Direct scanout

An active glass presentation that needs backdrop capture is incompatible with
direct scanout for that output. The plugin inhibits direct scanout only while
such a presentation is eligible to draw and releases the inhibition promptly
when it becomes hidden, detached, disabled, unsupported, or expired.

## Draw acknowledgement

A presentation reaches `drawn` only after:

1. its target is resolved and attached;
2. compatible capture resources exist;
3. capture succeeds for the current output generation and stage;
4. the shader completes a successful draw;
5. renderer state is restored.

Request acceptance, attachment creation, damage submission, and render-pass
queueing are not draw acknowledgements.
