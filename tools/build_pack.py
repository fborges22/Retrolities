#!/usr/bin/env python3
"""Build utilities and package executables at the root of a ZIP."""
import argparse
from collections import Counter
import fnmatch
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parent.parent
PLATFORMS = ("dos", "os2", "win16", "win9x", "winnt", "win64")
SUFFIXES = {
    "dos": "D", "os2": "2", "win16": "16",
    "win9x": "9X", "winnt": "NT", "win64": "64",
}


def archive_entries(files, long_names=False):
    counts = Counter(path.name.casefold() for path in files)
    entries = {}
    used = set()

    # Reserve unique 8.3 names before allocating shortened platform variants.
    for path in files:
        if counts[path.name.casefold()] == 1 and (
            long_names or re.fullmatch(r"[A-Za-z0-9_-]{1,8}\.[A-Za-z0-9]{1,3}", path.name)
        ):
            entries[path] = path.name
            used.add(path.name.casefold())

    for path in files:
        if path in entries:
            continue
        platform = path.parent.parent.name
        stem = re.sub(r"[^A-Z0-9_-]", "_", path.stem.upper())
        extension = path.suffix.upper()
        suffix = SUFFIXES[platform]
        if counts[path.name.casefold()] == 1:
            existing_suffix = "32" if platform == "win9x" else suffix
            if stem.endswith(existing_suffix):
                stem = stem[:-len(existing_suffix)]
                suffix = existing_suffix
        number = 0
        while True:
            if long_names:
                tail = f"-{platform}" + (f"-{number}" if number else "")
                name = f"{path.stem}{tail}{path.suffix}"
            else:
                tail = suffix + (str(number) if number else "")
                if len(tail) >= 8:
                    raise ValueError(f"Cannot allocate a unique 8.3 name for {path}")
                name = f"{stem[:8 - len(tail)]}{tail}{extension}"
            if name.casefold() not in used:
                break
            number += 1
        entries[path] = name
        used.add(name.casefold())
    return [(path, entries[path]) for path in files]


def pack_artifacts(
    root, output, platforms=PLATFORMS, patterns=(), long_names=False,
    compression=zipfile.ZIP_DEFLATED, compression_level=9,
):
    files = []
    for platform in platforms:
        for path in sorted((root / "build" / platform / "bin").glob("*")):
            if (
                path.is_file() and not path.is_symlink()
                and path.suffix.upper() in (".EXE", ".COM")
                and path.resolve() != output
                and (not patterns or any(
                    fnmatch.fnmatchcase(path.name.upper(), pattern.upper())
                    for pattern in patterns
                ))
            ):
                files.append(path)
    if not files:
        raise ValueError("No executables found for the selected platforms and filters")
    entries = archive_entries(files, long_names)

    output.parent.mkdir(parents=True, exist_ok=True)
    # Replace the destination only after the entire archive has been written.
    with tempfile.NamedTemporaryFile(
        dir=output.parent, prefix=".retrolities-", suffix=".zip", delete=False
    ) as temporary:
        temporary_path = Path(temporary.name)
    try:
        with zipfile.ZipFile(
            temporary_path, "w", compression=compression,
            compresslevel=compression_level,
        ) as archive:
            for path, name in entries:
                archive.write(path, name)
        temporary_path.replace(output)
    finally:
        temporary_path.unlink(missing_ok=True)
    return len(files)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--local", action="store_true",
        help="run make directly (requires the configured build toolchain)",
    )
    parser.add_argument(
        "-o", "--output", type=Path, default=ROOT / "build" / "retrolities.zip",
        help="ZIP destination (default: repository build/retrolities.zip)",
    )
    parser.add_argument(
        "-p", "--platforms", nargs="+", choices=PLATFORMS,
        help="platforms to build and package (default: all five)",
    )
    parser.add_argument(
        "-e", "--executables", nargs="+", metavar="GLOB",
        help="package only matching original filenames (case-insensitive; quote globs)",
    )
    parser.add_argument(
        "--skip-build", action="store_true",
        help="package existing executables without invoking the build",
    )
    parser.add_argument(
        "--long-names", action="store_true",
        help="use full platform suffixes for duplicate names (e.g. HELLO-win16.EXE)",
    )
    parser.add_argument(
        "--compression", choices=("deflated", "stored"), default="deflated",
        help="ZIP compression method (default: deflated)",
    )
    parser.add_argument(
        "--compression-level", type=int, choices=range(10), default=9,
        metavar="0-9", help="deflate compression level (default: 9; ignored for stored)",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true",
        help="show full compiler and linker commands",
    )
    args = parser.parse_args(argv)
    output = args.output.resolve()
    platforms = tuple(p for p in PLATFORMS if not args.platforms or p in args.platforms)
    command = ["make", *(platforms if args.platforms else ("all",))]
    if args.verbose:
        command.append("V=1")
    env = os.environ.copy()
    if not args.local:
        command = [
            "docker", "compose", "run", "--rm", "--build", "-T",
            "build", *command,
        ]
        if hasattr(os, "getuid"):
            env.setdefault("UID", str(os.getuid()))
            env.setdefault("GID", str(os.getgid()))

    try:
        if not args.skip_build:
            subprocess.run(command, cwd=ROOT, env=env, check=True)
        count = pack_artifacts(
            ROOT, output, platforms, args.executables, args.long_names,
            zipfile.ZIP_DEFLATED if args.compression == "deflated" else zipfile.ZIP_STORED,
            args.compression_level,
        )
    except subprocess.CalledProcessError as error:
        print(f"Build failed (exit {error.returncode}); ZIP not updated.", file=sys.stderr)
        return 1
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        print(f"Build/package failed: {error}", file=sys.stderr)
        return 1
    print(f"Packed {count} executables into {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
