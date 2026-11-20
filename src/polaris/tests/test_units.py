#!/usr/bin/env python3
"""Host-side checks for Polaris: PLZ codec, built images and on-disk layout."""
import os
import pathlib
import random
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools'))
import plimage  # noqa: E402
import plz      # noqa: E402

BUILD = HERE.parents[2] / 'build' / 'polaris'


def check(cond, what):
    if not cond:
        raise SystemExit('FAIL: ' + what)
    print('ok  ', what)


def main():
    rnd = random.Random(7)
    for n in [0, 1, 2, 3, 100, 5000]:
        data = bytes(rnd.choice(b'ABCD\0\xff') for _ in range(n)) + os.urandom(n // 10)
        if not data:
            data = b'x'
        check(plz.unpack(plz.pack(data)) == data, f'PLZ round trip ({len(data)} bytes)')
    s1 = (BUILD / 'stage1.bin').read_bytes()
    s2 = (BUILD / 'stage2.bin').read_bytes()
    check(len(s1) == 512 and s1[510:] == b'\x55\xaa', 'stage 1 is one signed sector')
    check(s1[0x1A0:0x1A4] == b'PLRS', 'stage 1 info block')
    check(s2[3:11] == b'POLARIS2', 'stage 2 signature')
    check((len(s2) + 511) // 512 <= plimage.MAX_STAGE2, 'stage 2 fits sectors 5-62')
    cfg = plimage.pack_config({'entries': [{'name': 'Test', 'kind': 'part', 'lba': 63}]})
    check(len(cfg) == 2048 and plimage.checksum(cfg) == struct.unpack_from('<H', cfg, 22)[0],
          'configuration record and checksum')
    with tempfile.TemporaryDirectory() as t:
        img = pathlib.Path(t) / 'hd.img'
        with open(img, 'wb') as f:
            f.truncate(32 * 1024 * 1024)
        mbr = bytearray(512)
        mbr[0x1BE:0x1CE] = bytes([0x80, 1, 1, 0, 6, 15, 63, 0]) + struct.pack('<II', 63, 65000)
        mbr[0x1B8:0x1BC] = b'SIG!'
        mbr[0:4] = b'OLD!'
        mbr[510:] = b'\x55\xaa'
        with open(img, 'r+b') as f:
            f.write(mbr)
        plimage.install_hdd(img, {'entries': []})
        d = img.read_bytes()
        check(d[0x1A0:0x1A4] == b'PLRS' and d[0x1B8:0x200] == mbr[0x1B8:0x200],
              'install keeps disk signature and partition table')
        check(d[512 + 2048 - 512:512 + 2048 - 508] == b'OLD!', 'original MBR saved in the configuration')
        plimage.uninstall(img)
        d = img.read_bytes()
        check(d[:512] == bytes(mbr), 'uninstall restores the original MBR')
        if subprocess.run(['which', 'mkfs.fat'], capture_output=True).returncode == 0:
            fl = pathlib.Path(t) / 'fd.img'
            plimage.make_floppy(fl, {'entries': []})
            b = fl.read_bytes()
            check(b[0x0E:0x10] == b'\x40\x00' and b[0x1A0:0x1A4] == b'PLRS',
                  'floppy: FAT12 with 64 reserved sectors and Polaris')


if __name__ == '__main__':
    main()
