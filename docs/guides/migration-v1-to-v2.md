# Migrate a client from v1 to v2

The v1 commands remain available during the deprecation window. New
integrations should use v2, and existing integrations can migrate one client
at a time without changing unrelated users of the plugin.

## Command mapping

| V1 | V2 |
|---|---|
| `hyprfluidglass-apply-json` | `session.open` followed by `session.replace` |
| `hyprfluidglass-clear` | `session.close`, or replace with no targets |
| one global element set | one isolated, leased session per client |
| `monitor` + `x/y/w/h` | region selector + output-logical geometry |
| `anchorWindow` regex | exact window selector with address and identity guard |
| layer bind selector | exact layer namespace selector |
| inline material fields | named session or durable config material |

## Migration sequence

1. Query v2 `capabilities` and require `rendering_ready: true`.
2. Keep the v1 surface opaque until the v2 target reports `drawn` through
   `target.inspect`.
3. Open one v2 client session and retain its session ID, token, generation, and
   expiry in process memory.
4. Convert the complete v1 element set into named materials and v2 targets.
5. Submit generation `1` with `session.replace`.
6. Stop publishing the migrated elements through the v1 command only after the
   v2 generation is accepted and drawn.
7. Heartbeat before the lease expires and increment the generation for each
   later complete replacement.
8. Close the v2 session during orderly shutdown.

Do not persist the session token or include it in diagnostics. After a plugin
reload or process restart, open a new session instead of attempting to reuse
the old token.

## Region example

This v1 element:

```json
{
  "id": "bar",
  "monitor": "DP-1",
  "x": 16,
  "y": 12,
  "w": 1888,
  "h": 44,
  "radius": 22,
  "glassLevel": 0.5
}
```

becomes a v2 target:

```json
{
  "id": "bar",
  "kind": "region",
  "selector": {"output": "DP-1"},
  "geometry": {
    "space": "output-logical",
    "x": 16,
    "y": 12,
    "width": 1888,
    "height": 44
  },
  "stage": "post-windows",
  "material": {"source": "session", "name": "shell"},
  "shape": {"kind": "rounded-rect", "radius": 22}
}
```

The replacement must also include the named `shell` material. See the
[generic client guide](generic-client.md) for a complete open, replace,
heartbeat, and close flow.

## Application glass

Use a `window` target for glass below an application. V2 requires the exact
Hyprland window address plus a PID, initial class, or both. The compositor
attaches an under-window decoration to that exact window; a broad class or
title regex is no longer accepted as sole runtime identity.

Durable application rules belong in
[Lua configuration](../reference/lua-configuration.md). Runtime window targets
are appropriate when a trusted local controller already owns an exact window
identity.

## Layer-shell glass

Give every simultaneously mapped layer surface a unique, stable namespace and
use that exact namespace in a `layer` target. Quickshell and AGS names are
client conventions only; they do not select a privileged plugin path.

## Failure behavior

If `rendering_ready` becomes false or target readiness leaves `drawn`, restore
the client's opaque fallback. A client must never make its own content
transparent merely because `session.replace` returned success; acceptance,
capture readiness, and a confirmed draw are distinct states.
