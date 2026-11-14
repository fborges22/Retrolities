# GUI tour of Picasso for tests/gui.sh (800x600 desktop).  Run with:
#   docker compose run --rm test tests/gui.sh build/win16/bin/PICASSO.EXE src/picasso/tests/tour.sh
tool() { click $((70 + ($1 % 2) * 25)) $((131 + ($1 / 2) * 25)); }   # tool index 0..15
color() { click $((105 + ($1 % 14) * 16)) $((503 + ($1 / 14) * 16)) "${2:-1}"; }
opt() { click 82 $((336 + $1 * 60 / $2 + 30 / $2)); }                # option i of n

tool 6;  color 16; drag 140 150 260 200                 # red pencil
tool 7;  color 20; drag 140 220 300 180                 # blue brush
tool 10; color 0;  opt 3 5; drag 320 140 420 220        # thick line
tool 12; color 4;  color 17 3; opt 1 3; drag 440 140 560 220   # green rect, yellow fill
tool 14; opt 2 3; color 21; drag 150 260 280 340        # solid magenta ellipse
tool 15; opt 0 3; color 6; drag 310 260 420 340         # rounded rect outline
tool 3;  color 19; click 360 300                        # fill it cyan
tool 8;  color 2; drag 460 260 560 330                  # airbrush
tool 9;  color 0; color 14 3; click 150 370; xdotool type --delay 60 "Hello Picasso!"; sleep 0.3
tool 11; color 5; drag 400 380 580 420; drag 450 350 450 352; drag 520 440 520 442   # curve
shot tour1

tool 1; drag 430 130 570 230; shot tour2                # select the rectangle
drag 500 180 480 330; shot tour3                        # move it
key ctrl+z; key ctrl+c
click 140 77; shot tour4                                # Edit menu
key Escape; key Escape
xdotool mousemove 152 100; sleep 1.5; shot tour5        # tool tip
click 343 100; click 343 100; shot tour6                # zoom in twice
click 368 100; shot tour7                               # grid
