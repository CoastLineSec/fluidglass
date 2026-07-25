# Runtime protocol reference

Availability: planned for HyprFluidGlass v2; not available in the current
release.

The runtime protocol lets shells and other local clients create temporary
glass targets without editing Hyprland configuration. It is generic: every
client uses the same versioned JSON request format.

## Transport

Requests use one Hyprland dispatcher:

```sh
hyprctl -j hyprfluidglass '<request-json>'
```

The JSON flag is recommended. Every response is a single JSON object even when
the caller omits `-j`; the flag prevents Hyprland tooling from applying
human-readable formatting around it.

Every request contains:

```json
{
  "version": 2,
  "operation": "capabilities",
  "request_id": "example-1"
}
```

| Field | Type | Required | Meaning |
|---|---|---:|---|
| `version` | integer | yes | Must be exactly `2` |
| `operation` | string | yes | Operation name |
| `request_id` | string | no | Opaque client correlation id |

Unknown request fields are rejected unless the operation explicitly declares
them. Duplicate object fields are rejected instead of allowing one value to
silently replace another. The complete request body is limited to 256 KiB and
JSON nesting is limited to 64 levels.

## Response envelope

Success:

```json
{
  "ok": true,
  "version": 2,
  "request_id": "example-1",
  "result": {}
}
```

Failure:

```json
{
  "ok": false,
  "version": 2,
  "request_id": "example-1",
  "error": {
    "code": "invalid-request",
    "path": "targets[0].geometry.width",
    "message": "expected a finite number greater than zero"
  }
}
```

Requests are total at the plugin boundary: malformed JSON, wrong types,
overflowing numbers, invalid UTF-8, and unexpected exceptions produce an error
response instead of escaping into a compositor callback.

## Capabilities

Request:

```json
{
  "version": 2,
  "operation": "capabilities"
}
```

Response fields include:

```json
{
  "ok": true,
  "version": 2,
  "result": {
    "protocol_versions": [2],
    "rendering_ready": true,
    "target_kinds": ["window", "layer", "region"],
    "shapes": ["rounded-rect", "ring", "compound"],
    "operations": [
      "capabilities",
      "status",
      "session.open",
      "session.replace",
      "session.heartbeat",
      "session.close",
      "target.inspect"
    ],
    "limits": {
      "request_bytes": 262144,
      "json_nesting": 64,
      "identifier_bytes": 128,
      "regex_bytes": 256,
      "sessions": 64,
      "targets_per_session": 256,
      "dynamic_targets": 512,
      "materials_per_owner": 128
    }
  }
}
```

Clients must query capabilities instead of inferring feature support from the
plugin build version. `rendering_ready` is `false` while the v2 control plane is
available but the v2 renderer is not accepting live presentations.

## Opening a session

```json
{
  "version": 2,
  "operation": "session.open",
  "client_id": "example-shell",
  "mode": "client"
}
```

`mode` is `client` or `preview`. A preview session has a shorter maximum lease
and exists for configuration UI previews, not normal shell operation.

Successful result:

```json
{
  "session_id": "opaque-session-id",
  "token": "opaque-owner-token",
  "generation": 0,
  "lease_ms": 15000,
  "expires_at_ms": 123456789
}
```

The returned owner is internally represented as:

```text
client:<client-id>:<session-id>
```

or:

```text
preview:<client-id>:<session-id>
```

The `token` is required for every later mutation. It prevents accidental
cross-client replacement, but is not a sandbox against a malicious process
running as the same user.

## Replacing session state

`session.replace` atomically replaces every material and target owned by the
session.

```json
{
  "version": 2,
  "operation": "session.replace",
  "session_id": "opaque-session-id",
  "token": "opaque-owner-token",
  "generation": 1,
  "materials": {
    "bar": {
      "glass_level": 0.45,
      "tint_enabled": true,
      "tint_color": "#DCEBFF"
    }
  },
  "targets": []
}
```

Rules:

- `generation` must be exactly the current generation plus one;
- `materials` and `targets` are complete replacements, not patches;
- the entire request is validated before live state changes;
- a failure preserves the previous generation byte-for-byte;
- replayed, skipped, or stale generations are rejected;
- a successful empty replacement clears only this session's state.

