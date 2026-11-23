# Program Manager features of ShellIV for tests/gui.sh (1024x768, no WM):
# group import from PROGMAN.INI, drag and drop, program properties and
# icons, starting programs (running tasks, recent programs).
w="$WINEPREFIX/drive_c/windows"
python3 src/shelliv/tests/mkgrp.py "$w/TOOLS.GRP" "Old Tools" \
    'Text Editor|NOTEPAD.EXE|C:\WINDOWS||0' 'Registry|C:\WINDOWS\REGEDIT.EXE|C:\|C:\WINDOWS\REGEDIT.EXE|0|1618' \
    'System Ini|NOTEPAD.EXE C:\WINDOWS\SYSTEM.INI|C:\WINDOWS||0'
printf '[Groups]\r\nGroup1=C:\\WINDOWS\\TOOLS.GRP\r\n' > "$w/progman.ini"
mkdir -p "$WINEPREFIX/drive_c/dd/target"
echo "drag me" > "$WINEPREFIX/drive_c/dd/DRAG.TXT"
log=build/shots/tour2.log
: > "$log"
addr() { key ctrl+l; xdotool type --delay 30 "$1"; key Return; sleep 1.5; }
key alt+p; sleep 1; key i; sleep 2.5; shot p01-imported; key Return; sleep 1
addr 'Programs\Old Tools'; key ctrl+2; sleep 0.5; shot p02-oldtools

# drag DRAG.TXT (second icon) onto the "target" folder (first icon): a move
addr 'C:\dd'; key ctrl+1; sleep 1; shot p03-dd
drag 430 235 338 235; sleep 2.5; shot p04-dropped
if ls "$WINEPREFIX/drive_c/dd/target" | grep -qix drag.txt; then
    echo "ok    drag and drop move" >> "$log"
else
    echo "FAIL  drag and drop move" >> "$log"
fi
# drag DRAG.TXT onto the Accessories group: adds a program item
addr 'C:\dd\target'; sleep 1
drag 338 235 140 248; sleep 2.5; shot p05-addedtogroup
grep -qi "DRAG.TXT" "$w"/[Ss][Hh][Ee][Ll][Ll][Ii][Vv].[Ii][Nn][Ii] && echo "ok    drop a file onto a group" >> "$log" \
    || echo "FAIL  drop a file onto a group" >> "$log"

addr 'Programs\Old Tools'; key Home; key alt+Return; sleep 1.5; shot p06-progprops
xdotool key alt+i; sleep 1.5; shot p07-chicon
key Escape; sleep 1
# give "Text Editor" the shortcut key Ctrl+Alt+E (typing just E adds Ctrl+Alt)
xdotool key alt+k; sleep 0.3; xdotool type E; sleep 0.5; shot p07b-hotkey; key Return; sleep 1; shot p07c-saved

# start a program: it appears under Running Tasks; close it from there
key Home; key Return; sleep 4
for win in $(xdotool search --name Notepad); do xdotool windowmove "$win" 420 360; done
sleep 1; click 350 450; sleep 2.5; shot p08-running
click 140 182; sleep 1.5; key End; key shift+F10; sleep 1; shot p09-taskmenu
key c; sleep 3
if xdotool search --name "Notepad" >/dev/null 2>&1; then
    echo "FAIL  close a task from the sidebar" >> "$log"
else
    echo "ok    close a task from the sidebar" >> "$log"
fi
addr 'Recent Programs'; shot p10-recent
# Ctrl+Alt+R comes from the imported group file: it starts the Registry Editor
click 140 182; sleep 1.5; key ctrl+alt+r; sleep 5
if xdotool search --name "Registry Editor" >/dev/null 2>&1; then
    echo "ok    shortcut key from a group file" >> "$log"
else
    echo "FAIL  shortcut key from a group file" >> "$log"
fi
shot p11-hotkey
for win in $(xdotool search --name "Registry Editor"); do xdotool windowmove "$win" 600 450; done
click 140 182; sleep 1.5
grep -q "Text Editor|.*|1605" "$w"/[Ss][Hh][Ee][Ll][Ll][Ii][Vv].[Ii][Nn][Ii] && echo "ok    set a shortcut key" >> "$log" \
    || echo "FAIL  set a shortcut key" >> "$log"
grep -qi "Old Tools" "$w"/[Ss][Hh][Ee][Ll][Ll][Ii][Vv].[Ii][Nn][Ii] && echo "ok    import PROGMAN.INI group" >> "$log" \
    || echo "FAIL  import PROGMAN.INI group" >> "$log"
# Minimize on use (Options): the setting is saved when Options closes. (Wine
# without a window manager does not iconify windows, so the behaviour itself
# is checked on Windows 3.11 by tests/win311.sh.)
# first close the Registry Editor (it keeps the keyboard) from Running Tasks
click 140 182; sleep 1.5; key ctrl+t; sleep 1; key shift+F10; sleep 1; key c; sleep 3
click 140 182; sleep 1.5
key alt+t; sleep 1; key o; sleep 2; xdotool key alt+m; sleep 0.5; shot p13-options; key Return
for i in 1 2 3 4 5 6 7 8 9 10; do
    grep -aq "MinimizeOnUse=1" "$w"/[Ss][Hh][Ee][Ll][Ll][Ii][Vv].[Ii][Nn][Ii] 2>/dev/null && break
    sleep 1
done
grep -aq "MinimizeOnUse=1" "$w"/[Ss][Hh][Ee][Ll][Ll][Ii][Vv].[Ii][Nn][Ii] && echo "ok    minimize on use saved" >> "$log" \
    || echo "FAIL  minimize on use saved" >> "$log"
cat "$log"
