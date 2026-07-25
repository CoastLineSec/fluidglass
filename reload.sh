#!/usr/bin/env bash
# Build and hot-reload HyprFluidGlass in the current Hyprland instance.
#
# This is a development helper. It does not edit shell or user configuration.
set -euo pipefail

case "${1:-}" in
    "")
        requested_state="preserve"
        ;;
    --enable)
        requested_state="on"
        ;;
    --disable)
        requested_state="off"
        ;;
    *)
        printf 'usage: %s [--enable|--disable]\n' "$0" >&2
        exit 2
        ;;
esac

script_dir="$(cd -- "$(dirname -- "$0")" && pwd -P)"
cd -- "$script_dir"

plugin_count() {
    hyprctl plugins list -j 2>/dev/null |
        python3 -c 'import json,sys; print(sum(p.get("name") == "hyprfluidglass" for p in json.load(sys.stdin)))'
}

current_hyprland_pid() {
    hyprctl instances -j 2>/dev/null |
        python3 -c '
import json
import os
import sys

signature = os.environ.get("HYPRLAND_INSTANCE_SIGNATURE", "")
instances = json.load(sys.stdin)
matches = [item for item in instances if item.get("instance") == signature]
if len(matches) != 1:
    raise SystemExit("could not identify the current Hyprland instance")
print(matches[0]["pid"])
'
}

mapped_plugin_paths() {
    local compositor_pid="$1"
    awk '
        /\/hyprfluidglass[^ ]*\.so/ {
            path = $NF
            if (path == "(deleted)")
                path = $(NF - 1)
            if (!seen[path]++)
                print path
        }
    ' "/proc/${compositor_pid}/maps"
}

plugin_enabled() {
    hyprctl -j hyprfluidglass-status 2>/dev/null |
        python3 -c 'import json,sys; print("on" if json.load(sys.stdin).get("enabled") else "off")'
}

wait_until_disabled() {
    local stable=0
    local attempts=0
    while ((attempts < 24)); do
        attempts=$((attempts + 1))
        if hyprctl hyprfluidglass-status 2>/dev/null | grep -q 'enabled=no'; then
            stable=$((stable + 1))
        else
            stable=0
        fi
        if ((stable >= 5)); then
            return 0
        fi
        sleep 0.25
    done
    return 1
}

printf '==> build a uniquely named development artifact\n'
new_plugin="$(make --no-print-directory dev-artifact | tail -n 1)"
if [[ ! -f "$new_plugin" ]]; then
    printf 'build did not produce the reported artifact: %s\n' "$new_plugin" >&2
    exit 1
fi

loaded_before="$(plugin_count)"
previous_state="off"
if ((loaded_before > 0)); then
    previous_state="$(plugin_enabled)"
    printf '==> disable the active material before unloading\n'
    hyprctl hyprfluidglass-material off >/dev/null
    if ! wait_until_disabled; then
        if [[ "$previous_state" == "on" ]]; then
            hyprctl hyprfluidglass-material on >/dev/null 2>&1 || true
        fi
        printf 'plugin did not remain disabled; reload aborted\n' >&2
        exit 1
    fi

    compositor_pid="$(current_hyprland_pid)"
    mapfile -t plugin_paths < <(mapped_plugin_paths "$compositor_pid")
    if ((${#plugin_paths[@]} == 0)); then
        printf 'could not resolve the loaded plugin path; reload aborted\n' >&2
        if [[ "$previous_state" == "on" ]]; then
            hyprctl hyprfluidglass-material on >/dev/null 2>&1 || true
        fi
        exit 1
    fi

    printf '==> unload HyprFluidGlass from the current compositor\n'
    for plugin_path in "${plugin_paths[@]}"; do
        printf '    %s\n' "$plugin_path"
        hyprctl plugin unload "$plugin_path" >/dev/null 2>&1 || true
        if [[ "$(plugin_count)" == "0" ]]; then
            break
        fi
    done
fi

remaining="$(plugin_count)"
if [[ "$remaining" != "0" ]]; then
    printf 'HyprFluidGlass still has %s loaded instance(s); new load aborted\n' "$remaining" >&2
    exit 1
fi

printf '==> load %s\n' "$new_plugin"
hyprctl plugin load "$new_plugin" >/dev/null

case "$requested_state" in
    on)
        final_state="on"
        ;;
    off)
        final_state="off"
        ;;
    preserve)
        if ((loaded_before > 0)); then
            final_state="$previous_state"
        else
            final_state="on"
        fi
        ;;
esac

hyprctl hyprfluidglass-material "$final_state" >/dev/null

printf '==> loaded=%s state=%s\n' "$(plugin_count)" "$final_state"
hyprctl hyprfluidglass-status
