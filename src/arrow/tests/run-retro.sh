#!/bin/sh
# End-to-end test of all five builds against a real iperf3 (run via:
#   docker compose --profile test run --rm test ).  Screenshots land in build/shots/.
cd "$(dirname "$0")/../../.." && mkdir -p build/shots
fail=0
SERVER_PID=
WINE_TMP=$(mktemp -d)
export WINEARCH=win32
export WINEPREFIX="$WINE_TMP/prefix"
cleanup() {
    if [ -n "$SERVER_PID" ]; then kill "$SERVER_PID" 2>/dev/null || true; fi
    wineserver -k >/dev/null 2>&1 || true
    rm -rf "$WINE_TMP"
}
trap cleanup EXIT
check() { if grep -q "$2" "$1"; then echo "PASS: $3"; else echo "FAIL: $3"; fail=1; fi; }

echo "=== Windows 9x build (Win32) under Wine"
src/arrow/tests/wine-shot.sh build/win9x/bin/ARROW32.EXE build/shots/win32.png > /tmp/w32.log 2>&1
check /tmp/w32.log "receiver" "Win32 GUI client completed a test"

echo "=== Windows NT/2000 build (Win32, Winsock 2) under Wine"
STREAMS=4 src/arrow/tests/wine-shot.sh build/winnt/bin/ARROWN.EXE build/shots/winnt.png > /tmp/wnt.log 2>&1
check /tmp/wnt.log "SUM.*receiver" "WinNT GUI client completed a 4-stream test"

echo "=== Windows 3.x build (Win16) under Wine"
STREAMS=2 REVERSE=1 src/arrow/tests/wine-shot.sh build/win16/bin/ARROW16.EXE build/shots/win16.png > /tmp/w16.log 2>&1
check /tmp/w16.log "SUM.*sender" "Win16 GUI client completed a 2-stream reverse test"

echo "=== DOS build under DOSBox-X (NE2000 + packet driver + mTCP)"
iperf3 -s > /tmp/srv.log 2>&1 &
SERVER_PID=$!
src/arrow/tests/dos-run.sh 60 - "arrowd -c 10.0.2.2 -t 3 -b > OUT1.TXT" "arrowd -c 10.0.2.2 -t 3 -R -b > OUT2.TXT" \
                      "arrowd -c 10.0.2.2 -t 3 -P 4 -b > OUT3.TXT" > /tmp/dos.log 2>&1
check /tmp/dos.log "iperf Done" "DOS client: forward"
[ "$(grep -c 'iperf Done' /tmp/dos.log)" = 3 ] && echo "PASS: DOS client: reverse and 4 streams" || { echo "FAIL: DOS client reverse/parallel"; fail=1; }

src/arrow/tests/dos-run.sh 30 - "arrowd -s -b > OUTS.TXT" > /dev/null 2>&1 &
DOSPID=$!
sleep 8
iperf3 -c 127.0.0.1 -p 5292 -t 3 > /tmp/c1.log 2>&1
iperf3 -c 127.0.0.1 -p 5292 -t 3 -R -P 2 > /tmp/c2.log 2>&1
check /tmp/c1.log "iperf Done" "DOS server: forward"
check /tmp/c2.log "iperf Done" "DOS server: reverse, 2 streams"
wait $DOSPID

src/arrow/tests/dos-run.sh 12 build/shots/dos.png "arrowd -c 10.0.2.2 -t 20 -P 2" > /dev/null 2>&1
sh src/arrow/tests/run-win64.sh || fail=1
echo "screenshots: $(ls build/shots/*.png | tr '\n' ' ')"
[ $fail = 0 ] && echo "ALL RETRO TESTS PASSED" || echo "SOME TESTS FAILED"
exit $fail
