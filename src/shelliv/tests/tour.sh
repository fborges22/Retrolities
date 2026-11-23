# GUI tour of ShellIV for tests/gui.sh (1024x768 screen, no window manager).
#   docker compose run --rm test sh -c 'GUI_DESKTOP=none GUI_SCREEN=1024x768x24 \
#       tests/gui.sh build/win386/bin/SHELLIV.EXE src/shelliv/tests/tour.sh'
addr() { key ctrl+l; xdotool type --delay 30 "$1"; key Return; sleep 1; }
shot t01-programs
addr 'Programs\Accessories'; shot t02-group
addr 'C:\windows'; key ctrl+3; sleep 0.5; shot t03-details
key ctrl+2; sleep 0.5; shot t04-tiles
key ctrl+1; xdotool type --delay 80 "win.ini"; sleep 1; shot t05-preview
addr 'My Computer'; key ctrl+2; key Home; sleep 0.5; shot t06-computer
key ctrl+1
key ctrl+f; sleep 1; shot t07-find
key Escape
key alt+v; sleep 0.7; key t; sleep 0.5; key s; sleep 0.8; addr 'C:\windows\system32'; key ctrl+3; sleep 0.5; shot t08-sunrise
key alt+v; sleep 0.7; key t; sleep 0.5; key g; sleep 0.5; addr 'Wastebasket'; shot t09-graphite
key alt+v; sleep 0.7; key t; sleep 0.5; key m; sleep 0.5
key alt+t; sleep 0.7; key o; sleep 1; shot t10-options
key Escape
key alt+h; sleep 0.7; key a; sleep 1; shot t11-about
key Return
