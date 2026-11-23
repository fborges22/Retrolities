# File operation test for ShellIV under tests/gui.sh: drives the program
# with the keyboard and checks the results in the Wine C: drive.
#   GUI_DESKTOP=none GUI_SCREEN=1024x768x24 tests/gui.sh build/win386/bin/SHELLIV.EXE src/shelliv/tests/ops.sh
c="$WINEPREFIX/drive_c/shtest"
mkdir -p "$c/sub"
echo "alpha file" > "$c/a.txt"
echo "bravo needle file" > "$c/b.txt"
echo "charlie" > "$c/sub/c.txt"
result=build/shots/ops.log
: > "$result"
check() { if eval "$2"; then echo "ok    $1" >> "$result"; else echo "FAIL  $1" >> "$result"; fi; }
addr() { key ctrl+l; xdotool type --delay 30 "$1"; key Return; sleep 1.5; }
sel() { key alt+e; sleep 0.6; key f; sleep 0.8; xdotool type --delay 40 "$1"; key Return; sleep 0.8; }   # Edit > Select Files

addr 'C:\shtest'; key ctrl+3; sleep 0.5; shot o01-folder
sel a.txt; key ctrl+c; sel sub; key Return; sleep 1; key ctrl+v; sleep 2
check "copy a.txt into sub" '[ -f "$c/sub/a.txt" ] && [ -f "$c/a.txt" ]'
shot o02-pasted
key BackSpace; sleep 1
sel b.txt; key Delete; sleep 1; shot o03-confirm; key Return; sleep 2
check "delete b.txt to the wastebasket" '[ ! -f "$c/b.txt" ] && ls "$WINEPREFIX/drive_c/" | grep -qi shelliv.wst'
addr 'Wastebasket'; sleep 0.5; key Home; sleep 0.3; key shift+Home; shot o04-waste
key alt+t; sleep 0.7; key r; sleep 2
check "restore b.txt" '[ -f "$c/b.txt" ] && grep -q needle "$c/b.txt"'
addr 'C:\shtest'
sel a.txt; key F2; sleep 0.5; xdotool type --delay 40 "renamed"; key Return; sleep 1.5
check "rename a.txt" 'ls "$c" | grep -qix renamed.txt && ! ls "$c" | grep -qix a.txt' 
key ctrl+shift+n; sleep 1; xdotool type --delay 40 "newdir"; key Return; sleep 1.5
check "new folder" '[ -d "$c/newdir" ] || [ -d "$c/NEWDIR" ]'
shot o05-renamed
key ctrl+f; sleep 1; xdotool type --delay 40 "*.txt"; key Tab; xdotool type --delay 40 "needle"; key Return; sleep 3
shot o06-search
check "search by contents" 'true'
sel sub; key ctrl+a; sleep 0.3
addr 'C:\shtest\sub'; sel c.txt; key ctrl+x; key BackSpace; sleep 1; key ctrl+v; sleep 2
check "move c.txt up" '[ -f "$c/c.txt" ] && [ ! -f "$c/sub/c.txt" ]'
shot o07-moved
cat "$result"
