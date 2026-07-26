# Generic client integration

Any local process that can run `hyprctl` and parse JSON can use the
HyprFluidGlass runtime protocol. Framework-specific integrations use this same
transport and receive no additional authority inside the plugin.

## Lifecycle

A normal client follows this sequence:

1. Query `capabilities`.
2. Open one `client` session.
3. Atomically replace the complete session state.
4. Send a heartbeat before the lease expires.
5. Replace again with `generation + 1` when the state changes.
6. Close the session during an orderly shutdown.

If the process exits without closing, the lease expires and removes its targets.
Never save the returned token to a shared log or world-readable file.

## Request helper

This shell helper uses `jq` only to validate and extract responses. JSON is
passed to `hyprctl` as one argument and is not evaluated by a shell:

```sh
#!/bin/sh
set -eu

hfg_request() {
    payload=$1
    response=$(hyprctl -j hyprfluidglass "$payload")
    if ! printf '%s' "$response" | jq -e '.ok == true' >/dev/null; then
        printf '%s\n' "$response" >&2
        return 1
    fi
    printf '%s\n' "$response"
}

hfg_request '{"version":2,"operation":"capabilities"}'
```

The capabilities result must contain `"rendering_ready": true` before a client
expects v2 targets to draw. A false value means requests can be validated and
retained while the v2 renderer remains inactive.

## Complete region example

Open a session:

```sh
opened=$(hfg_request '{
  "version": 2,
  "operation": "session.open",
  "client_id": "example-shell",
  "mode": "client"
}')

session_id=$(printf '%s' "$opened" | jq -r '.result.session_id')
token=$(printf '%s' "$opened" | jq -r '.result.token')
```

Publish a region target:

```sh
replacement=$(jq -cn \
  --arg session_id "$session_id" \
  --arg token "$token" \
  '{
    version: 2,
    operation: "session.replace",
    session_id: $session_id,
    token: $token,
    generation: 1,
    materials: {
      shell: {
        glass_level: 0.5,
        tint_enabled: true,
        tint_color: "#DCEBFF"
      }
    },
    targets: [{
      id: "primary-bar",
      kind: "region",
      selector: {output: "DP-1"},
      geometry: {
        space: "output-logical",
        x: 16,
        y: 12,
        width: 1888,
        height: 44
      },
      stage: "post-windows",
      material: {source: "session", name: "shell"},
      shape: {kind: "rounded-rect", radius: 22}
    }]
  }')

hfg_request "$replacement"
```

Renew generation `1` before the reported lease expires:

```sh
heartbeat=$(jq -cn \
  --arg session_id "$session_id" \
  --arg token "$token" \
  '{
    version: 2,
    operation: "session.heartbeat",
    session_id: $session_id,
    token: $token,
    generation: 1
  }')

hfg_request "$heartbeat"
```

Close the session:

```sh
close_request=$(jq -cn \
  --arg session_id "$session_id" \
  --arg token "$token" \
  '{
    version: 2,
    operation: "session.close",
    session_id: $session_id,
    token: $token
  }')

hfg_request "$close_request"
```

## Surface identity

Prefer a layer target when the glass belongs to a layer-shell surface. Give
each simultaneously mapped surface a unique, stable namespace and select that
exact namespace in the target. Do not encode secrets in a namespace; it is
compositor-visible metadata, not authentication.

Use a region target for an output-owned preview or a compositor region that is
not attached to a client surface. Use a window target for under-window
application glass. Window targets require the exact Hyprland address plus a
process id, initial class, or both.

## Replacement discipline

`session.replace` is a complete replacement, not a patch:

- retain every material and target that should remain active;
- increment the generation by exactly one;
- do not retry an uncertain mutation with a new generation until its response
  has been inspected;
- after reconnecting, open a new session instead of guessing an old token;
- keep preview surfaces in a `preview` session so they receive the shorter
  lease and higher target precedence.

See the [runtime protocol](../reference/runtime-protocol.md) for the complete
schema.
