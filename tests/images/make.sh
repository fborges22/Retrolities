#!/bin/sh
# Generates the reference pictures used by the codec tests into the directory
# given as argument (needs ImageMagick).  Names are 8.3 so DOS can read them.
set -eu
out=$1
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
cd "$out"
IM=$(command -v magick || command -v convert)
# A colourful 150x113 picture with gradients, sharp edges and text-like detail.
$IM -seed 7 -size 150x113 gradient:blue-yellow \( -size 150x113 plasma:fractal \) -compose blend -define compose:args=50 -composite \
    -fill red -draw 'circle 40,40 40,70' -fill white -stroke black -draw 'rectangle 80,20 140,60' \
    -fill none -stroke green -strokewidth 3 -draw 'line 5,100 145,10' -depth 8 -type TrueColor -alpha off src.ppm
$IM src.ppm -colors 200 -type Palette pal.ppm
$IM src.ppm -colorspace Gray gray.pgm

# name, source, ImageMagick options and "FORMAT:name"
gen() { src=$2; shift 2; $IM "$src" "$@"; }

gen bmp24.bmp    src.ppm  -type TrueColor BMP3:bmp24.bmp
gen bmp8.bmp     pal.ppm  -type Palette -compress None BMP3:bmp8.bmp
gen bmp8im.bmp   pal.ppm  -type Palette -compress RLE BMP3:bmp8im.bmp
gen bmp4.bmp     src.ppm  -colors 16 -type Palette -depth 4 -compress None BMP3:bmp4.bmp
gen bmp1.bmp     src.ppm  -monochrome -type Bilevel BMP3:bmp1.bmp
gen bmp32.bmp    src.ppm  -alpha on -define bmp:format=bmp4 BMP:bmp32.bmp
gen bmp16.bmp    src.ppm  -define bmp:subtype=RGB565 BMP:bmp16.bmp

python3 "$here/mkrle.py" bmp8.bmp bmprle8.bmp
python3 "$here/mkrle.py" bmp4.bmp bmprle4.bmp

gen png24.png    src.ppm  -define png:color-type=2 PNG:png24.png
gen png24i.png   src.ppm  -interlace PNG -define png:color-type=2 PNG:png24i.png
gen png48.png    src.ppm  -depth 16 -define png:color-type=2 -define png:bit-depth=16 PNG:png48.png
gen png8.png     pal.ppm  -define png:color-type=3 PNG:png8.png
gen png8i.png    pal.ppm  -interlace PNG -define png:color-type=3 PNG:png8i.png
gen png4.png     src.ppm  -colors 16 -define png:color-type=3 -define png:bit-depth=4 PNG:png4.png
gen png1.png     src.ppm  -monochrome -define png:color-type=3 -define png:bit-depth=1 PNG:png1.png
gen pnggray.png  gray.pgm -define png:color-type=0 PNG:pnggray.png
gen pngg16.png   gray.pgm -depth 16 -define png:color-type=0 -define png:bit-depth=16 PNG:pngg16.png
gen pngg4.png    gray.pgm -depth 4 -define png:color-type=0 -define png:bit-depth=4 PNG:pngg4.png
gen pngrgba.png  src.ppm  -alpha set -channel A -evaluate set 50% +channel -define png:color-type=6 PNG:pngrgba.png
gen pngga.png    gray.pgm -alpha set -channel A -evaluate set 50% +channel -define png:color-type=4 PNG:pngga.png

gen jpg.jpg      src.ppm  -quality 92 -sampling-factor 1x1 JPEG:jpg.jpg
gen jpg420.jpg   src.ppm  -quality 85 -sampling-factor 2x2 JPEG:jpg420.jpg
gen jpg422.jpg   src.ppm  -quality 85 -sampling-factor 2x1 JPEG:jpg422.jpg
gen jpgprog.jpg  src.ppm  -quality 85 -sampling-factor 2x2 -interlace JPEG JPEG:jpgprog.jpg
gen jpgpr444.jpg src.ppm  -quality 90 -sampling-factor 1x1 -interlace JPEG JPEG:jpgpr444.jpg
gen jpggray.jpg  gray.pgm -quality 90 JPEG:jpggray.jpg
gen jpggrayp.jpg gray.pgm -quality 90 -interlace JPEG JPEG:jpggrayp.jpg
gen jpgrst.jpg   src.ppm  -quality 85 -sampling-factor 2x2 -define jpeg:restart-interval=3 JPEG:jpgrst.jpg
gen jpgcmyk.jpg  src.ppm  -colorspace CMYK -quality 92 -sampling-factor 1x1 JPEG:jpgcmyk.jpg

gen tif.tif      src.ppm  -compress None TIFF:tif.tif
gen tiflzw.tif   src.ppm  -compress LZW TIFF:tiflzw.tif
gen tiflzwp.tif  src.ppm  -compress LZW -define tiff:predictor=2 TIFF:tiflzwp.tif
gen tifpack.tif  src.ppm  -compress RLE TIFF:tifpack.tif
gen tifzip.tif   src.ppm  -compress Zip TIFF:tifzip.tif
gen tifmm.tif    src.ppm  -compress LZW -endian MSB TIFF:tifmm.tif
gen tifpal.tif   pal.ppm  -type Palette -compress LZW TIFF:tifpal.tif
gen tifgray.tif  gray.pgm -compress LZW TIFF:tifgray.tif
gen tifbw.tif    src.ppm  -monochrome -compress None TIFF:tifbw.tif
gen tif48.tif    src.ppm  -depth 16 -compress LZW TIFF:tif48.tif
gen tiftile.tif  src.ppm  -compress LZW -define tiff:tile-geometry=64x64 TIFF:tiftile.tif
gen tifrgba.tif  src.ppm  -alpha set -channel A -evaluate set 50% +channel -compress LZW TIFF:tifrgba.tif
gen tifcmyk.tif  src.ppm  -colorspace CMYK -compress LZW TIFF:tifcmyk.tif
