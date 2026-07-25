# Target resolution

HyprFluidGlass resolves durable rules and leased runtime targets through one
target model. Resolution identifies a compositor object; presentation maps that
object to one or more output generations; rendering happens only after both
steps succeed.

Check `capabilities.rendering_ready` before expecting a target to draw. The
resolution contract remains valid while the v2 renderer is unavailable.

## Window identity

A runtime window target contains:

- the exact lower-case Hyprland window address;
- a positive process id, an initial class, or both.

The address selects the current compositor object. The additional evidence
protects against attaching to a different window if an address is reused.
Every mapped window also receives an internal monotonic object token for the
lifetime of the loaded plugin. The token is not a public selector.

Window targets stop resolving when the window is unmapped, fading out, ready
for deletion, or no longer matches its identity evidence.

Durable window rules are evaluated in declaration order. The first matching
rule supplies the material. Each matching live window becomes an independent
config-owned attachment and follows the compositor's current geometry,
opacity, corner radius, and corner power.

Application glass attaches through Hyprland's window-decoration API at
`DECORATION_LAYER_UNDER`. It does not create a companion layer-shell window
and does not infer an application from its position.

## Layer identity

A runtime layer target uses one exact, non-empty layer-shell namespace. A
namespace must identify at most one mapped layer surface. If two live surfaces
publish the same selected namespace, the target is unresolved; the plugin does
not choose the nearest or first surface.

The render stage follows the layer:

| Layer | Render stage |
|---|---|
| `background` | `post-wallpaper` |
| `bottom` | `post-wallpaper` |
| `top` | `post-windows` |
| `overlay` | `post-windows` |

An optional layer target geometry is surface-local and clipped to the live
surface. Omitting it selects the complete surface.

Durable layer rules are evaluated in declaration order. The first match
creates a rectangular full-surface attachment. Shells that need rounded,
ring-shaped, compound, or partial-surface glass should publish an explicit
runtime target with the desired shape.

## Region identity

A region target selects one current output connector, one output-local logical
rectangle, and one render stage. The connector name is exact. More than one
current generation for the same connector is treated as stale state rather
than resolved arbitrarily.

Region geometry is translated into global logical coordinates and then uses
the same per-output mapping path as window and layer targets.

## Authority and precedence

Authorities remain independent:

1. durable `config`;
2. leased `client` sessions;
3. leased `preview` sessions.

For one exact compositor attachment, a client target overrides durable config
and a preview target overrides both. Lower-authority targets remain intact and
become eligible again when the overriding lease ends.

Two targets at the same authority level cannot silently compete for one exact
attachment. Both report an unresolved collision until the owners remove the
conflict.

Window collision identity is the compositor window object. Layer collision
identity includes the surface and selected subregion. Region collision
identity includes the output object, rectangle, and render stage. Distinct
layer subregions and distinct region stages may therefore coexist.

## Failure isolation

Resolution is per target. One missing window, ambiguous namespace, stale
output, or malformed compositor snapshot does not discard valid sibling
targets from the same session. Each failure is retained against its target
identity for readiness and inspection.

Disabled and fully clipped targets are inactive rather than invalid. They do
not allocate capture resources or keep an output repainting.
