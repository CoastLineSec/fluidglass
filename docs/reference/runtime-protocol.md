# Runtime protocol reference

Availability: the v2 control plane and renderer are active. Clients must still
check `rendering_ready` before expecting submitted targets to draw.

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
    "transitions": {
      "targets": true,
      "compound_parts": true
    },
    "presentation_handoffs": {
      "retain_until_drawn": true,
      "target_kinds": ["layer"],
      "geometry_morph": {
        "layer_targets": true,
        "coordinate_space": "surface-local",
        "coordinate_spaces": ["surface-local", "output-local"],
        "shapes": ["rounded-rect-uniform-radius"],
        "easings": ["ease-out-cubic"],
        "anchor": "compositor-monotonic",
        "reversal": true,
        "max_active_per_target": 1,
        "max_active": 512
      }
    },
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
      "materials_per_owner": 128,
      "compound_parts": 32,
      "compound_connectors": 32,
      "bezier_segments": 16,
      "transition_ms": 60000,
      "presentation_handoff_ms": 2000,
      "presentation_morph_ms": 1000
    }
  }
}
```

Clients must query capabilities instead of inferring feature support from the
plugin build version. `rendering_ready` becomes `true` after the compositor
render path has initialized. A `false` value indicates startup or runtime
failure, not permission to assume that retained targets are visible.

## Status

Request:

```json
{
  "version": 2,
  "operation": "status"
}
```

The `renderer` object reports live render-path state without exposing session
tokens:

```json
{
  "state": "active",
  "rendering_ready": true,
  "presentations": 2,
  "capture_resources": 1,
  "draws": 2,
  "window_attachments": 0,
  "direct_scanout_leases": 1,
  "last_error": null
}
```

`state` is `inactive`, `active`, or `failed`. When it is `failed`,
`last_error` contains a structured `code`, `path`, and sanitized `message`.
Counts describe the last reconciled scene and are diagnostic rather than an
ownership API.

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

A client may request bounded continuity for a currently drawn layer target while
submitting its successor geometry:

```json
{
  "handoffs": [
    {
      "target_id": "primary-bar",
      "source_generation": 1,
      "mode": "retain-until-drawn",
      "timeout_ms": 750
    }
  ]
}
```

Clients must send `handoffs` only when
`presentation_handoffs.retain_until_drawn` is advertised and the target kind is
listed. Every requested predecessor must be drawn in the exact current
generation and the successor replacement must retain its target id and layer
namespace. A failed handoff request rejects the replacement and preserves the
current generation. A successful replacement reports accepted handoffs
separately from target readiness.

When `presentation_handoffs.geometry_morph` advertises compatible layer,
coordinate-space, shape, easing, and anchor support, a handoff may also request
one compositor-timed geometry morph:

```json
{
  "handoffs": [
    {
      "target_id": "primary-bar",
      "source_generation": 1,
      "mode": "retain-until-drawn",
      "timeout_ms": 750,
      "morph": {
        "transition_id": "primary-bar-attach-1",
        "duration_ms": 240,
        "easing": "ease-out-cubic",
        "anchor": "compositor-monotonic"
      }
    }
  ]
}
```

The morph contract supports surface-local rectangles and capability-gated
output-local rectangles with one uniform rounded-rectangle radius. An
output-local request includes exact `source` and `destination` objects, each
containing a `rect` and `radius`, and sets
`"coordinate_space": "output-local"`. The endpoint sizes and radii must match
the source and successor targets.

The response reports the accepted transition id,
compositor-monotonic anchor, authoritative source and destination endpoints,
effective duration, easing, coordinate space, and `active`, `settling`,
`completed`, or `failed` state. Output-local geometry remains authoritative
while `settling` until the live layer attachment reaches its destination. A
replacement can reverse an active morph; the accepted source is the
compositor-visible geometry at replacement time. Unchanged targets may retain
compatible active motion across a complete generation replacement.

Morph settlement does not imply readiness. The successor remains the only
authoritative generation and must independently reach `drawn`. Retained
presentation fallback ends after the successor's first successful draw.
Timeout, session loss, target removal, surface replacement, or output
invalidation cancels the morph.

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
  "base": {
    "radius": 28
  },
  "cutout": {
    "x": 32,
    "y": 32,
    "width": 796,
    "height": 656,
    "corner_radii": {
      "top_left": 20,
      "top_right": 20,
      "bottom_right": 16,
      "bottom_left": 16
    }
  },
  "parts": [
    {
      "x": 0,
      "y": 0,
      "width": 860,
      "height": 44,
      "corner_radii": {
        "top_left": 22,
        "top_right": 22,
        "bottom_right": 8,
        "bottom_left": 8
      },
      "junctions": {
        "top_left": 0,
        "top_right": 7,
        "bottom_right": 7,
        "bottom_left": 0
      },
      "material_extent": {
        "x": -8,
        "y": -8,
        "width": 876,
        "height": 60
      },
      "opacity": 0.75
    }
  ],
  "connectors": [
    {
      "x": 16,
      "y": 40,
      "width": 12,
      "height": 12
    }
  ],
  "connector_curve": 7
}
```

