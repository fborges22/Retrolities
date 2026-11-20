# Retrolities

Cross-compiled utilities for MS-DOS, OS/2 2.0+, Windows 3.x, Windows 9x,
Windows NT/2000 and x64 Windows. The project uses Open Watcom v2 and MinGW-w64
in Docker.

## Quick start

```sh
docker compose run --rm build                    # build all utilities and Arrow targets
docker compose run --rm build make dos           # build one platform
docker compose run --rm build make win64         # Arrow and Picasso for x64 Windows
docker compose run --rm build make help          # list targets
docker compose run --rm test                     # format, smoke and protocol tests
docker compose run --rm build bash               # open the build environment
```

Docker images are built on first use; run `docker compose build` to rebuild
them. Build outputs are written to `build/`. Files are created as user
`1000:1000`; if needed, set `UID` and `GID` to your own values before running
Compose.

### Build and package

Run the Python helper from the host to build all projects, including Arrow's
six platform targets, and create a single ZIP:

```sh
python3 tools/build_pack.py
python3 tools/build_pack.py --output build/release.zip
```

The helper builds the Docker build image as needed and runs `make all`.
The default archive is `build/retrolities.zip`. It contains only `.EXE` and
`.COM` files from `build/<platform>/bin/`, all at the ZIP root. Unique 8.3
names (such as `ARROWD.EXE`) are preserved. Duplicate names receive short
platform suffixes: `D`, `2`, `16`, `9X`, `NT`, `64`; stems are shortened to
keep 8.3 format (for example, `HELLO16.EXE` and `PICASSNT.EXE`). Any remaining
collisions receive a numeric suffix. Longer names are shortened while
retaining platform suffixes (for example, `TOOLNAME64.EXE` becomes
`TOOLNA64.EXE`). Renaming affects only ZIP entries, not
the original build files.

Existing build files are retained; no clean is performed. The ZIP is
replaced only after both the build and packaging succeed.

| Option | Purpose |
|---|---|
| `-o`, `--output PATH` | ZIP destination |
| `-p`, `--platforms PLATFORM ...` | Build and package selected platforms: `dos`, `os2`, `win16`, `win9x`, `winnt`, `win64` |
| `-e`, `--executables GLOB ...` | Package selected original filenames, case-insensitively; quote wildcards |
| `--skip-build` | Package existing executables without Docker or compilers |
| `--local` | Build with the locally configured toolchain instead of Docker |
| `--long-names` | Use full platform suffixes for duplicates, such as `HELLO-win16.EXE` |
| `--compression deflated\|stored` | Select compression (default: `deflated`) |
| `--compression-level 0-9` | Deflate level (default: `9`) |
| `-v`, `--verbose` | Show full compiler/linker commands |

```sh
python3 tools/build_pack.py --platforms dos win16 --output build/retro.zip
python3 tools/build_pack.py --skip-build --executables 'ARROW*.EXE'
python3 tools/build_pack.py --skip-build --long-names --compression stored
```

Executable filters affect packaging only; all projects for the selected
platforms are still built. `--skip-build` can include stale binaries left
by earlier builds.

Inside the build container (or with the toolchains configured locally), use:

```sh
python3 tools/build_pack.py --local
```

## Utilities

