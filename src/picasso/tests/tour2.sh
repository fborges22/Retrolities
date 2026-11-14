# Second GUI tour: selections, transforms, dialogs and files (see tour.sh).
tool() { click $((70 + ($1 % 2) * 25)) $((131 + ($1 / 2) * 25)); }
color() { click $((105 + ($1 % 14) * 16)) $((503 + ($1 / 14) * 16)) "${2:-1}"; }
opt() { click 82 $((336 + $1 * 60 / $2 + 30 / $2)); }
menu() { key "alt+$1"; shift; for k in "$@"; do key "$k"; done; }

tool 12; color 16; color 17 3; opt 1 3; drag 140 140 300 240        # red/yellow rectangle
tool 13; color 6; color 19 3; drag 340 150 420 150; click 460 220; click 380 260; dclick 330 210   # polygon
tool 2;  color 14 3; drag 150 190 290 190                           # eraser stroke
tool 4;  click 200 160; tool 7; opt 4 12; click 82 359; drag 480 150 560 240   # picked colour, square brush
shot b1
tool 0;  xdotool mousemove 160 150 mousedown 1 mousemove 240 135 sleep .05 mousemove 300 180 sleep .05 mousemove 250 250 sleep .05 mousemove 170 230 sleep .05 mousemove 160 150 mouseup 1; sleep .3
shot b2
drag 220 190 240 330; shot b3                                       # move free-form piece
menu i f r; shot b4                                                 # rotate selection right
key Escape
menu i i; shot b5                                                   # invert picture
key ctrl+z
menu i f v; shot b6                                                 # flip picture vertically
menu i f r; shot b7                                                 # rotate picture
key ctrl+z; key ctrl+z
menu i a; shot b8; xdotool type --delay 50 "300"; key Tab; xdotool type --delay 50 "200"; key Return; shot b9
key ctrl+z
menu h a; shot b10; key Return
menu o e; shot b11; key Escape
key ctrl+a; key ctrl+c; menu i c; shot b12; key ctrl+v; shot b13     # clear, paste back
key Escape
drag 632 444 560 400; shot b14                                      # shrink with the corner handle
key ctrl+s; sleep 1; shot b15; xdotool type --delay 50 'C:\pic.bmp'; key Return; sleep 1
key ctrl+n; sleep .5; shot b16
key ctrl+o; sleep 1; xdotool type --delay 50 'C:\pic.bmp'; key Return; sleep 1; shot b17
menu v t; menu v b; menu v c; menu v s; shot b18
cp "$WINEPREFIX/drive_c/pic.bmp" build/shots/pic.bmp
