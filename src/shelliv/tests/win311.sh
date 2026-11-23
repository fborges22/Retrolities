#!/bin/sh
# ShellIV on a real Windows 3.11 installation in QEMU (run in the "test"
# container). The disk image is copied, never changed: ShellIV is installed
# as the Windows shell on the copy, Windows boots headless, the script types
# through QEMU's monitor and takes screenshots, and the results are checked
# on the disk afterwards.
#
#   docker compose run --rm -v ~/Shared/win311:/img:ro test \
#       src/shelliv/tests/win311.sh /img/hda.img
#
# The image must boot to Windows with a Program Manager setup (PROGMAN.INI
# and .GRP files), FAT16 at sector 63, and (like the rtts.eu/win311 image)
# may need an NE2000 PCI card and a Sound Blaster 16 to start quietly.
# Screenshots go to build/shots/w311-*.png.
set -u
cd "$(dirname "$0")/../../.."
img=${1:?usage: win311.sh <hda.img>}
exe=build/win386/bin/SHELLIV.EXE
[ -f "$exe" ] || { echo "build $exe first" >&2; exit 1; }
work=$(mktemp -d)
trap 'kill $qemu 2>/dev/null; rm -rf "$work"' EXIT
mkdir -p build/shots
cp "$img" "$work/hda.img"
dd if=/dev/zero of="$work/a.img" bs=1024 count=1440 2>/dev/null      # blank floppy
echo "drive c: file=\"$work/hda.img\" offset=32256" > "$work/mtools"
export MTOOLSRC="$work/mtools"
mmd c:/shelliv 2>/dev/null
mcopy -o "$exe" c:/shelliv/
mdel c:/windows/shelliv.ini 2>/dev/null
mtype c:/windows/system.ini | sed 's/^shell=.*/shell=C:\\SHELLIV\\SHELLIV.EXE\r/I' > "$work/system.ini"
mcopy -o "$work/system.ini" c:/windows/system.ini

# -icount fixes the guest speed (about a 486) so WfW's network driver
# calibration cannot divide by zero on a fast host.
qemu-system-i386 -icount shift=7 -drive file="$work/hda.img",format=raw \
    -drive file="$work/a.img",format=raw,if=floppy -vga cirrus \
    -device sb16 -nic user,ipv6=off,model=ne2k_pci,restrict=on -display none \
    -monitor unix:"$work/mon",server,nowait >/dev/null 2>&1 &
qemu=$!

mon() {
    python3 -c 'import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); time.sleep(0.2); s.recv(4096)
s.sendall((sys.argv[2] + "\n").encode()); time.sleep(0.4); s.close()' "$work/mon" "$1"
}
shot() { mon "screendump $work/s.ppm"; sleep 1; convert "$work/s.ppm" "build/shots/w311-$1.png"; }
k() { mon "sendkey $1"; sleep "${2:-1.2}"; }
typ() {
    s=$1
    while [ -n "$s" ]; do
        c=$(printf '%s' "$s" | cut -c1); s=$(printf '%s' "$s" | cut -c2-)
        case "$c" in
            [a-z0-9]) k "$c" 0.4 ;;
            [A-Z]) k "shift-$(printf '%s' "$c" | tr A-Z a-z)" 0.4 ;;
            ':') k shift-semicolon 0.4 ;; '\') k backslash 0.4 ;; '.') k dot 0.4 ;;
            ' ') k spc 0.4 ;; '*') k shift-8 0.4 ;; '_') k shift-minus 0.4 ;;
            '/') k slash 0.4 ;; '>') k shift-dot 0.4 ;;
        esac
    done
}
addr() { k ctrl-l 2; typ "$1"; k ret 12; }
pick() { k alt-e 2; k f 6; typ "$1"; k ret 6; }   # Edit > Select Files

