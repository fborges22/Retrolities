# Fourth GUI tour: other file formats and a picture larger than 2000 pixels.
menu() { key "alt+$1"; shift; for k in "$@"; do key "$k"; done; }
c="$WINEPREFIX/drive_c"
convert -size 3000x2200 gradient:navy-orange -fill white -stroke black -strokewidth 6 \
    -draw 'circle 300,300 300,500' -draw 'rectangle 2700,1900 2950,2150' "$c/big.png"
convert -size 400x300 plasma:fractal -quality 85 -interlace JPEG "$c/photo.jpg"

key ctrl+o; sleep 1; shot g1; xdotool type --delay 40 'C:\photo.jpg'; key Return; sleep 3; shot g2
key ctrl+o; sleep 1; xdotool type --delay 40 'C:\big.png'; key Return; sleep 14; shot g3
drag 510 478 700 478; drag 735 300 735 455; sleep 1; shot g4       # scroll to the far corner
menu i a; sleep .7; shot g5; key Escape
menu f a; sleep 1; shot g6; xdotool type --delay 40 'C:\big2.tif'; key Return; sleep 14
menu f a; sleep 1; xdotool type --delay 40 'C:\big3'; key Return; sleep 14; shot g7
cp "$c/big2.tif" "$c/big3.tif" build/shots/ 2>/dev/null
ls -l "$c" > build/shots/files.txt
compare -metric PSNR "$c/big.png" "$c/big2.tif" null: > build/shots/cmp.txt 2>&1
