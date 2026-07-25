# HyprFluidGlass documentation

HyprFluidGlass is a Hyprland compositor plugin for rendering fluid-glass
materials behind application windows, layer-shell surfaces, and compositor
regions.

## Current release

The current plugin interface and build instructions are documented in the
project [README](../README.md).

## V2 reference

The v2 control plane and Lua configuration entry point are available in current
development builds. The renderer contract is published so shell and
integration authors can prepare against the same versioned interface:

- [Architecture](architecture/v2-overview.md)
- [Rendering and geometry](architecture/rendering-contract.md)
- [Lua configuration](reference/lua-configuration.md)
- [Runtime protocol](reference/runtime-protocol.md)

Check `capabilities.rendering_ready` before expecting a v2 target to draw. A
`false` value means the control plane can validate and retain state while the
v2 renderer remains inactive.

## Integration guides

Quickshell, AGS, and generic-client guides will use the same runtime protocol.
Framework names are client conventions and do not receive special behavior
inside the plugin.

Integration guides will be added when the v2 renderer is available for live
testing.

## Project boundary

HyprFluidGlass provides compositor rendering and attachment lifecycle. It does
not:

- implement a desktop shell;
- generate GTK or application themes;
- host or distribute shell add-ons;
- monitor whether applications are responding;
- provide a security sandbox between processes running as the same user.
