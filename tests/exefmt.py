#!/usr/bin/env python3
"""Print the executable format of a file: com, mz, ne, pe-console or pe-gui."""
import struct
import sys


def exe_format(path):
    data = open(path, "rb").read()
    if data[:2] != b"MZ":
        return "com" if path.upper().endswith(".COM") else "unknown"
    if len(data) < 0x40:
        return "mz"
    off = struct.unpack_from("<I", data, 0x3C)[0]
    sig = data[off:off + 4]
    if sig[:2] == b"NE":
        return "ne"
    if sig == b"PE\0\0":
        subsystem = struct.unpack_from("<H", data, off + 24 + 68)[0]
        return {2: "pe-gui", 3: "pe-console"}.get(subsystem, "pe-unknown")
    return "mz"


if __name__ == "__main__":
    print(exe_format(sys.argv[1]))
