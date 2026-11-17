#!/usr/bin/env python3
"""Drive the sector viewer's selection, export, import, editor and undo (ui.c)."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ui = Path(sys.argv[1]).resolve()

TOTAL, SPC, FATSZ, ROOT_ENTRIES = 2880, 4, 3, 224
DATA = 1 + 2 * FATSZ + ROOT_ENTRIES * 32 // 512  # first data sector: 21


def floppy():
    """FAT12 superfloppy with four sectors per cluster and patterned data."""
    image = bytearray(TOTAL * 512)
    boot = bytearray(512)
    boot[0:3] = b'\xeb\x3c\x90'
    boot[3:11] = b'MAGICTST'
    struct.pack_into('<HBHBHHBHHHII', boot, 11, 512, SPC, 1, 2, ROOT_ENTRIES, TOTAL, 0xF0,
                     FATSZ, 18, 2, 0, 0)
    boot[38] = 0x29
    boot[43:54] = b'SECTORTEST '
    boot[54:62] = b'FAT12   '
    boot[510:512] = b'\x55\xaa'
    image[0:512] = boot
    for copy in range(2):
        at = (1 + copy * FATSZ) * 512
        image[at:at + 3] = b'\xf0\xff\xff'
    for sector in range(DATA, TOTAL):
        image[sector * 512:(sector + 1) * 512] = bytes((sector * 3 + i) & 0xff for i in range(512))
    return bytes(image)


def lba(cluster):
    return DATA + (cluster - 2) * SPC


def sectors(data, first, count):
    return data[first * 512:(first + count) * 512]


def play(root, image, lines, code=0):
    keys = root / 'keys.txt'
    keys.write_text('\n'.join(lines) + '\n')
    screen = root / 'screen.txt'
    result = subprocess.run([str(ui), '--tui', str(image)], capture_output=True, text=True,
                            env=dict(os.environ, MAGICDISK_KEYS=str(keys),
                                     MAGICDISK_SCREEN=str(screen)))
    shown = screen.read_text() if screen.exists() else ''
    assert result.returncode == code, (result.returncode, result.stderr, shown)
    return shown


def write_flow(journal):
    return ['wait:REVIEW SECTOR WRITE', 'enter', 'wait:TYPE WRITE', 'type:WRITE', 'enter',
            'wait:NEW JOURNAL FILE', f'type:{journal}', 'enter',
            'wait:Sector edit written and verified.']


def undo(journal, torn=False):
    return (['f10', 'wait:Open image...', 'end', 'up', 'up'] + ([] if torn else ['up']) +
            ['wait:Undo sector edit...', 'enter'] +
            (['wait:RECOVER INTERRUPTED SECTOR EDIT', 'enter'] if torn else []) +
            ['wait:SECTOR EDIT JOURNAL TO RESTORE', f'type:{journal}', 'enter',
             'wait:REVIEW SECTOR EDIT UNDO', 'enter', 'wait:TYPE RESTORE', 'type:RESTORE',
             'enter', 'wait:Original sectors restored and verified.'])


with tempfile.TemporaryDirectory(prefix='magicdisk-sector-ui-') as directory:
    root = Path(directory)
    image = root / 'floppy.img'
    original = floppy()
    image.write_bytes(original)
    opened = ['wait:Allocation view ready.', 'f8', 'wait:READ-ONLY SECTOR VIEWER',
              f'wait:Disk LBA {DATA} /', 'wait:Cluster 2 [1/4]']

    # Cluster navigation, sector selection snapped to whole clusters, export.
    export = root / 'clusters.bin'
    play(root, image, opened + [
        'key:k', 'wait:GO TO DATA CLUSTER', 'type:1', 'enter', 'wait:Invalid cluster; enter 2',
        'key:k', 'type:5', 'enter', f'wait:Disk LBA {lba(5)} /', 'wait:Cluster 5 [1/4]',
        'pgdn', 'wait:Cluster 5 [2/4]', 'key:c',
        f'wait:Selected LBA {lba(5)}-{lba(5) + 3} (4 sectors, clusters 5-5)',
        'pgdn', f'wait:Selected LBA {lba(5)}-{lba(6)} (5 sectors, clusters 5-6)', 'key:c',
        f'wait:Selected LBA {lba(5)}-{lba(6) + 3} (8 sectors, clusters 5-6)',
        'wait: SELECTED ', 'key:x', f'wait:EXPORT LBA {lba(5)}-{lba(6) + 3}',
        f'type:{export}', 'enter', 'wait:Exported 8 sectors (4096 bytes). Disk unchanged.',
        # Existing files are never replaced.
        'key:x', f'type:{export}', 'enter', 'wait:Cannot create export file',
        'key: ', 'absent:Selected LBA', 'esc', 'wait:Allocation view ready.', 'esc'])
    assert export.read_bytes() == sectors(original, lba(5), 8)
    assert image.read_bytes() == original

    # Single-sector export without a selection; C refuses metadata sectors.
    single = root / 'boot.bin'
    play(root, image, opened + [
        'home', 'wait:Disk LBA 0 /', 'wait:Volume boot sector', 'key:c',
        'wait:C needs sectors inside', 'key:x', 'wait:EXPORT LBA 0-0', f'type:{single}', 'enter',
        'wait:Exported 1 sector (512 bytes).', 'esc', 'esc'])
    assert single.read_bytes() == original[:512]

    # Import: size checks, cancelled review, then a journaled write and undo.
    patch = bytes((0xA0 + i) & 0xff for i in range(8 * 512))
    short = root / 'short.bin'
    short.write_bytes(patch[:1024])
    odd = root / 'odd.bin'
    odd.write_bytes(b'x' * 700)
    source = root / 'patch.bin'
    source.write_bytes(patch)
    select = ['key:k', 'type:5', 'enter', 'key:c', 'pgdn', 'key:c',
              f'wait:Selected LBA {lba(5)}-{lba(6) + 3} (8 sectors']
    play(root, image, opened + select + [
        'key:i', 'wait:IMPORT RAW SECTORS FROM FILE', f'type:{short}', 'enter',
        'wait:File holds 2 sectors; the selection holds 8.',
        'key:i', f'type:{odd}', 'enter', 'wait:nonzero multiple of 512 bytes',
        'key:i', f'type:{source}', 'enter', 'wait:REVIEW SECTOR WRITE',
        'wait:Import patch.bin: 8 sectors', f'wait:Disk LBA {lba(5)}-{lba(6) + 3}; starts at Cluster 5',
        'wait:8 of 8 sectors change', 'esc', 'wait:Selected LBA',
        'key:i', f'type:{source}', 'enter', 'wait:REVIEW SECTOR WRITE', 'enter',
        'wait:TYPE WRITE', 'type:write', 'enter', 'wait:Write cancelled; no sectors written.',
        'esc', 'esc'])
    assert image.read_bytes() == original

    journal = root / 'import.mdj'
    play(root, image, opened + select + [
        'key:i', f'type:{source}', 'enter'] + write_flow(journal) + [
        'absent:Selected LBA', 'wait:READ-ONLY SECTOR VIEWER', 'wait:A0 A1 A2 A3', 'esc',
        'wait:ALLOCATION MAP', 'absent:READ-ONLY SECTOR VIEWER', 'esc'])
    changed = image.read_bytes()
    assert sectors(changed, lba(5), 8) == patch
    assert changed[:lba(5) * 512] == original[:lba(5) * 512]
    assert changed[(lba(6) + 4) * 512:] == original[(lba(6) + 4) * 512:]
    assert journal.stat().st_size == 64 + 512 + 8 * 1024

    # Importing identical bytes writes nothing.
    same = root / 'same.bin'
    same.write_bytes(sectors(changed, lba(5), 1))
    play(root, image, opened + ['key:k', 'type:5', 'enter', 'key:i', f'type:{same}', 'enter',
                                'wait:File matches the current sectors; nothing to write.',
                                'esc', 'esc'])

    # Undo from the Disk menu restores the original bytes.
    play(root, image, ['wait:Allocation view ready.'] + undo(journal) + ['esc'])
    assert image.read_bytes() == original

    # Without a selection the file length decides the range.
    journal2 = root / 'import2.mdj'
    play(root, image, opened + ['key:g', 'type:100', 'enter', 'key:i', f'type:{short}', 'enter',
                                'wait:Import short.bin: 2 sectors', 'wait:Disk LBA 100-101']
         + write_flow(journal2) + ['esc', 'esc'])
    assert sectors(image.read_bytes(), 100, 2) == patch[:1024]
    play(root, image, ['wait:Allocation view ready.'] + undo(journal2) + ['esc'])
    assert image.read_bytes() == original

    # Editor: hex digits, text after Tab, backspace undo, guarded navigation.
    edit = ['key:g', 'type:40', 'enter', 'wait:Disk LBA 40 /', 'key:e', 'wait:SECTOR EDITOR',
            'wait:EDIT HEX', 'type:DEADbeef', 'wait:4 changed', 'wait:Offset 004',
            'tab', 'wait:EDIT TEXT', 'type:Hi!', 'wait:7 changed', 'bs', 'wait:6 changed',
            'pgdn', 'wait:F2 writes or F3 reverts this sector before moving.',
            'wait:Disk LBA 40 /']
    play(root, image, opened + edit + [
        'esc', 'wait:DISCARD SECTOR EDITS', 'esc', 'wait:SECTOR EDITOR', 'wait:6 changed',
        'f3', 'wait:Edits reverted', 'wait:0 changed', 'f2', 'wait:No bytes changed',
        'type:12', 'esc', 'wait:DISCARD SECTOR EDITS', 'enter', 'wait:READ-ONLY SECTOR VIEWER',
        'esc', 'esc'])
    assert image.read_bytes() == original

    journal3 = root / 'edit.mdj'
    play(root, image, opened + edit + ['f2', 'wait:Write 6 edited bytes', 'wait:Disk LBA 40:',
                                       'wait:1 of 1 sector change; 6 bytes differ.']
         + write_flow(journal3) + ['wait:SECTOR EDITOR', 'wait:0 changed', 'wait:DE AD BE EF 48 69',
                                   'pgdn', 'wait:Disk LBA 41 /', 'esc',
                                   'wait:READ-ONLY SECTOR VIEWER', 'esc', 'esc'])
    after = image.read_bytes()
    expected = bytearray(sectors(original, 40, 1))
    expected[0:6] = b'\xde\xad\xbe\xef\x48\x69'
    assert sectors(after, 40, 1) == bytes(expected)
    assert after[:40 * 512] == original[:40 * 512] and after[41 * 512:] == original[41 * 512:]

    # A later unrelated change blocks plain undo; the interrupted path allows only mixtures.
    tampered = bytearray(after)
    tampered[40 * 512 + 100] ^= 0xff
    image.write_bytes(tampered)
    play(root, image, ['wait:Allocation view ready.', 'f10', 'end', 'up', 'up', 'up', 'enter',
                       f'type:{journal3}', 'enter', 'enter', 'type:RESTORE', 'enter',
                       'wait:LBA 40 differs from journal', 'esc'])
    assert image.read_bytes() == bytes(tampered)
    torn = bytearray(after)
    torn[40 * 512 + 2] = original[40 * 512 + 2]
    image.write_bytes(torn)
    play(root, image, ['wait:Allocation view ready.'] + undo(journal3, torn=True) + ['esc'])
    assert image.read_bytes() == original

    # Raw images without a FAT volume can be edited too; K needs a volume.
    raw = root / 'RAW.IMG'
    raw.write_bytes(bytes(range(256)) * 4)
    play(root, raw, ['f8', 'wait:READ-ONLY SECTOR VIEWER', 'key:k',
                     'wait:Cluster navigation needs a mounted FAT volume.', 'key:e',
                     'type:7', 'right', 'wait:Offset 001', 'type:00', 'wait:2 changed', 'f2']
         + write_flow(root / 'raw.mdj') + ['esc', 'esc', 'esc'])
    assert raw.read_bytes()[:3] == b'\x70\x00\x02'
    print('sector editor UI tests passed')