| Utility | Platforms | Description |
|---|---|---|
| [Arrow](#arrow) | DOS, OS/2, Windows 3.x/9x/NT, x64 Windows | TCP throughput tester |
| [Picasso](#picasso) | Windows 3.x/9x/NT, Windows 11 x64 | Windows 95-style paint program |
| [imgconv](#imgconv-and-libimgio) | All Open Watcom platforms | BMP, PNG, JPEG and TIFF converter |
| [Polaris](src/polaris/README.md) | PC BIOS (386+), installer for DOS | Boot manager: themed TUI, partition hide/activate, drive swap, El Torito CD and ATAPI ZIP/LS-120 boot without BIOS support |
| [MagicDisk](src/magicdisk/README.md) | DOS (386 protected mode) | FAT12/16/32 diagnosis and journaled repair: automatic fix, cross-links, lost chains, surface/write tests, partition recovery |
| `hello` | All Open Watcom platforms | Console example |
| `hellowin` | Windows | GUI example with resources |

## Arrow

Arrow is an independent TCP throughput tester for DOS, OS/2 2.0+, Windows
3.x, Windows 9x, Windows NT/2000 and x64 Windows. It supports client and server
modes, parallel streams and reverse tests using the iperf 3.x control protocol. The
default DOS build uses mTCP; an opt-in `ARROWM.EXE` build uses MSCLIENT 3.0's
resident TCP/IP sockets. Windows builds use Winsock. Arrow is not affiliated with
or endorsed by the iperf3 project.

Build all Arrow targets as part of the default build, or select one target:

```sh
docker compose run --rm build make PROJECTS=arrow
docker compose run --rm build make dos
docker compose run --rm build make os2
docker compose run --rm build make win16
docker compose run --rm build make win9x
docker compose run --rm build make winnt
docker compose run --rm build make win64
```

| Executable | Platform |
|---|---|
| `ARROWD.EXE` | MS-DOS / FreeDOS |
| `ARROWM.EXE` | MS-DOS / MSCLIENT 3.0 TCP/IP (opt-in SDK build) |
| `ARROWO.EXE` | OS/2 2.0+ |
| `ARROW16.EXE` | Windows 3.x |
| `ARROW32.EXE` | Windows 95/98/ME |
| `ARROWN.EXE` | Windows NT 4.0 / 2000 |
| `ARROW64.EXE` | x64 Windows |

The binaries are placed in `build/<platform>/bin/`. See
[`src/arrow/README.md`](src/arrow/README.md) for usage, platform details
and protocol test helpers. Arrow source uses the repository MIT license; the
`ARROWD.EXE` also links mTCP, which is licensed under GPLv3. Build `ARROWM.EXE`
with `make msclient MSSDK=<SDK root>`; see Arrow's README for container builds,
resident sockets setup and the separate SDK licensing requirements.

## Picasso

`PICASSO.EXE` is a paint program for Windows 3.1, Windows 95/98/ME and Windows
NT/2000. `PICASSO64.EXE` is its native x64 build for Windows 11. Both use a
Windows 95-style interface with
a 3-D toolbar, tool tips, tool options, color palette and status bar.

- **Drawing:** selection, eraser, fill, color picker, magnifier, pencil, brush,
  airbrush, text, line, curve, rectangle, polygon, ellipse and rounded rectangle.
- **Editing:** move or copy selections; cut, copy and paste; undo and repeat;
  flip, rotate, invert and resize; zoom to 800% with a pixel grid.
- **Files and printing:** opens BMP, PNG, JPEG and TIFF; saves BMP, PNG, JPEG
  and TIFF. Accepts files from the command line and File Manager.
- **256-color displays:** maintains an optimized palette for the picture and
  drawing colors.

`PICASSO.EXE /convert input output` converts an image without opening the GUI.
Source files are in `src/picasso`; artwork in `res/` is generated by
`python3 src/picasso/res/mkart.py`.

Pictures can be up to 16000 x 16000 pixels, subject to available Windows
memory. JPEG quality defaults to 90 and can be changed with `JpegQuality=` in
the `[Picasso]` section of `WIN.INI`. PNG and TIFF transparency is blended onto
white; 16-bit samples are reduced to 8 bits.

## imgconv and lib/imgio

`imgconv input output [n]` converts BMP, PNG, JPEG and TIFF files. The optional
`n` sets JPEG quality or BMP color depth; for example:

```sh
IMGCONV PHOTO.JPG PHOTO.BMP 8
```

`lib/imgio` is the streaming codec library used by imgconv and Picasso. It
processes images row by row, so pictures are not held entirely in memory and
no allocation exceeds 64 KB.

| Format | Reads | Writes |
|---|---|---|
| BMP | 1/4/8/16/24/32-bit, RLE4/RLE8, bit fields, OS/2, top-down | 1/4/8/24-bit |
| PNG | All color types and bit depths, interlacing, transparency | 8-bit RGB or palette |
| JPEG | Baseline, extended and progressive Huffman; gray, YCbCr, CMYK/YCCK | Baseline, 4:4:4 or 4:2:0, gray |
| TIFF | Gray, palette, RGB(A), CMYK; strips or tiles; none, PackBits, LZW, Deflate | LZW, RGB or palette |

Unsupported formats include arithmetic-coded or 12-bit JPEG, and TIFF with
JPEG, CCITT fax compression or separate planes. The library defaults to 8086/
286 code; setting `CPU_win16 := 3` in `lib/imgio/project.mk` enables faster
32-bit JPEG arithmetic on 386 and later CPUs.

## Build system

### Platforms

The common Open Watcom build system supports these platforms:

| Target | Operating system | Compiler | Defaults |
|---|---|---|---|
| `dos` | MS-DOS, real mode | `wcc` | 8086, small model (`t` produces `.COM`) |
| `win16` | Windows 3.x | `wcc` | 286, large model |
| `win9x` | Windows 95/98/ME | `wcc386` | 386, ANSI API |
| `winnt` | Windows NT 4.0 / 2000 | `wcc386` | Pentium-tuned, optional UNICODE |

Arrow and Picasso have x64 Windows targets, `win64`, built with MinGW-w64 in
the same build image. Standard outputs use `build/<platform>/bin/NAME.EXE`; object
files, maps and libraries are under `build/<platform>/obj` and `.../lib`.

### Repository layout

```text
docker/build/   Open Watcom, MinGW-w64 and mTCP build image
docker/test/    DOSBox-X, Wine and protocol test tools
mk/rules.mk     common build rules for one module and platform
mk/platform/    compiler and linker settings per platform
lib/            shared libraries (retro platform helpers and imgio codecs)
src/            utilities, including Arrow and Picasso
tests/          shared smoke-test and GUI-test runners
tools/          project scaffolding and helper scripts
```

Arrow and Picasso have dedicated makefiles for their MinGW-w64 x64 targets;
the top-level Makefile includes them in `win64` builds.

### Adding a utility

Create a console or GUI project with:

```sh
docker compose run --rm build make new NAME=mytool
docker compose run --rm build make new NAME=mygui TYPE=gui
```

The scaffold creates `src/<name>/project.mk` and a source file. Directories
under `src/` with a `project.mk` are discovered and built automatically.

| Variable | Default | Purpose |
|---|---|---|
| `NAME` | Directory name | Output base name, up to 8 characters |
| `TYPE` | `console` | `console` or `gui` |
| `PLATFORMS` | All four Open Watcom targets | Platforms to build |
| `SRCS` | `*.c`, `*.cpp`, `*.asm` in the project directory | Source files |
| `RES` | None | Windows resource script (`.rc`) |
| `LIBS` | `retro` | Libraries from `lib/` to link |
| `UNICODE` | Off | Define Unicode macros on `winnt` |
| `CFLAGS`, `CXXFLAGS`, `AFLAGS`, `RCFLAGS`, `LDFLAGS` | — | Additional tool options |

Per-platform variants such as `SRCS_<platform>`, `CFLAGS_<platform>`,
`MODEL_<platform>` and `CPU_<platform>` override defaults. `LDFLAGS` accepts
`wlink` directives, for example `LDFLAGS_dos := option stack=8k`. A library
sets `KIND := lib` in its `project.mk`; its directory is added to the include
path for projects that list it in `LIBS`.

For common-rule projects, each source is given `RETRO_BITS` and either
`RETRO_CONSOLE` or `RETRO_GUI`. `lib/retro/retro.h` provides `RETRO_WINDOWS`
and `RETRO_WIN32`. On Win16, console projects use Open Watcom's default text
window so standard I/O remains available.

## Tests

Run the test container after building:

```sh
docker compose run --rm test
```

The runner checks executable formats, runs console projects with a
`src/<name>/test.expect` file (DOSBox-X for DOS, Wine for Win9x/NT), and sources
any `src/<name>/test.sh` project tests. Arrow's test exercises client and
server interoperability with a compatible protocol peer. Picasso and imgconv
also test image conversion against reference files generated with ImageMagick.

Win16 and GUI apps are format-checked by default. To run a GUI under Wine on a
headless X server and capture a screenshot:

```sh
docker compose run --rm test tests/gui.sh build/win16/bin/PICASSO.EXE
docker compose run --rm test tests/gui.sh \
  build/win16/bin/PICASSO.EXE src/picasso/tests/tour.sh
```

The GUI script provides `shot`, `click`, `dclick`, `drag` and `key` helpers to
the scripted session.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
