# Quickshell integration

Quickshell clients use the standard HyprFluidGlass JSON protocol. The plugin
does not import QML, inspect a Quickshell process, or grant special behavior to
`quickshell:` namespaces.

## Layer namespace

Assign a unique stable namespace to every simultaneously mapped layer surface:

```qml
import Quickshell
import Quickshell.Wayland

PanelWindow {
    WlrLayershell.namespace: "example-quickshell:bar:DP-1"
}
```

Select that exact namespace with a v2 layer target. If multiple live surfaces
publish the same selected namespace, resolution fails instead of choosing one
arbitrarily.

## Running requests

Quickshell provides `Process` and `StdioCollector` in `Quickshell.Io`. Pass the
request JSON as one process argument:

```qml
import Quickshell
import Quickshell.Io

Scope {
    id: root

    property var pendingRequest: ({
        version: 2,
        operation: "capabilities"
    })
    property var lastResponse: null

    Process {
        id: requestProcess
        command: [
            "hyprctl",
            "-j",
            "hyprfluidglass",
            JSON.stringify(root.pendingRequest)
        ]

        stdout: StdioCollector {
            onStreamFinished: {
                const response = JSON.parse(text)
                root.lastResponse = response
                if (!response.ok)
                    console.warn(
                        `HyprFluidGlass ${response.error.code} at ` +
                        `${response.error.path}: ${response.error.message}`)
            }
        }
    }

    Component.onCompleted: requestProcess.running = true
}
```

A production client should wrap this transport in one singleton that:

- serializes mutations so generations cannot race;
- opens one session for the Quickshell process;
- retains the token only in memory;
- publishes complete replacements;
- renews the current generation before the lease expires;
- closes the session during orderly shutdown;
- drops visual assumptions when `rendering_ready` is false.

Do not start one process per widget. Aggregate all glass targets owned by the
shell into one session replacement, then let the plugin share compatible
captures internally.

The Quickshell
[process guide](https://quickshell.outfoxxed.me/docs/guide/introduction/#running-a-process)
documents `Process` and `StdioCollector`. See the
[runtime protocol](../reference/runtime-protocol.md) for the complete request
schema.