Material fields use the same schema as the
[Lua configuration](lua-configuration.md#material-definitions).

## Material references

Every target uses an explicit material authority:

```json
{
  "source": "session",
  "name": "bar"
}
```

or:

```json
{
  "source": "config",
  "name": "fluid"
}
```

`session` resolves inside the calling session's replacement. `config` resolves
inside the last known-good durable Lua snapshot. There is no implicit fallback
between authorities.

## Common target fields

```json
{
  "id": "primary-bar",
  "kind": "layer",
  "material": {
    "source": "session",
    "name": "bar"
  },
  "shape": {
    "kind": "rounded-rect",
    "radius": 18
  }
}
```

| Field | Type | Required | Meaning |
|---|---|---:|---|
| `id` | string | yes | Stable identity within this session |
| `kind` | string | yes | `window`, `layer`, or `region` |
| `material` | object | yes | Explicit material reference |
| `shape` | object | yes | Validated shape |
| `enabled` | boolean | no | Defaults to `true` |

Target ids are limited to 128 bytes and unique within the replacement.

## Layer targets

A layer target binds to an exact layer-shell namespace:

```json
{
  "id": "primary-bar",
  "kind": "layer",
  "selector": {
    "namespace": "example-shell:bar:primary"
  },
  "geometry": {
    "space": "surface-local",
    "x": 0,
    "y": 0,
    "width": 1888,
    "height": 44
  },
  "material": {
    "source": "session",
    "name": "bar"
  },
  "shape": {
    "kind": "rounded-rect",
    "radius": 22
  }
}
```

The namespace is an exact, non-empty string. Runtime targets do not accept a
namespace regular expression.

`geometry.space` is `surface-local`. Geometry is expressed in logical pixels
relative to the mapped layer surface. Omitting `geometry` selects the entire
surface.

## Window targets

A runtime window target binds to one exact Hyprland window:

```json
{
  "id": "file-manager",
  "kind": "window",
  "selector": {
    "address": "0x12345678",
    "pid": 4242,
    "initial_class": "org.gnome.Nautilus"
  },
  "material": {
    "source": "config",
    "name": "fluid"
  },
  "shape": {
    "kind": "rounded-rect",
    "radius": 12
  }
}
```

`address` and at least one identity guard are required. The plugin verifies the
guard before attaching and whenever the address is re-resolved. A mismatch
makes the target `unresolved`; it never falls back to a broad search.

Window glass is attached below the exact window. It follows the compositor's
window geometry and is visible through transparent application regions.

## Region targets

```json
{
  "id": "material-preview",
  "kind": "region",
  "selector": {
    "output": "DP-1"
  },
  "geometry": {
    "space": "output-logical",
    "x": 100,
    "y": 100,
    "width": 480,
    "height": 320
  },
  "stage": "post-windows",
  "material": {
    "source": "session",
    "name": "preview"
  },
  "shape": {
    "kind": "rounded-rect",
    "radius": 24
  }
}
```

The output name is exact. Geometry uses monitor-local logical pixels with a
top-left origin. Supported stage names are reported by `capabilities`; a client
must not assume an unavailable stage.

## Shapes

### Rounded rectangle

```json
{
  "kind": "rounded-rect",
  "radius": 20
}
```

`radius` is a finite, non-negative logical-pixel value and is clamped to the
resolved rectangle's geometric maximum at presentation time.

### Ring

```json
{
  "kind": "ring",
  "outer_radius": 24,
  "thickness": 3
}
```

The inner shape is derived from `thickness`. Both values must be finite and
non-negative.

### Compound

```json
{
  "kind": "compound",
  "parts": [
    {
      "x": 0,
      "y": 0,
      "width": 600,
      "height": 44,
      "radius": 22
    },
    {
      "x": 608,
      "y": 0,
      "width": 220,
      "height": 44,
      "radius": 22
    }
  ]
}
```

Compound parts are relative to the target geometry. Part count is bounded and
reported through `capabilities`. Empty compounds and non-positive part sizes
are invalid.

## Heartbeats and expiry

```json
{
  "version": 2,
  "operation": "session.heartbeat",
  "session_id": "opaque-session-id",
  "token": "opaque-owner-token",
  "generation": 1
}
```

A heartbeat renews the lease only when the token and current generation match.
It does not alter materials or targets. When a lease expires, the plugin
detaches the session and emits an `expired` readiness event.

Successful result:

```json
{
  "session_id": "opaque-session-id",
  "generation": 1,
  "lease_ms": 15000,
  "expires_at_ms": 123471789
}
```

The ownership token is returned only when the session opens. Heartbeat, status,
replacement, and close responses do not repeat it.

Clients should heartbeat well before the deadline and open a new session after
Hyprland or the plugin reloads.

## Closing a session

```json
{
  "version": 2,
  "operation": "session.close",
  "session_id": "opaque-session-id",
  "token": "opaque-owner-token"
}
```

Close is idempotent for the same valid session token during its short tombstone
window. Closing never affects durable configuration or another owner.

## Status

```json
{
  "version": 2,
  "operation": "status"
}
```

Default status is privacy-preserving. It reports counts, owner ids, generation,
lease state, readiness totals, output resource generations, and sanitized
errors. It does not include window titles or raw selectors.

## Inspecting one target

```json
{
  "version": 2,
  "operation": "target.inspect",
  "session_id": "opaque-session-id",
  "token": "opaque-owner-token",
  "target_id": "primary-bar"
}
```

The session token is required because inspection can expose selector and
attachment information. The response includes definition state, resolution
state, presentation keys, readiness, and the latest sanitized failure.

## Readiness events

The plugin posts Hyprland IPC events with event name:

```text
hyprfluidglass
```

The event data is one compact JSON object:

```json
{
  "version": 2,
  "event": "target.readiness",
  "owner": "client:example-shell:opaque-session-id",
  "generation": 1,
  "target_id": "primary-bar",
  "presentation": {
    "output": "DP-1",
    "output_generation": 7,
    "stage": "post-layer"
  },
  "state": "drawn"
}
```

Clients receive these on Hyprland's event socket using the same mechanism as
other compositor events. Events are notifications, not an authority: a client
reconnects by calling `status` or `target.inspect` after an event-socket
disconnect.

The readiness sequence is:

```text
accepted -> resolved -> attached -> capture-ready -> drawn
```

Failure and terminal states are:

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

`drawn` is emitted only after the render pass completes a successful draw for
that presentation.

## Error codes

The stable initial error codes are:

- `invalid-json`
- `invalid-request`
- `unsupported-version`
- `unsupported-operation`
- `resource-limited`
- `session-not-found`
- `invalid-token`
- `stale-generation`
- `invalid-material`
- `invalid-target`
- `unresolved-target`
- `unsupported-target`
- `internal-error`

New error codes may be added in a compatible protocol revision. Clients should
display the human-readable message and treat unknown codes as a failed request.
