# AGS integration

AGS clients use the standard HyprFluidGlass JSON protocol. The plugin does not
inspect AGS, GTK, or Astal internals.

Current AGS releases provide asynchronous process helpers through
`ags/process`. Use `execAsync`; the synchronous helper blocks the shell event
loop.

```ts
import { execAsync } from "ags/process"

type HfgResponse<T> = {
  ok: boolean
  version: 2
  request_id?: string
  result?: T
  error?: {
    code: string
    path: string
    message: string
  }
}

export async function hfgRequest<T>(
  request: Record<string, unknown>,
): Promise<T> {
  const output = await execAsync([
    "hyprctl",
    "-j",
    "hyprfluidglass",
    JSON.stringify({ ...request, version: 2 }),
  ])
  const response = JSON.parse(output) as HfgResponse<T>
  if (!response.ok || response.result === undefined) {
    const error = response.error
    throw new Error(
      `HyprFluidGlass ${error?.code ?? "error"} at ` +
      `${error?.path ?? ""}: ${error?.message ?? "request failed"}`,
    )
  }
  return response.result
}
```

Query the capability contract during startup:

```ts
const capabilities = await hfgRequest<{
  rendering_ready: boolean
  target_kinds: string[]
  shapes: string[]
}>({ operation: "capabilities" })
```

Open one session for the AGS process:

```ts
const session = await hfgRequest<{
  session_id: string
  token: string
  generation: number
  lease_ms: number
}>({
  operation: "session.open",
  client_id: "example-ags-shell",
  mode: "client",
})
```

Keep `session_id`, `token`, and the current generation in process memory. Send
complete `session.replace` requests when AGS surface state changes, and schedule
`session.heartbeat` comfortably before `lease_ms` elapses. Close the session
when the application shuts down.

For a layer target, set a unique stable namespace on the corresponding
layer-shell window and submit that exact value:

```ts
const target = {
  id: "primary-bar",
  kind: "layer",
  selector: { namespace: "example-ags-shell:bar:DP-1" },
  geometry: {
    space: "surface-local",
    x: 0,
    y: 0,
    width: 1888,
    height: 44,
  },
  material: { source: "session", name: "shell" },
  shape: { kind: "rounded-rect", radius: 22 },
}
```

Namespaces are client identity, not plugin routing. An `ags:` prefix is a useful
human convention but does not activate special behavior.

See the AGS
[process utility reference](https://aylur.github.io/ags/guide/utilities.html)
for `execAsync`, and the
[runtime protocol](../reference/runtime-protocol.md) for session and target
schemas.
