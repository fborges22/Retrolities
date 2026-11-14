# Picasso file format tests (sourced by tests/run.sh): PICASSO.EXE /convert
# loads a picture into its display bitmap and saves it again in the Win16,
# Win32 and x64 programs under Wine (on a headless X server). Results are
# compared with ImageMagick's reading of the reference pictures.

# source : output : minimum PSNR (0 = identical)
pc_cases="bmp24.bmp:q1.bmp:0 bmp8.bmp:q2.bmp:0 bmprle8.bmp:q3.bmp:0 bmprle4.bmp:q4.bmp:0 bmp1.bmp:q5.bmp:0
png24.png:q6.bmp:0 png24i.png:q7.bmp:0 png8i.png:q8.bmp:0 pnggray.png:q9.bmp:0 pngrgba.png:q10.bmp:45
jpg.jpg:q11.bmp:40 jpg420.jpg:q12.bmp:30 jpgprog.jpg:q13.bmp:30 jpggray.jpg:q14.bmp:40
tiflzw.tif:q15.bmp:0 tifpal.tif:q16.bmp:0 tiftile.tif:q17.bmp:0 tifzip.tif:q18.bmp:0
png24.png:q19.png:0 png8.png:q20.png:0 png24.png:q21.tif:0 png8.png:q22.tif:0 png24.png:q23.jpg:30
jpg.jpg:q24.png:40 tiflzw.tif:q25.png:0 bmp4.bmp:q26.png:0"

if command -v Xvfb >/dev/null; then
    for pc_plat in win16 win9x winnt win64; do
        pc_exe=PICASSO.EXE
        [ "$pc_plat" != win64 ] || pc_exe=PICASSO64.EXE
        [ -f build/$pc_plat/bin/$pc_exe ] || continue
        mkdir -p "$tmp/ic"
        [ -d "$tmp/ic/img" ] || tests/images/make.sh "$tmp/ic/img" >/dev/null 2>&1
        work=$tmp/pc
        rm -rf "$work" && mkdir -p "$work" && cp "build/$pc_plat/bin/$pc_exe" "$work/PICASSO.EXE" \
            && cp "$tmp"/ic/img/* "$work/"
        Xvfb :97 -screen 0 800x600x24 >/dev/null 2>&1 &
        pc_x=$!
        sleep 1
        for item in $pc_cases; do
            IFS=: read -r src out min <<EOT
$item
EOT
            if [ "$pc_plat" = win64 ]; then
                (cd "$work" && DISPLAY=:97 timeout 120 env WINEPREFIX="$tmp/wine64" WINEARCH=win64 \
                    wine PICASSO.EXE /convert "$src" "$out" >/dev/null 2>&1)
            else
                (cd "$work" && DISPLAY=:97 timeout 120 wine PICASSO.EXE /convert "$src" "$out" >/dev/null 2>&1)
            fi
            ic_compare "picasso $pc_plat $src -> $out" "$work/$src" "$work/$out" "$min"
        done
        if [ "$pc_plat" = win64 ]; then
            WINEPREFIX="$tmp/wine64" wineserver -k >/dev/null 2>&1
        else
            wineserver -k >/dev/null 2>&1
        fi
        kill $pc_x 2>/dev/null
        wait $pc_x 2>/dev/null
    done
fi
