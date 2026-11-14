#!/bin/sh
# usage: wine-shot.sh <exe> <out.png> [peer args]   (runs inside the test container)
# Starts a compatible peer, launches the GUI under Wine/Xvfb, presses Enter (= Start),
# takes a screenshot mid-test and prints the server-side result.
EXE=$1; OUT=$2
export DISPLAY=:99
XVFB_PID=; GUI_PID=; SERVER_PID=
cleanup() {
    for pid in "$GUI_PID" "$SERVER_PID" "$XVFB_PID"; do
        if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; fi
    done
}
capture() {
    output=$1
    import -window "$W" -bordercolor black -border 1 -trim +repage "$output"
    if [ -f "${output%.png}-0.png" ]; then
        mv "${output%.png}-0.png" "$output"
        rm -f "${output%.png}-1.png"
    fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
Xvfb :99 -screen 0 800x600x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1
mkdir -p "$HOME"
wineboot -i >/dev/null 2>&1
printf '[Settings]\r\nHost=127.0.0.1\r\nPort=5201\r\nTime=%s\r\nStreams=%s\r\nBlock=16384\r\nServer=%s\r\nReverse=%s\r\n' "${TIME:-6}" "${STREAMS:-1}" "${SERVER:-0}" "${REVERSE:-0}" > "$WINEPREFIX/drive_c/windows/NETBOLT.INI"
# the NT build keeps the same settings in the registry instead
printf 'REGEDIT4\r\n\r\n[HKEY_CURRENT_USER\\Software\\NetBolt\\Settings]\r\n"Host"="127.0.0.1"\r\n"Port"="5201"\r\n"Time"="%s"\r\n"Streams"="%s"\r\n"Block"="16384"\r\n"Server"="%s"\r\n"Reverse"="%s"\r\n' "${TIME:-6}" "${STREAMS:-1}" "${SERVER:-0}" "${REVERSE:-0}" > /tmp/netbolt.reg
wine regedit /S /tmp/netbolt.reg >/dev/null 2>&1
if [ "${SERVER:-0}" = 0 ]; then
    iperf3 -s -1 > /tmp/srv.log 2>&1 &
    SERVER_PID=$!
fi
sleep 0.5
wine "$EXE" >/tmp/wine.log 2>&1 &
GUI_PID=$!
sleep ${BOOT:-6}
W=$(xdotool search --name "NetBolt" | head -1)
echo "window: $W"
[ -n "$W" ] || { echo "NetBolt window was not found" >&2; exit 1; }
xdotool windowactivate --sync $W 2>/dev/null; xdotool windowfocus $W 2>/dev/null
xdotool key --window $W Return
if [ "${SERVER:-0}" = 1 ]; then
    sleep 0.5
    if [ "${REVERSE:-0}" = 1 ]; then
        iperf3 -c 127.0.0.1 -t "${TIME:-6}" -P "${STREAMS:-1}" -R > /tmp/srv.log 2>&1 &
    else
        iperf3 -c 127.0.0.1 -t "${TIME:-6}" -P "${STREAMS:-1}" > /tmp/srv.log 2>&1 &
    fi
    SERVER_PID=$!
fi
sleep ${SHOT_AT:-4}
capture "${OUT%.png}-running.png"
sleep $(( ${TIME:-6} + 2 - ${SHOT_AT:-4} ))
capture "$OUT"
tail -6 /tmp/srv.log
tail -3 /tmp/wine.log
