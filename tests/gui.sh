#!/bin/sh
# Runs a Windows GUI program under Wine on a headless X server so it can be
# driven with xdotool and captured with ImageMagick (inside the "test" container):
#
#   tests/gui.sh <exe> [script]
#
# The optional script is sourced once the program is up; it can use the helpers
# shot <name> (writes build/shots/<name>.png), click / dclick x y [button],
# drag x1 y1 x2 y2 [button], key <keys> and any xdotool command.
set -u
cd "$(dirname "$0")/.."
exe=$1
script=${2:-}
tmp=$(mktemp -d)
export WINEPREFIX="$tmp/wine" WINEDEBUG="${WINEDEBUG:--all}" DISPLAY=:99
unset SDL_VIDEODRIVER
mkdir -p build/shots
Xvfb :99 -screen 0 "${GUI_SCREEN:-800x600x24}" >/dev/null 2>&1 &
xvfb=$!
trap 'wineserver -k >/dev/null 2>&1; kill $xvfb 2>/dev/null; rm -rf "$tmp"' EXIT
sleep 1
shot() { import -window root "build/shots/$1.png"; }
click() { xdotool mousemove "$1" "$2" sleep 0.15 click "${3:-1}" sleep 0.25; }
dclick() { xdotool mousemove "$1" "$2" sleep 0.15 click --repeat 2 --delay 80 1 sleep 0.3; }
drag() {  # x1 y1 x2 y2 [button]
    xdotool mousemove "$1" "$2" sleep 0.1 mousedown "${5:-1}" sleep 0.1 \
        mousemove $((($1 * 2 + $3) / 3)) $((($2 * 2 + $4) / 3)) sleep 0.08 \
        mousemove $((($1 + $3 * 2) / 3)) $((($2 + $4 * 2) / 3)) sleep 0.08 \
        mousemove "$3" "$4" sleep 0.1 mouseup "${5:-1}" sleep 0.25
}
key() { xdotool key "$@"; sleep 0.3; }

# Classic gray colour scheme, as on Windows 3.x / 95.
wine reg add 'HKCU\Control Panel\Colors' /v ButtonFace /d '192 192 192' /f >/dev/null 2>&1
wine reg add 'HKCU\Control Panel\Colors' /v ButtonShadow /d '128 128 128' /f >/dev/null 2>&1
wine reg add 'HKCU\Control Panel\Colors' /v ButtonHilight /d '255 255 255' /f >/dev/null 2>&1
wine reg add 'HKCU\Control Panel\Colors' /v Menu /d '192 192 192' /f >/dev/null 2>&1
wine reg add 'HKCU\Control Panel\Colors' /v ActiveTitle /d '0 0 128' /f >/dev/null 2>&1
wine reg add 'HKCU\Control Panel\Colors' /v Hilight /d '0 0 128' /f >/dev/null 2>&1
wineserver -w
shift; [ $# -gt 0 ] && shift
# GUI_DESKTOP=none runs the program directly on the X screen (for Wine
# versions whose virtual desktop does not paint under Xvfb).
if [ "${GUI_DESKTOP:-}" = none ]; then
    wine "$exe" "$@" &
else
    wine explorer "/desktop=gui,${GUI_DESKTOP:-800x600}" "$exe" "$@" &
fi
sleep "${GUI_WAIT:-12}"
if [ -n "$script" ]; then . "$script"; else shot screen; fi
