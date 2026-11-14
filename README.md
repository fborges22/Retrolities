# Retrolities

Utilities for MS-DOS, Windows 3.x, Windows 9x and Windows NT/2000, cross-built
on Linux with the [Open Watcom v2](https://github.com/open-watcom/open-watcom-v2)
C/C++ toolchain running in Docker.

## Quick start

```sh
docker compose run --rm build              # build everything into ./build
docker compose run --rm build make dos     # one platform
docker compose run --rm build make help    # all make targets
docker compose run --rm build bash         # interactive Open Watcom shell
docker compose run --rm test               # smoke tests (DOSBox-X + Wine)
```

The images are built on first use (`docker compose build` to rebuild them).
Files are created as user 1000:1000; `export UID GID=$(id -g)` if yours differs.

## Utilities

| Name      | Platforms | Description                                                   |
|-----------|-----------|---------------------------------------------------------------|
| `picasso` | `win16` `win9x` `winnt` | Windows 95 style paint program for Windows 3.1, 9x and NT/2000 (see below) |
| `imgconv` | all       | converts pictures between BMP, PNG, JPEG and TIFF (see below) |
| `hello`   | all       | sample console program                                        |
| `hellowin`| Windows   | sample GUI program with resources                             |

## Platforms

| Platform | Target                  | Compiler | Defaults                         | Macro         |
|----------|-------------------------|----------|----------------------------------|---------------|
| `dos`    | MS-DOS, real mode       | `wcc`    | 8086, small model (`t` = `.COM`) | `RETRO_DOS`   |
| `win16`  | Windows 3.x             | `wcc`    | 286, large model                 | `RETRO_WIN16` |
| `win9x`  | Windows 95/98/ME        | `wcc386` | 386, ANSI API                    | `RETRO_WIN9X` |
| `winnt`  | Windows NT 4.0 / 2000   | `wcc386` | Pentium-tuned, optional UNICODE  | `RETRO_WINNT` |

Every source file is also given `RETRO_BITS` (16 or 32) and `RETRO_CONSOLE` or
`RETRO_GUI`. `lib/retro/retro.h` adds `RETRO_WINDOWS` and `RETRO_WIN32`.

Output goes to `build/<platform>/bin/NAME.EXE` (upper-case 8.3 names); objects,
map files and libraries are under `build/<platform>/obj` and `.../lib`.

## Layout

```
docker/build/   Open Watcom image (also ships mTCP sources in $MTCP)
docker/test/    DOSBox-X + Wine image
mk/rules.mk     builds one module for one platform
mk/platform/    per-platform compiler / linker settings
lib/<name>/     shared libraries: lib/retro (linked into everything), lib/imgio (image codecs)
src/<name>/     one directory per utility
tests/          smoke-test runner
tools/          helper scripts
```

## Adding a utility

```sh
docker compose run --rm build make new NAME=mytool            # console
docker compose run --rm build make new NAME=mytool TYPE=gui   # Windows GUI
```

This creates `src/mytool/` with a `project.mk` and a source file. Any directory
under `src/` holding a `project.mk` is built automatically.

### project.mk variables

| Variable     | Default              | Meaning                                              |
|--------------|----------------------|------------------------------------------------------|
| `NAME`       | directory name       | output base name (max. 8 characters)                 |
| `TYPE`       | `console`            | `console` or `gui`                                   |
| `PLATFORMS`  | all four             | platforms this utility is built for                  |
| `SRCS`       | all `*.c *.cpp *.asm`| source files, relative to the project directory      |
| `RES`        | none                 | resource script (`.rc`), Windows platforms only; it is rebuilt when a header or a file in `res/` changes |
| `LIBS`       | `retro`              | libraries from `lib/` to link                        |
| `UNICODE`    | off                  | `1` defines `UNICODE`/`_UNICODE` on `winnt` only     |
| `CFLAGS` `CXXFLAGS` `AFLAGS` `RCFLAGS` `LDFLAGS` | | extra tool options                |

In a library's `project.mk`, `CFLAGS` and `CPU_<platform>` apply to the library itself.

Per-platform variants: `SRCS_<platform>`, `CFLAGS_<platform>` (likewise for the
other flag variables), `MODEL_<platform>` (memory model for `dos` / `win16`) and
`CPU_<platform>` (e.g. `CPU_dos := 3`). `LDFLAGS` takes `wlink` directives,
e.g. `LDFLAGS_dos := option stack=8k`.

`TYPE := console` on `win16` uses Open Watcom's default windowing system, so
plain `printf` programs run in a text window under Windows 3.x.

A library is a directory under `lib/` whose `project.mk` sets `KIND := lib`;
its directory is added to the include path of the programs that list it in
`LIBS`. Libraries are built once per platform and memory model.

## Tests

`docker compose run --rm test` checks the executable format of every binary
and runs each console utility that has a `src/<name>/test.expect` file
(DOSBox-X for `dos`, Wine for `win9x` / `winnt`); every line of that file must
appear in the program's output. Win16 and GUI programs are format-checked only.

A project can add its own tests in `src/<name>/test.sh`, which is sourced by
`tests/run.sh` (helpers: `ok`, `bad`, `dos_run`, `$tmp`). `imgconv` and
`picasso` use this to convert about 45 reference pictures generated with
ImageMagick (`tests/images/make.sh`) and compare the results with ImageMagick's
own reading of them: `IMGCONV.EXE` as 16-bit code under DOSBox-X and as Win32
code under Wine, and `PICASSO.EXE /convert` as a Win16 and as a Win32 program
under Wine.

### GUI programs

`tests/gui.sh` runs a Windows program under Wine on a headless X server, so it
can be driven with the mouse and keyboard and captured to `build/shots/*.png`:

```sh
docker compose run --rm test tests/gui.sh build/win16/bin/PICASSO.EXE                              # one screenshot
docker compose run --rm test tests/gui.sh build/win16/bin/PICASSO.EXE src/picasso/tests/tour.sh    # scripted session
```

The script is sourced with the helpers `shot`, `click`, `dclick`, `drag` and
`key` available (see the header of `tests/gui.sh`).

## Picasso

`build/win16/bin/PICASSO.EXE` is a single-file paint program for Windows 3.1
and its Win32 ports are `build/win9x/bin/PICASSO.EXE` for Windows 95/98/ME and
`build/winnt/bin/PICASSO.EXE` for Windows NT 4.0 / 2000 (Pentium-tuned), all
built from the same sources. The Win32 ports use the ANSI API, with long
file names and the Explorer-style file dialogs. Both have the look and feel
of Windows 95 Paint: 3-D toolbar with tool tips, tool
box with per-tool options, color box, status bar with hints, cursor position
and sizes, and custom tool cursors.

- **Tools:** free-form and rectangular selection, eraser, fill, color picker,
  magnifier, pencil, brush (12 shapes), airbrush, text, line, curve, rectangle,
  polygon, ellipse and rounded rectangle. The left button draws with the
  foreground color, the right button with the background color; Shift
  constrains lines and shapes.
- **Selections:** move, Ctrl+drag to copy, cut / copy / paste through the
  Clipboard, opaque or transparent, flip, rotate and invert.
- **Picture:** three levels of undo / repeat, flip / rotate / invert, resize
  by dragging the handles or with *Image > Attributes*, zoom up to 800% with a
  pixel grid.
- **Files:** opens BMP (including RLE compressed, 16 and 32-bit and OS/2
  bitmaps), PNG, JPEG (baseline and progressive) and TIFF; saves BMP
  (monochrome, 16, 256 colors, 24-bit), PNG, JPEG and TIFF - the type is chosen
  in *Save As* or simply by the extension typed. Prints, accepts a file name on
  the command line and files dropped from File Manager.
- **256-color displays:** Picasso manages its own palette - the colors of the
  picture (true-color pictures are reduced to an optimal 236-color palette)
  plus the colors you draw with - and copies it to the Clipboard.
- `PICASSO.EXE /convert input output` converts a file without opening a window.

Sources are in `src/picasso`: `main.c` (frame, toolbars, menus, dialogs),
`canvas.c` (canvas window and tools), `image.c` (bitmap, undo, selection,
transforms), `palette.c` (256-color displays), `file.c` (files through
`lib/imgio`, printing). The few differences between the Win16 and Win32 APIs
are wrapped by macros at the top of `picasso.h`. The artwork in `res/` is generated by
`python3 src/picasso/res/mkart.py`; `tests/tour*.sh` are scripted GUI sessions.

Notes:

- Pictures can be up to 16000 x 16000 pixels; in practice the limit is the
  memory Windows can give to one bitmap (about 16 MB in Windows 3.1).
- JPEG files are saved with quality 90; set `JpegQuality=` (1-100) in a
  `[Picasso]` section of `WIN.INI` to change it. Below 90 the color is stored
  at half resolution (4:2:0).
- Transparency in PNG and TIFF files is blended onto white; 16-bit samples are
  reduced to 8 bits.

## imgconv and lib/imgio

`imgconv input output [n]` converts between BMP, PNG, JPEG and TIFF on every
platform (`n` = JPEG quality or BMP color depth), e.g. `IMGCONV PHOTO.JPG PHOTO.BMP 8`.

`lib/imgio` is the codec library behind it and Picasso, written from scratch
for 16-bit targets (see `lib/imgio/imgio.h` for the API):

| Format | Reads                                                              | Writes                         |
|--------|--------------------------------------------------------------------|--------------------------------|
| BMP    | 1/4/8/16/24/32-bit, RLE4/RLE8, bit fields, OS/2, top-down          | 1/4/8/24-bit                   |
| PNG    | all color types and bit depths, interlaced, transparency           | 8-bit RGB or palette           |
| JPEG   | baseline, extended and progressive Huffman; gray, YCbCr, CMYK/YCCK | baseline, 4:4:4 or 4:2:0, gray |
| TIFF   | gray, palette, RGB(A), CMYK; strips or tiles; none, PackBits, LZW, Deflate | LZW, RGB or palette    |

Pictures are streamed row by row through callbacks, so they are never held in
memory by the library and no allocation exceeds 64 KB. Pictures with 256
colors or fewer are saved with a palette; saving true color as an 8-bit BMP
uses median-cut color reduction. Not supported: JPEG with arithmetic coding or
12-bit samples, TIFF with JPEG or CCITT fax compression or separate planes.
The library is built for 8086/286 code by default; adding `CPU_win16 := 3` to
`lib/imgio/project.mk` makes the 32-bit arithmetic of the JPEG codec
considerably faster on 386 and later machines.