sleep "${W311_BOOT:-240}"                     # boot, network start, first-run import
shot 01-firstrun
addr 'Programs\Main'; shot 02-main
addr 'C:\WINDOWS'; k ctrl-3 10; shot 03-details
addr 'My Computer'; k ctrl-2 8; shot 04-computer
# copy SYSTEM.INI to C:\TEMP through the file clipboard, rename the copy,
# delete it to the wastebasket and restore it
addr 'C:\WINDOWS'; pick SYSTEM.INI; k ctrl-c 3
addr 'C:\TEMP'; k ctrl-v 20; shot 05-pasted
pick SYSTEM.INI; k f2 6; typ SYS2; k ret 15
pick SYS2.INI; k delete 6; k ret 20
addr 'Wastebasket'; k home 4; shot 06-waste; k alt-t 3; k r 25
# give Notepad (the first item of Main) the shortcut key Ctrl+Alt+E and use it
addr 'Programs\Main'; k home 4; k alt-ret 12; k alt-k 2; typ e; k ret 12
k ctrl-alt-e 25; shot 07-hotkey; k alt-f4 8
k ctrl-f 10; typ '*.INI'; k tab 2; typ mouse; k ret 120; shot 08-search
# Associate: .INI files open with Write
addr 'C:\TEMP'; pick SYS2.INI; k alt-f 2; k a 8; typ WRITE.EXE; k ret 8
# attributes of several files at once: make both X1*.TMP read-only
pick 'X1*.TMP'; k alt-ret 15; shot 09-props; k alt-r 2; k ret 10
# Run, minimized: a DOS command that leaves a file behind
k ctrl-r 8; typ 'COMMAND.COM /C ECHO RUN > C:\TEMP\RUNMIN.TXT'; k alt-m 2; k ret 25
# a minimized DOS session is suspended until it is activated: switch to it
# from Running Tasks (the last node of the sidebar)
switch_last() { k ctrl-t 3; k ret 30; }   # Go > Running Tasks, Enter switches
shot 10-minimized; switch_last
# StartUp: a program that leaves a file behind, started at the next boot
k ctrl-l 2; k esc 3
addr 'Programs\StartUp'; k alt-f 2; k p 8; typ Marker; k tab 2
typ 'COMMAND.COM /C ECHO START > C:\TEMP\STARTUP.TXT'; k ret 10; shot 11-startup
# Disk menu: label drive C:, then format the blank floppy with FORMAT
k ctrl-l 2; k esc 3
addr 'C:\'; k alt-d 2; k l 8; typ SHELLIV; k ret 40
k alt-d 2; k f 8; k ret 30; shot 13-format; k ret 150
k ret 15; k n 3; k ret 30
# Copy Disk: DISKCOPY A: A: onto itself, answering its prompts
k alt-d 2; k c 8; k ret 30; k ret 120; k ret 120; shot 14-diskcopy; k n 3; k ret 5; k n 3; k ret 25
# Minimize on use: starting a program minimizes ShellIV; after closing the
# program, the desktop shows where the sidebar was
k ctrl-l 2; k esc 3
k alt-t 2; k o 8; k alt-m 2; k ret 8; shot 15-before
convert "build/shots/w311-15-before.png" -format '%[pixel:p{150,600}]' info: > "$work/pixel0"
addr 'NOTEPAD.EXE'; sleep 15; k alt-f4 15; shot 15-minimized
convert "build/shots/w311-15-minimized.png" -format '%[pixel:p{150,600}]' info: > "$work/pixel"
# restore ShellIV (its icon has the focus after Notepad closes)
k alt-spc 3; k r 10
k alt-t 2; k o 8; k alt-m 2; k ret 8      # and switch the option off again
# Exit Windows properly (the image powers off after Windows)
k alt-f 2; k x 4; k ret 60
kill $qemu 2>/dev/null; wait $qemu 2>/dev/null

# second boot: ShellIV starts the StartUp group
qemu-system-i386 -icount shift=7 -drive file="$work/hda.img",format=raw \
    -drive file="$work/a.img",format=raw,if=floppy -vga cirrus \
    -device sb16 -nic user,ipv6=off,model=ne2k_pci,restrict=on -display none \
    -monitor unix:"$work/mon",server,nowait >/dev/null 2>&1 &
qemu=$!
sleep "${W311_BOOT:-240}"; sleep 40; shot 12-second-boot; switch_last
mon quit
wait $qemu 2>/dev/null

fail=0
check() {
    if eval "$2"; then echo "  ok    win311: $1"; else echo "  FAIL  win311: $1"; fail=1; fi
}
check "first run imports the PROGMAN.INI groups" \
    'mtype c:/windows/shelliv.ini | grep -aqi "^Count=" && [ "$(mtype c:/windows/shelliv.ini | grep -ac "^G[0-9]*=")" -ge 2 ]'
check "copy, rename, delete to the wastebasket and restore" \
    'mdir -b c:/temp/sys2.ini >/dev/null 2>&1'
check "wastebasket index rewritten" '! mdir -b c:/shelliv.wst/w*.ini >/dev/null 2>&1'
check "shortcut key saved" 'mtype c:/windows/shelliv.ini | grep -aq "|1605"'
check "associate .INI with WRITE.EXE" 'mtype c:/windows/win.ini | grep -aqi "^ini=WRITE.EXE ^.ini"'
check "read-only set on several files at once" \
    '[ "$(mattrib c:/temp/x1*.tmp | awk "{ \$NF = \"\"; print }" | grep -c R)" -ge 2 ]'
check "run minimized" 'mtype c:/temp/runmin.txt | grep -aq RUN'
check "Label Disk" 'mlabel -s c: | grep -qi SHELLIV'
check "Format Disk (FORMAT A: on a blank floppy)" 'mdir -i "$work/a.img" :: >/dev/null 2>&1'
check "Copy Disk (DISKCOPY A: A:) leaves a valid disk" 'mdir -i "$work/a.img" :: >/dev/null 2>&1'
check "minimize on use (sidebar gone after starting a program)" \
    'echo "    sidebar pixel before: $(cat "$work/pixel0"), after: $(cat "$work/pixel")";
     navy() { python3 -c "import re,sys; v=[int(x) for x in re.findall(r\"\d+\", open(sys.argv[1]).read())][:3]; sys.exit(0 if len(v)==3 and v[0]<60 and v[1]<70 and 30<v[2]<90 else 1)" "$1"; }
     navy "$work/pixel0" && ! navy "$work/pixel"'
check "StartUp group runs when ShellIV is the shell" 'mtype c:/temp/startup.txt | grep -aq START'
exit $fail
