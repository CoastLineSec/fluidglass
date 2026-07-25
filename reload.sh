#!/usr/bin/env bash
# Rebuild + hot-reload the hyprfluidglass plugin — cleanly, with no instance pile-up and no crash.
#
# ROOT CAUSE of the old "extra instances" bug: Hyprland matches plugins by EXACT path STRING
# (PluginSystem.cpp: getPluginByPath -> `p->m_path == path`, no realpath/canonicalization).
# If you load with an absolute path but unload with a relative one (or vice-versa), the strings
# differ, getPluginByPath() finds nothing, and `plugin unload` is a SILENT NO-OP — the instance
# stays registered and keeps rendering (shadowing your new code). Fix: ALWAYS use absolute paths,
# and unload every hyprfluidglass .so that is actually mapped (read the exact strings from /proc/maps).
#
# CRASH SAFETY: unloadPlugin() runs PLUGIN_EXIT, which resets the render-stage listener BEFORE
# dlclose — so unload is safe ONLY once the plugin has stopped rendering. We disable via the
# SETTING (hyprFluidGlassEnabled=false, which makes the shell STOP re-asserting enabled) and GATE on
# the plugin reporting `enabled=no` stably before touching unload. Never unload a rendering plugin.
#
# NOTE: after unload, Hyprland's instance list drops (the real measure), but glibc may keep the old
# .so mapped (TLS-pinning; dead, unsubscribed, never called). That memory is reclaimed on restart.
set -u
SET="$HOME/.config/HyprGlassShell/settings.json"
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR" || exit 1
HPID=$(pgrep -x Hyprland | head -1)
ninst()  { hyprctl plugins list -j 2>/dev/null | python3 -c 'import json,sys;print(len(json.load(sys.stdin)))' 2>/dev/null || echo "?"; }
mapped() { grep -oE '/[^ ]*hyprfluidglass[^ ]*\.so' /proc/$HPID/maps 2>/dev/null | sort -u; }
alive()  { hyprctl version >/dev/null 2>&1 && echo ALIVE || echo DOWN; }
setenabled() { python3 - "$SET" "$1" <<'PY'
import json,sys
p,val=sys.argv[1],sys.argv[2]=="true"
d=json.load(open(p)); d["hyprFluidGlassEnabled"]=val
json.dump(d,open(p,"w"),indent=2)
PY
}

echo "==> build (live plugin untouched if this fails)"
make >/dev/null || { echo "BUILD FAILED — aborting, live plugin unchanged"; exit 1; }

echo "==> disable via setting + material off; gate until STABLY disabled"
setenabled false; hyprctl hyprfluidglass-material off >/dev/null 2>&1
ok=0
for i in $(seq 1 24); do
  hyprctl hyprfluidglass-status 2>&1 | grep -q 'enabled=no' && ok=$((ok+1)) || ok=0
  [ "$ok" -ge 5 ] && break; sleep 0.25
done
if [ "$ok" -lt 5 ]; then
  echo "ABORT: plugin did not stay disabled — NOT unloading (would risk a crash). Re-enabling."
  setenabled true; hyprctl hyprfluidglass-material on >/dev/null 2>&1; exit 1
fi

echo "==> unload EVERY mapped hyprfluidglass .so by exact absolute path"
for so in $(mapped); do
  echo "    unload $so -> $(hyprctl plugin unload "$so" 2>&1 | tr -d '\n')"
done
sleep 0.4

N=$(ninst); A=$(alive)
echo "==> instances now=$N  hyprland=$A"
if [ "$N" != "0" ] || [ "$A" != "ALIVE" ]; then
  echo "ABORT: not a clean slate (instances=$N alive=$A) — NOT loading. Re-enabling what's there."
  setenabled true; hyprctl hyprfluidglass-material on >/dev/null 2>&1; exit 1
fi

echo "==> clean slate -> load ONE fresh copy (absolute path)"
rm -f build/hyprfluidglass-*.so
NEW="$DIR/build/hyprfluidglass-$(date +%s).so"
cp "$DIR/build/hyprfluidglass.so" "$NEW"
echo "    load $NEW -> $(hyprctl plugin load "$NEW" 2>&1 | tr -d '\n')"

echo "==> re-enable"
setenabled true; hyprctl hyprfluidglass-material on >/dev/null 2>&1
sleep 0.4
echo "RESULT: instances=$(ninst)  $(hyprctl hyprfluidglass-status 2>&1)  hyprland=$(alive)"
