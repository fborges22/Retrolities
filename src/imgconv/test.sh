# Codec tests (sourced by tests/run.sh): reference pictures made by ImageMagick
# are converted with IMGCONV.EXE - the 16-bit DOS build under DOSBox-X and the
# Win32 build under Wine - and the results are compared with the originals.
#
#   decoding: every reference file -> 24-bit BMP, compared with ImageMagick's
#             own reading of the file (0 = must be identical, n = minimum PSNR);
#   encoding: pictures written in every format must be readable by ImageMagick
#             and match the source.

ic_dec="bmp24.bmp:0 bmp8.bmp:0 bmp8im.bmp:0 bmp4.bmp:0 bmp1.bmp:0 bmprle8.bmp:0 bmprle4.bmp:0
bmp32.bmp:0 bmp16.bmp:40
png24.png:0 png24i.png:0 png48.png:0 png8.png:0 png8i.png:0 png4.png:0 png1.png:0 pnggray.png:0
pngg16.png:0 pngg4.png:0 pngrgba.png:45 pngga.png:45
jpg.jpg:40 jpg420.jpg:30 jpg422.jpg:30 jpgprog.jpg:30 jpgpr444.jpg:40 jpggray.jpg:40 jpggrayp.jpg:40
jpgrst.jpg:30 jpgcmyk.jpg:25
tif.tif:0 tiflzw.tif:0 tiflzwp.tif:0 tifpack.tif:0 tifzip.tif:0 tifmm.tif:0 tifpal.tif:0 tifgray.tif:0
tifbw.tif:0 tif48.tif:0 tiftile.tif:0 tifrgba.tif:45 tifcmyk.tif:25"

# source : output : extra argument : minimum PSNR (0 = identical)
ic_enc="png24.png:e1.png::0 png8.png:e2.png::0 png4.png:e3.png::0 png1.png:e4.png::0 pnggray.png:e5.png::0
png24.png:e6.tif::0 png8.png:e7.tif::0 pnggray.png:e8.tif::0
png24.png:e9.jpg::30 png24.png:e10.jpg:75:25 pnggray.png:e11.jpg::35
png24.png:e12.bmp:24:0 png8.png:e13.bmp:8:0 bmp4.bmp:e14.bmp:4:0 png1.png:e15.bmp:1:0 png24.png:e16.bmp:8:28
jpgprog.jpg:e17.png::30 tiflzw.tif:e18.png::0"

ic_compare() {  # <label> <reference> <result> <min psnr>
    if [ ! -s "$3" ]; then bad "$1 (no output)"; return; fi
    convert "$2" -background white -alpha remove -alpha off -colorspace sRGB -depth 8 "$tmp/ic/ref.ppm" 2>/dev/null
    convert "$3" -alpha off -depth 8 "$tmp/ic/res.ppm" 2>/dev/null
    if [ "$4" = 0 ]; then
        if cmp -s "$tmp/ic/ref.ppm" "$tmp/ic/res.ppm"; then ok "$1"; else bad "$1 (differs)"; fi
    else
        psnr=$(compare -metric PSNR "$tmp/ic/ref.ppm" "$tmp/ic/res.ppm" null: 2>&1 | sed 's/ .*//')
        if echo "$psnr $4" | awk '{ exit !($1 == "inf" || $1 + 0 >= $2) }'; then ok "$1"; else bad "$1 (PSNR $psnr < $4)"; fi
    fi
}

ic_platform() {  # <platform> <command that runs RUN.BAT in $tmp/ic/work>
    exe=build/$1/bin/IMGCONV.EXE
    [ -f "$exe" ] || return 0
    work=$tmp/ic/work
    rm -rf "$work" && mkdir -p "$work" && cp "$exe" "$tmp"/ic/img/* "$work/"
    : > "$work/RUN.BAT"
    n=0
    for item in $ic_dec; do             # outputs are numbered: DOS names are 8.3
        n=$((n + 1))
        printf 'IMGCONV %s D%s.BMP 24\r\n' "${item%%:*}" "$n" >> "$work/RUN.BAT"
    done
    for item in $ic_enc; do
        IFS=: read -r src out arg min <<EOT
$item
EOT
        printf 'IMGCONV %s %s %s\r\n' "$src" "$out" "$arg" >> "$work/RUN.BAT"
    done
    $2 "$work"
    n=0
    for item in $ic_dec; do
        n=$((n + 1))
        f=${item%%:*}
        res=$(ls "$work" | grep -i "^d$n\.bmp$" | head -n 1)
        ic_compare "imgconv $1 read  $f" "$tmp/ic/img/$f" "$work/${res:-missing}" "${item##*:}"
    done
    for item in $ic_enc; do
        IFS=: read -r src out arg min <<EOT
$item
EOT
        res=$(ls "$work" | grep -i "^$out$" | head -n 1)
        ic_compare "imgconv $1 write $out ($src $arg)" "$tmp/ic/img/$src" "$work/${res:-missing}" "$min"
    done
}

ic_dos() { dos_run "$1" "RUN.BAT"; }
ic_wine() { (cd "$1" && timeout 300 wine cmd /c RUN.BAT >/dev/null 2>&1); }

if [ -f build/dos/bin/IMGCONV.EXE ] || [ -f build/winnt/bin/IMGCONV.EXE ]; then
    mkdir -p "$tmp/ic"
    tests/images/make.sh "$tmp/ic/img" >/dev/null 2>&1
    ic_platform dos ic_dos
    ic_platform winnt ic_wine
fi
