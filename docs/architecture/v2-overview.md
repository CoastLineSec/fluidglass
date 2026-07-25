# HyprFluidGlass v2 architecture

This document defines the public architectural boundary for HyprFluidGlass v2.
See the release documentation for the features available in a particular
version.

## Purpose

HyprFluidGlass is a generic Hyprland compositor plugin for drawing fluid-glass
materials over compositor-owned content. It accepts durable configuration and
temporary runtime targets, resolves those targets to compositor objects, and
renders the selected material at the correct render stage.

The plugin does not implement a desktop shell. It contains no HyprGlassShell,
Quickshell, AGS, or HTMShell product logic. Those clients shape and identify
their surfaces, then use the same public plugin contract as any other client.

The separation is deliberate:

- HyprFluidGlass owns compositor integration, target resolution, capture,
  rendering, readiness, and render diagnostics.
- Shells and applications own UI structure, interaction, settings UX, and
  policy about which of their surfaces should use glass.
- A separate appearance service may own GTK theme generation and application
  policy. The plugin only renders the requested compositor attachment.

## Supported target model

Every target has one of three kinds:

| Kind | Attachment | Intended use |
|---|---|---|
| `window` | A decoration below one exact Hyprland window | Application glass |
| `layer` | A uniquely named layer-shell surface | Bars, docks, launchers, widgets |
| `region` | A monitor-local logical rectangle at a declared render stage | Previews and specialized compositor regions |

These are compositor concepts, not product concepts. The plugin does not know
whether a layer surface is a bar, dock, widget, or notification center.

### Window targets

A runtime window target resolves to an exact window address and includes an
identity guard such as the process id and initial class. The guard prevents an
expired address from silently attaching to a different window after address
reuse.

Durable window rules may use exact selectors or explicitly declared regular
expressions. An empty selector is invalid. Regular expressions are bounded,
compiled when configuration is accepted, and never compiled in a render path.

Application glass is rendered as an `IHyprWindowDecoration` in
`DECORATION_LAYER_UNDER`. Transparent areas in an application theme reveal the
glass attached to that same window. The plugin does not rewrite application
CSS and does not create hidden shell windows behind applications.

### Layer targets

A layer target resolves through a stable, unique layer-shell namespace. Shell
authors should make namespaces unique per logical surface and session. The
plugin does not reserve framework-specific prefixes.

Examples of client-chosen namespaces:

```text
quickshell:my-shell:bar:primary
ags:example-shell:dock:DP-1
htmshell:demo:launcher
```

These examples are client conventions only. They are not special-cased by the
plugin.

### Region targets

A region target declares:

- the output identity;
- a monitor-local logical rectangle;
- the render stage;
- a material reference.

Region targets are lower-level than window and layer targets. They are useful
for configuration previews and compositor-owned regions, but should not replace
a stable window or layer attachment when one exists.

## Three configuration authorities

V2 has three explicit authorities:

| Authority | Lifetime | Purpose |
|---|---|---|
| `config` | Durable across reloads | Lua-defined materials and rules |
| `client:<client-id>:<session>` | Leased runtime session | Shell/application runtime targets |
| `preview:<client-id>:<session>` | Short leased runtime session | Temporary configuration previews |

An owner can replace only its own state. One client cannot clear or replace
another client's targets, and a runtime session cannot overwrite durable
configuration.

This is collision ownership, not a security boundary. Processes running as the
same user and able to invoke Hyprland IPC must still be treated as mutually
trusted. Opaque session tokens prevent accidental cross-client mutation; they
do not defend against a hostile process with the same user privileges.

## Durable Lua configuration

The native Hyprland Lua entry point is:

```text
hl.plugin.hyprfluidglass.configure
```

It accepts one complete, versioned snapshot:

```lua
hl.plugin.hyprfluidglass.configure({
    version = 2,
    enabled = true,
    default_material = "fluid",
    materials = {
        fluid = {
            glass_level = 0.5,
            tint_enabled = false,
        },
    },
    window_rules = {},
    layer_rules = {},
})
```

Configuration is transactional:

1. `preReload` starts an empty staging transaction.
2. Lua calls populate and validate the staging snapshot.
3. `reloaded` commits the complete snapshot only when it is valid.
4. An invalid or incomplete reload preserves the last known-good snapshot.

The Lua API does not incrementally mutate live render state while Hyprland is
parsing configuration.

## Runtime protocol

The versioned runtime API is exposed through one JSON command:

```text
hyprctl -j hyprfluidglass '<request-json>'
```

The v2 operations are:

- `capabilities`
- `status`
- `session.open`
- `session.replace`
- `session.heartbeat`
- `session.close`
- `target.inspect`

`session.replace` submits a complete generation for one session. Validation and
resolution are separate: a structurally valid target may be accepted while the
corresponding compositor object is not mapped yet.

Session properties:

- an opaque ownership token;
- a monotonically increasing generation;
- a bounded lease renewed by heartbeat;
- atomic full replacement;
- automatic cleanup on close or lease expiry.

## Resource limits

The initial v2 limits are conservative and must be enforced before allocation:

| Resource | Initial limit |
|---|---:|
| Request body | 256 KiB |
| Identifier, material, or client id | 128 bytes |
| Regular-expression source | 256 bytes |
| Concurrent runtime sessions | 64 |
| Targets per session | 256 |
| Total dynamic targets | 512 |
| Materials per owner | 128 |

All numeric input must be finite and inside the field's documented range.
Conversion from JSON or Lua numbers must be checked before narrowing to an
integer or compositor scalar.

Limits are part of the reported capability contract so clients can adapt
without guessing.

## Core state model

V2 keeps definition, attachment, presentation, and readiness separate.

### TargetDefinition

Validated intent owned by `config`, a runtime client, or a preview session. It
contains identity, target kind, selector, shape, material reference, and
target-specific options.

### ResolvedAttachment

The currently resolved Hyprland object and the identity evidence used to
resolve it. Attachments can disappear and reappear without destroying the
definition.

### PresentationState

Render resources and geometry for one attachment, one output generation, and
one render stage. A target spanning outputs can therefore have independent
capture and draw state on each output.

### Readiness

Readiness advances through observable stages:

```text
accepted -> resolved -> attached -> capture-ready -> drawn
```

The error states are:

```text
invalid
unresolved
unsupported
capture-failed
shader-failed
resource-limited
expired
detached
```

`drawn` is recorded only from the render pass after a successful GPU draw. A
request handler cannot claim that a target has drawn merely because it was
accepted.

## Material and shape model

Materials are named, validated values independent of target identity. A target
references a material and may use only the documented, bounded overrides.

V2 preserves the established fluid-glass appearance while replacing the
surrounding lifecycle and API.

The initial shape vocabulary is generic:

- `rounded-rect`
- `ring`
- `compound`

There are no shell-specific shape names in the plugin.

## Rendering pipeline

The intended render pipeline is:

1. Resolve the target to a window, layer surface, or output region.
2. Derive monitor-local logical geometry and a presentation key.
3. Map geometry to the output's render space.
4. Capture or reuse the required backdrop for the declared render stage.
5. Execute the fluid-glass shader inside the target shape.
6. Restore every modified GL and renderer state value.
7. Record draw readiness and diagnostics.

Capture resources are owned by an output generation, not just by an output
name or pointer. Hotplug, mode changes, scale changes, and transform changes
invalidate the generation and retire the old resources.

## Compatibility

The current v1 dispatchers remain temporarily available through a compatibility
adapter. The adapter translates v1 payloads into a reserved v2 owner and uses
the same v2 model and renderer. It is not a second rendering implementation.

The adapter will be removed only after:

- Quickshell compatibility is proven through v2;
- AGS and generic-client examples are available;
- migration instructions are published;
- the deprecation window is announced.

## Diagnostics and privacy

Diagnostics report:

- plugin and protocol versions;
- Hyprland compatibility information;
- active owners, generations, leases, and target counts;
- readiness and error states;
- resource-limit usage;
- sanitized render failures.

Diagnostics must not collect unrelated process liveness, window titles,
keystrokes, notification content, or user activity. Application-not-responding
monitoring is outside this compositor plugin.

Debug reports redact or omit sensitive selectors and application identity by
default. A user must explicitly request verbose identity data.

## Deliberate non-goals

V2 does not:

- host or distribute shell add-ons;
- implement a desktop shell;
- configure GTK themes;
- provide application-not-responding monitoring;
- create framework-specific APIs;
- claim same-user IPC is a hostile-process sandbox;
- silently apply visual changes during the architectural rewrite;
- guarantee untested Hyprland ABI compatibility.

## Hyprland compatibility

Hyprland plugins are ABI-coupled to Hyprland. HyprFluidGlass supports explicitly
tested Hyprland release pins. New releases are added after build and live
validation; support is not inferred merely because the source compiles.

