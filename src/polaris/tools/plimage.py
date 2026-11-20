#!/usr/bin/env python3
"""Polaris image tool: install Polaris into disk/floppy images from a host.

    plimage.py floppy OUT.img [--config menu.json]      new Polaris floppy (FAT12)
    plimage.py hdd IMAGE [--config menu.json]           install into a disk image
    plimage.py config IMAGE                             print the stored menu
    plimage.py uninstall IMAGE                          restore the original MBR

The structures mirror src/polaris/polaris.h.  A menu JSON file looks like:

    {"timeout": 10, "theme": 0, "title": "Choose an operating system",
     "entries": [{"name": "FreeDOS", "kind": "part", "drive": 128, "pindex": 1,
                  "lba": 2048, "sectors": 1000000, "ptype": 6, "icon": "dos",
                  "hotkey": "1", "flags": ["active"]}]}
"""
import argparse
import json
import pathlib
import struct
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BUILD = ROOT / 'build' / 'polaris'

LBA_CONFIG, CONFIG_SECTS, LBA_STAGE2, MAX_STAGE2 = 1, 4, 5, 58
FLOPPY_RESERVED = 64
INFO_OFS = 0x1A0
MAX_ENTRIES = 20

KINDS = {'none': 0, 'part': 1, 'disk': 2, 'floppy': 3, 'cdrom': 4, 'atapidisk': 5,
         'biosnext': 6, 'reboot': 7, 'poweroff': 8, 'separator': 9}
ICONS = ['disk', 'dos', 'win9x', 'winnt', 'os2', 'linux', 'bsd', 'cdrom', 'floppy', 'zip',
         'net', 'power', 'reboot', 'gear', 'star', 'lock']
FLAGS = {'active': 1, 'swap': 2, 'fixbpb': 4, 'password': 8, 'unhide': 16, 'nocheck': 32}
CFLAGS = {'remember': 1, 'quiet': 2, 'nostars': 4, 'nofont': 8, 'sound': 16}
EMU = {'auto': 0, 'none': 1, 'floppy': 2, 'hdd': 3}


def pl_hash(s):
    h = 0x5A17
    for ch in s:
        c = ord(ch.upper()) if 'a' <= ch <= 'z' else ord(ch)
        h ^= c
        h = ((h << 5) | (h >> 11)) & 0xFFFF
        h = (h * 31 + 0x2B) & 0xFFFF
    return h or 1


def pack_entry(e):
    name = e.get('name', '').encode('ascii')[:27]
    kind = KINDS[e.get('kind', 'part')]
    icon = ICONS.index(e.get('icon', 'disk'))
    hot = e.get('hotkey', '')
    flags = 0
    for f in e.get('flags', []):
        flags |= FLAGS[f]
    pw = 0
    if e.get('password'):
        pw = pl_hash(e['password'])
        flags |= FLAGS['password']
    lba = e.get('lba', 0)
    if kind in (4, 5) and 'io' in e:
        lba = e['io'] | e.get('ctl', e['io'] + 0x206) << 16
    hide = 0
    for p in e.get('hide', []):
        hide |= 1 << (p - 1)
    vdrive = e.get('vdrive', 0xE0 if kind == 4 else 0x80)
    rec = struct.pack('<28sBBBBBBBBIIHBB16s', name, kind, icon, ord(hot.upper()) if hot else 0,
                      flags, e.get('drive', 0x80 if kind in (1, 2) else 0), e.get('ptype', 0),
                      e.get('pindex', 0), EMU[e.get('emu', 'auto')], lba, e.get('sectors', 0),
                      pw, hide, vdrive, b'')
    assert len(rec) == 64
    return rec


def checksum(buf):
    s = 0
    for i in range(0, len(buf), 2):
        if i == 22:
            continue
        s += struct.unpack_from('<H', buf, i)[0]
    return (-s) & 0xFFFF


def pack_config(menu, mbr_backup=b'\0' * 512):
    entries = menu.get('entries', [])
    assert len(entries) <= MAX_ENTRIES
    flags = 0
    for f in menu.get('flags', []):
        flags |= CFLAGS[f]
    default = menu.get('default', 0)
    timeout = menu.get('timeout', 10)
    hdr = struct.pack('<8sHHBBBBBBBBHH32s8s', b'POLARIS\x1a', 1, 2048, len(entries), default,
                      255 if timeout is None else timeout, menu.get('theme', 0), flags, 255,
                      255, 0, pl_hash(menu['setup_password']) if menu.get('setup_password') else 0,
                      0, menu.get('title', 'Choose an operating system').encode()[:31], b'')
    assert len(hdr) == 64
    body = b''.join(pack_entry(e) for e in entries).ljust(MAX_ENTRIES * 64, b'\0')
    buf = bytearray(hdr + body + b'\0' * (2048 - 64 - MAX_ENTRIES * 64 - 512) + mbr_backup)
    assert len(buf) == 2048
    struct.pack_into('<H', buf, 22, checksum(buf))
    return bytes(buf)


