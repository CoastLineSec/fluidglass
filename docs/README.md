# HyprFluidGlass documentation

HyprFluidGlass is a Hyprland compositor plugin for rendering fluid-glass
materials behind application windows, layer-shell surfaces, and compositor
regions.

## Current release

The current plugin interface and build instructions are documented in the
project [README](../README.md).

## V2 reference

The v2 interface is documented before its release so shell and integration
authors can build against a stable contract:

- [Architecture](architecture/v2-overview.md)
- [Rendering and geometry](architecture/rendering-contract.md)
- [Lua configuration](reference/lua-configuration.md)
- [Runtime protocol](reference/runtime-protocol.md)

Pages marked as planned describe the v2 contract and are not available through
the current release.

## Integration guides

Quickshell, AGS, and generic-client guides will use the same runtime protocol.
Framework names are client conventions and do not receive special behavior
inside the plugin.

Integration guides will be added when the v2 runtime API is available for
testing.

## Project boundary

HyprFluidGlass provides compositor rendering and attachment lifecycle. It does
not:

- implement a desktop shell;
- generate GTK or application themes;
- host or distribute shell add-ons;
- monitor whether applications are responding;
- provide a security sandbox between processes running as the same user.

