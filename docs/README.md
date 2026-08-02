# HyprFluidGlass documentation

HyprFluidGlass is a Hyprland compositor plugin for rendering fluid-glass
materials behind application windows, layer-shell surfaces, and compositor
regions.

## Current release

The current plugin interface and build instructions are documented in the
project [README](../README.md).

## V2 reference

The v2 control plane, renderer, and Lua configuration entry point are active.
Shell and integration authors should use the versioned interface:

- [Architecture](architecture/v2-overview.md)
- [Target resolution](architecture/target-resolution.md)
- [Rendering and geometry](architecture/rendering-contract.md)
- [Lua configuration](reference/lua-configuration.md)
- [Runtime protocol](reference/runtime-protocol.md)

Check `capabilities.rendering_ready` before expecting a v2 target to draw. A
`false` value means the render path has not initialized successfully. Inspect
the structured renderer error returned by `status` before publishing targets.

## Integration guides

Framework names are client conventions and do not receive special behavior
inside the plugin:

- [Generic client](guides/generic-client.md)
- [AGS](guides/ags.md)
- [Quickshell](guides/quickshell.md)

The guides cover the active v2 control plane and clearly gate rendering on the
reported `rendering_ready` capability.

## Project boundary

HyprFluidGlass provides compositor rendering and attachment lifecycle. It does
not:

- implement a desktop shell;
- generate GTK or application themes;
- host or distribute shell add-ons;
- monitor whether applications are responding;
- provide a security sandbox between processes running as the same user.
