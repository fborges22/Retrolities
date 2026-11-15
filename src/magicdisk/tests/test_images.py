#!/usr/bin/env python3
"""Integration tests with independently formatted FAT volumes and corruptions."""
import pathlib
import struct
import subprocess
import sys
import tempfile

EXE = pathlib.Path(sys.argv[1]).resolve()

def run(path, *args, code=0, contains=None):
    p = subprocess.run([str(EXE), str(path), *args], capture_output=True, text=True)
    assert p.returncode == code, p.stdout + p.stderr
    if contains:
        assert contains in p.stdout, p.stdout
    return p.stdout

with tempfile.TemporaryDirectory() as tmp:
    root = pathlib.Path(tmp)
    for bits, size in [(12, 1440*1024), (16, 32*1024*1024), (32, 64*1024*1024)]:
        path = root / f'fat{bits}.img'
        with path.open('wb') as f:
            f.truncate(size)
        subprocess.run(['mkfs.fat', '-F', str(bits), str(path)], check=True, capture_output=True)
        run(path, contains=f'FAT{bits}')
        data = bytearray(path.read_bytes())
        reserved = struct.unpack_from('<H', data, 14)[0]
        fatsz = struct.unpack_from('<H', data, 22)[0] or struct.unpack_from('<I', data, 36)[0]
        copies = data[16]
        def fat(c, value):
            for copy in range(copies):
                off = (reserved + copy*fatsz)*512
                if bits == 12:
                    off += c+c//2
                    old = struct.unpack_from('<H', data, off)[0]
                    val = (old & 15) | (value << 4) if c & 1 else (old & 0xf000) | value
                    struct.pack_into('<H', data, off, val)
                else:
                    struct.pack_into('<H' if bits == 16 else '<I', data, off+c*(bits//8), value)
        eoc = (1 << bits)-1 if bits != 32 else 0xfffffff
        # FAT12 entry 341 straddles a sector boundary.
        fat(341, eoc)
        path.write_bytes(data)
        run(path, code=1, contains='LOST_CLUSTERS')
        fat(341, 0)
        roots = (reserved+copies*fatsz)*512
        if bits == 32:
            roots += (struct.unpack_from('<I', data, 44)[0]-2)*data[13]*512
        data[roots:roots+11] = b'EXAMPLE TXT'
        data[roots+11] = 0x20
        struct.pack_into('<H', data, roots+26, 341)
        struct.pack_into('<I', data, roots+28, 512*data[13])
        fat(341, eoc)
        if bits == 32:
            fsinfo = struct.unpack_from('<H', data, 48)[0]*512
            struct.pack_into('<I', data, fsinfo+488, 0xffffffff)
        path.write_bytes(data)
        run(path, contains='1 files')
        fat(341, 341)
        path.write_bytes(data)
        run(path, code=1, contains='CHAIN_SHARED')
        fat(341, eoc)
        data[(reserved+fatsz)*512+7] ^= 1
        path.write_bytes(data)
        run(path, code=1, contains='FAT_MIRROR')
        data[13] = 3
        path.write_bytes(data)
        run(path, code=2, contains='Invalid FAT geometry')
    # Partition parsing is separate from volume mounting.
    disk = bytearray(512*100)
    disk[510:512] = b'\x55\xaa'
    def entry(where, typ, start, size):
        disk[where+4] = typ
        struct.pack_into('<II', disk, where+8, start, size)
    entry(446, 6, 1, 20)
    entry(462, 15, 30, 70)
    disk[30*512+510:30*512+512] = b'\x55\xaa'
    entry(30*512+446, 6, 1, 20)
    p = root/'mbr.img'
    p.write_bytes(disk)
    run(p, '--list', contains='LBA         31')
    entry(30*512+462, 15, 30, 40)
    disk[60*512+510:60*512+512] = b'\x55\xaa'
    entry(60*512+462, 15, 30, 40)
    p.write_bytes(disk)
    run(p, '--list', code=2, contains='Loop in extended')
    entry(462, 6, 10, 20)
    p.write_bytes(disk)
    run(p, '--list', code=2, contains='Overlapping')
print('PASS: FAT12/16/32 clean, file, lost, loop, mirrors, BPB, MBR and EBR tests')
