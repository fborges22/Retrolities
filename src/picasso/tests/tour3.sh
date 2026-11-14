# Third GUI tour: dialogs, text options, right button, copies (see tour.sh).
tool() { click $((70 + ($1 % 2) * 25)) $((131 + ($1 / 2) * 25)); }
color() { click $((105 + ($1 % 14) * 16)) $((503 + ($1 / 14) * 16)) "${2:-1}"; }
opt() { click 82 $((336 + $1 * 60 / $2 + 30 / $2)); }
menu() { key "alt+$1"; shift; for k in "$@"; do key "$k"; done; }

menu h a; sleep .5; shot c1; key Return
menu i a; sleep .5; xdotool type --delay 50 "450"; key Tab; xdotool type --delay 50 "300"; shot c2; key Return
tool 3; color 23; click 300 250                                     # green-ish background
tool 9; color 16; color 17 3; click 140 140; xdotool type --delay 50 "Opaque"; key Return; xdotool type --delay 50 "text"; shot c3
opt 1 2; shot c4                                                    # same text, transparent
click 300 150; xdotool type --delay 50 "abc"; key BackSpace; shot c5
tool 14; opt 1 3; drag 140 250 220 310 3                            # right button: swapped colours
tool 1; drag 130 240 230 320; xdotool keydown ctrl; drag 180 280 330 290; xdotool keyup ctrl; shot c6   # ctrl+drag copy
opt 1 2; drag 330 290 480 300; shot c7                              # transparent move
key Escape
menu o e; sleep .7; shot c8; key Escape
menu o f; sleep .7; shot c9; key Escape
key ctrl+p; sleep 1; shot c10; key Return; key Escape
tool 5; click 82 381; shot c11                                      # magnifier option 8x
key alt+F4; sleep .7; shot c12; key Escape