def load_parts():
    s1 = (BUILD / 'stage1.bin').read_bytes()
    s2 = (BUILD / 'stage2.bin').read_bytes()
    assert len(s1) == 512 and s1[INFO_OFS:INFO_OFS + 4] == b'PLRS'
    assert s2[3:11] == b'POLARIS2'
    count = (len(s2) + 511) // 512
    if count > MAX_STAGE2:
        sys.exit('stage 2 does not fit')
    return s1, s2.ljust(count * 512, b'\0'), count


def stage1_with_info(s1, count, media):
    b = bytearray(s1)
    struct.pack_into('<HHHB', b, INFO_OFS + 6, LBA_STAGE2, count, LBA_CONFIG, media)
    return b


def write_at(f, lba, data):
    f.seek(lba * 512)
    f.write(data)


def install_hdd(image, menu):
    s1, s2, count = load_parts()
    with open(image, 'r+b') as f:
        mbr = f.read(512)
        if mbr[510:512] != b'\x55\xaa':
            sys.exit('image has no partition table')
        starts = [struct.unpack_from('<I', mbr, 0x1BE + i * 16 + 8)[0]
                  for i in range(4) if mbr[0x1BE + i * 16 + 4]]
        need = LBA_STAGE2 + count
        if starts and min(starts) < need:
            sys.exit(f'first partition starts at LBA {min(starts)}; Polaris needs {need}')
        backup = mbr
        if mbr[INFO_OFS:INFO_OFS + 4] == b'PLRS':        # reinstall: keep the saved MBR
            f.seek(LBA_CONFIG * 512)
            old = f.read(2048)
            backup = old[2048 - 512:]
        new = bytearray(stage1_with_info(s1, count, 0))
        new[0x1B8:0x200] = mbr[0x1B8:0x200]                # signature + partitions
        write_at(f, LBA_CONFIG, pack_config(menu, backup))
        write_at(f, LBA_STAGE2, s2)
        write_at(f, 0, new)
    print(f'Polaris installed: stage 2 {count} sectors, {len(menu.get("entries", []))} entries')


def make_floppy(out, menu):
    s1, s2, count = load_parts()
    out = pathlib.Path(out)
    if out.exists():
        out.unlink()
    subprocess.run(['mkfs.fat', '-C', '-F', '12', '-R', str(FLOPPY_RESERVED), '-n', 'POLARIS',
                    '-i', '504C5253', str(out), '1440'], check=True, stdout=subprocess.DEVNULL)
    with open(out, 'r+b') as f:
        bpb = f.read(512)
        b = stage1_with_info(s1, count, 1)
        b[3:0x3E] = bpb[3:0x3E]
        write_at(f, 0, bytes(b))
        write_at(f, LBA_CONFIG, pack_config(menu))
        write_at(f, LBA_STAGE2, s2)
    print(f'Polaris floppy written: {out}')


def uninstall(image):
    with open(image, 'r+b') as f:
        mbr = f.read(512)
        if mbr[INFO_OFS:INFO_OFS + 4] != b'PLRS':
            sys.exit('Polaris is not installed on this image')
        f.seek(LBA_CONFIG * 512)
        backup = f.read(2048)[2048 - 512:]
        new = bytearray(backup if backup[510:] == b'\x55\xaa' else bytes(512))
        new[0x1B8:0x200] = mbr[0x1B8:0x200]
        write_at(f, 0, bytes(new))
        write_at(f, 1, b'\0' * 512 * 62)
    print('original boot code restored')


def show(image):
    with open(image, 'rb') as f:
        f.seek(LBA_CONFIG * 512)
        c = f.read(2048)
    if c[:8] != b'POLARIS\x1a':
        sys.exit('no Polaris configuration')
    count, default, timeout, theme = c[12], c[13], c[14], c[15]
    print(f'{count} entries, default {default}, timeout {timeout}, theme {theme}, '
          f'checksum {"ok" if checksum(c) == struct.unpack_from("<H", c, 22)[0] else "BAD"}')
    for i in range(count):
        e = c[64 + i * 64:128 + i * 64]
        name = e[:28].split(b'\0')[0].decode()
        kind = [k for k, v in KINDS.items() if v == e[28]][0]
        print(f'  {i:2} {name:28} {kind:10} drive {e[32]:02X}h lba {struct.unpack_from("<I", e, 36)[0]}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=['floppy', 'hdd', 'config', 'uninstall'])
    ap.add_argument('image')
    ap.add_argument('--config', help='menu JSON')
    a = ap.parse_args()
    menu = json.loads(pathlib.Path(a.config).read_text()) if a.config else {'entries': []}
    if a.command == 'floppy':
        make_floppy(a.image, menu)
    elif a.command == 'hdd':
        install_hdd(a.image, menu)
    elif a.command == 'config':
        show(a.image)
    else:
        uninstall(a.image)


if __name__ == '__main__':
    main()
