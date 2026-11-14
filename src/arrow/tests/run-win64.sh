#!/bin/sh
# Win64 GUI interoperability tests; run inside the test container.
set -eu
cd "$(dirname "$0")/../../.."
WINE_TMP=$(mktemp -d)
export WINEARCH=win64
export WINEPREFIX="$WINE_TMP/prefix"
mkdir -p build/shots
log=$(mktemp)
trap 'wineserver -k >/dev/null 2>&1 || true; rm -f "$log"; rm -rf "$WINE_TMP"' EXIT

echo "=== Arrow x64 Windows build (MinGW-w64, Winsock 2) under Wine"
STREAMS=4 TIME=3 SHOT_AT=2 sh src/arrow/tests/wine-shot.sh \
    build/win64/bin/ARROW64.EXE build/shots/win64.png > "$log" 2>&1
cat "$log"
grep -q "SUM.*receiver" "$log"
echo "PASS: Win64 GUI client completed a 4-stream forward test"

STREAMS=4 REVERSE=1 TIME=3 SHOT_AT=2 sh src/arrow/tests/wine-shot.sh \
    build/win64/bin/ARROW64.EXE build/shots/win64-reverse.png > "$log" 2>&1
cat "$log"
grep -q "SUM.*sender" "$log"
echo "PASS: Win64 GUI client completed a 4-stream reverse test"

for reverse in 0 1; do
    SERVER=1 STREAMS=4 REVERSE=$reverse TIME=3 SHOT_AT=2 sh src/arrow/tests/wine-shot.sh \
        build/win64/bin/ARROW64.EXE "build/shots/win64-server-$reverse.png" > "$log" 2>&1
    cat "$log"
    grep -q "SUM.*receiver" "$log"
    echo "PASS: Win64 GUI server completed a 4-stream test (reverse=$reverse)"
done
