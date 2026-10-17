#!/usr/bin/env bash
# Standalone Gamescope DRM baseline for prospective shutter-stereo integration.
# Intentionally does not open libnvstusb, modify KMS settings, or use the glasses.
set -euo pipefail

if [[ -n "${WAYLAND_DISPLAY:-}" || -n "${DISPLAY:-}" || "${XDG_SESSION_TYPE:-}" == "wayland" || "${XDG_SESSION_TYPE:-}" == "x11" ]]; then
    echo "ERROR: Run from a local TTY login, not inside Plasma/X11/Wayland." >&2
    echo "Switch to a spare TTY (for example Ctrl+Alt+F3), log in, then run this script." >&2
    exit 2
fi
if [[ ! -t 0 ]]; then
    echo "ERROR: DRM baseline must be run from an interactive local TTY." >&2
    exit 2
fi
for cmd in gamescope vkcube tee; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "ERROR: $cmd is not installed or not in PATH." >&2
        echo "On Arch: install gamescope and vulkan-tools (vkcube)." >&2
        exit 127
    fi
done

log="${GAMESCOPE_STEREO_LOG:-$HOME/gamescope-drm-baseline.log}"
echo "Gamescope DRM diagnostic; no USB/shutter commands."
echo "Requested display: 2560x1440, nested/client refresh: 120 Hz."
echo "Important: -r is the nested CLIENT refresh; confirm the PHYSICAL output mode"
echo "selected by the DRM backend from logs; do not assume -r sets the connector."
echo "Log: $log"
echo "Stop vkcube or press Ctrl+C when finished."
echo "Do not wear stereo glasses for this test."
echo
set +e
env -u DISPLAY -u WAYLAND_DISPLAY \
    gamescope --backend drm -W 2560 -H 1440 \
        -w 2560 -h 1440 -r 120 -- vkcube \
        2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e
echo "Gamescope exit status: $status"
exit "$status"
