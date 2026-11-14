#!/bin/sh
# Smoke tests, run inside the "test" container after a build:
#
#   1. every binary in build/<platform>/bin has the right executable format;
#   2. every console project with a src/<project>/test.expect file is run
#      (DOSBox-X for dos, Wine for win9x / winnt) and each line of that file
#      must appear in its output.
#
# Win16 binaries and GUI programs are only format-checked.
set -u
cd "$(dirname "$0")/.."

fail=0
count=0
tmp=$(mktemp -d)
trap 'wineserver -k >/dev/null 2>&1; rm -rf "$tmp"' EXIT

# Wine refuses a prefix whose parent directory is not owned by the caller.
export WINEPREFIX="$tmp/wine"
export WINEDEBUG="${WINEDEBUG:--all}"

ok()  { count=$((count + 1)); printf '  ok    %s\n' "$1"; }
bad() { count=$((count + 1)); fail=$((fail + 1)); printf '  FAIL  %s\n' "$1"; }

if [ ! -d build ]; then
    echo "nothing to test: run the build first (docker compose run --rm build)" >&2
    exit 1
fi

# ---- 1. executable formats -------------------------------------------------
for exe in build/*/bin/*; do
    [ -f "$exe" ] || continue
    platform=$(echo "$exe" | cut -d/ -f2)
    fmt=$(python3 tests/exefmt.py "$exe")
    case "$platform:$fmt" in
        dos:mz|dos:com|win16:ne|win9x:pe-*|winnt:pe-*) ok "format $exe ($fmt)" ;;
        *) bad "format $exe ($fmt)" ;;
    esac
done

# ---- 2. run console programs -----------------------------------------------
run_dos() {  # <exe> <output file>
    dir=$tmp/dos
    rm -rf "$dir" && mkdir -p "$dir" && cp "$1" "$dir/"
    timeout 60 dosbox-x -silent -fastlaunch -nolog -nomenu \
        -c "mount c $dir" -c "c:" -c "$(basename "$1") > OUT.TXT" -c "exit" \
        >/dev/null 2>&1
    tr -d '\r' < "$dir/OUT.TXT" > "$2" 2>/dev/null
}

run_wine() {  # <exe> <output file>
    timeout 120 wine "$1" 2>/dev/null | tr -d '\r' > "$2"
}

check_output() {  # <label> <expect file> <output file>
    missing=0
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        grep -qF -- "$line" "$3" || { missing=1; printf '        missing: %s\n' "$line"; }
    done < "$2"
    if [ $missing -eq 0 ]; then ok "$1"; else bad "$1"; sed 's/^/        | /' "$3"; fi
}

for expect in src/*/test.expect; do
    [ -f "$expect" ] || continue
    name=$(basename "$(dirname "$expect")" | tr a-z A-Z)
    for platform in dos win9x winnt; do
        exe=$(ls build/$platform/bin/$name.EXE build/$platform/bin/$name.COM 2>/dev/null | head -n 1)
        [ -n "$exe" ] || continue
        out=$tmp/$platform-$name.out
        case $platform in
            dos) run_dos "$exe" "$out" ;;
            *)   run_wine "$exe" "$out" ;;
        esac
        check_output "run    $exe" "$expect" "$out"
    done
done

echo
if [ $fail -ne 0 ]; then
    echo "$fail of $count checks FAILED"
    exit 1
fi
echo "all $count checks passed"
