# Image file codecs (BMP, PNG, JPEG, TIFF) written for 16-bit targets:
# no allocation above 64 KB, pictures are streamed row by row.
NAME := imgio
KIND := lib
LIBS :=
# Optimise for speed and leave out the per-call stack checks: decoding is
# dominated by small functions called for every byte.
CFLAGS := -oxt
