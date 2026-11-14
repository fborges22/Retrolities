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
lib/<name>/     shared libraries (lib/retro is linked into everything)
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
| `RES`        | none                 | resource script (`.rc`), Windows platforms only      |
| `LIBS`       | `retro`              | libraries from `lib/` to link                        |
| `UNICODE`    | off                  | `1` defines `UNICODE`/`_UNICODE` on `winnt` only     |
| `CFLAGS` `CXXFLAGS` `AFLAGS` `RCFLAGS` `LDFLAGS` | | extra tool options                |

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