A compound is a bounded assembly in target-local logical coordinates:

- `base` optionally fills the target geometry.
- `cutout` subtracts one rounded rectangle from the base and is valid only
  when `base` is present.
- `parts` adds independently rounded rectangles.
- `connectors` adds bridging rectangles to the combined mask.
- `connector_curve` is a finite, non-negative curve value used where joined
  pieces meet.

Every rounded element accepts either a uniform `radius` or a `corner_radii`
object, never both. A `corner_radii` object contains all four fields:
`top_left`, `top_right`, `bottom_right`, and `bottom_left`.

Each part may also declare:

- `junctions`: four non-negative corner values for joined-edge fillets.
- `material_extent`: the target-local rectangle used for that part's material
  sampling bounds. It defaults to the part rectangle.
- `opacity`: a finite value from `0` to `1`, defaulting to `1`.

A compound must contain a base or at least one part. Cutouts, parts, material
extents, and connectors require positive sizes. Part and connector limits are
reported through `capabilities`.

## Transitions

A target may carry one transition:

```json
{
  "id": "bar-enter-17",
  "phase": "enter",
  "edge": "bottom",
  "duration_ms": 240,
  "elapsed_ms": 40,
  "travel": 44,
  "easing": [
    {
      "control1_x": 0.2,
      "control1_y": 0,
      "control2_x": 0.3,
      "control2_y": 1,
      "end_x": 1,
      "end_y": 1
    }
  ]
}
```

`id` identifies one motion event. Replacing a session with the same transition
ID updates the target without restarting that event. A new ID starts a new
event even when `phase` is unchanged. On a successful replacement, the plugin
anchors the supplied `elapsed_ms` to compositor monotonic time and advances it
until `duration_ms` is reached. Repeating the same ID cannot move that live
elapsed time backward.

Transition fields:

- `phase`: `enter` or `exit`.
- `edge`: `top`, `bottom`, `left`, or `right`.
- `duration_ms`: a positive integer no greater than the advertised
  `transition_ms` limit.
- `elapsed_ms`: a non-negative integer no greater than `duration_ms`. It lets a
  client and the compositor begin from the same point in an already-running
  animation.
- `travel`: a finite, non-negative logical-pixel distance.
- `easing`: a bounded array of cubic Bezier segments. An empty array is linear.

Each Bezier segment begins at the previous segment's endpoint, or `(0, 0)` for
the first segment. Control-point x values stay inside that segment's x range,
endpoint x values advance monotonically, and the final segment ends at
`(1, 1)`.

A compound part accepts the same object under its `transition` field and adds
`protrusion`, the maximum logical-pixel length that may collapse during the
part transition. When omitted, `protrusion` defaults to `travel`.

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
detaches the session and removes its target-readiness records.

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
state, presentation keys, readiness, the optional `handoff` state, and the
latest sanitized failure. Handoff state is `retained`, `completed`, or
`failed`; a retained predecessor never counts as current-generation `drawn`.
When a geometry morph was accepted, the handoff also reports its endpoints,
anchor, duration, easing, and state. Aggregate `status` output reports
presentation morph counts separately from handoff and readiness counts.

## Readiness inspection

Readiness is available through `target.inspect`. The current v2 runtime does
not post target-readiness notifications to Hyprland's event socket, so clients
use bounded inspection while synchronization is incomplete.

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

`drawn` is recorded only after the render pass completes a successful draw for
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
