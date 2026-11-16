#!/usr/bin/env python3
"""Findings open reviewed repair workflows, without implicit source writes."""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import command, create, Layout
from duplicate_fixture import duplicate_alias
from chain_fixture import set_fat
from short_name_fixture import entries
from ui_scripts import device_keys, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
parser.add_argument('--commit-files', action='store_true')
args = parser.parse_args()
source_hash = digest(args.boot)
host = args.executable.resolve().parents[2] / 'host' / 'magicdisk'
cases = [(12, 'file'), (16, 'file'), (32, 'file'), (12, 'chain'), (12, 'directory'),
         (12, 'timestamp'), (12, 'header'), (12, 'mirror'), (12, 'conflict'),
         (12, 'unsupported'),
         (32, 'fsinfo'), (32, 'boot')]
if args.commit_files:
    cases = [case for case in cases if case[1] == 'file']
info = {
    'file': ('FILE_SIZE', 'REVIEW FILE-CHAIN REPAIR', 'File repair cancelled;'),
    'chain': ('CHAIN_RANGE', 'REVIEW FILESYSTEM TREE REPAIR', 'Combined repair cancelled;'),
    'directory': ('DIR_SIZE', 'REVIEW DIRECTORY-ENTRY REPAIR', 'Entry repair cancelled;'),
    'timestamp': ('DIR_TIMESTAMP', 'REVIEW INVALID TIMESTAMPS', 'Timestamp repair cancelled;'),
    'header': ('FAT_HEADER', 'REVIEW FAT HEADERS', 'FAT header repair cancelled;'),
    'mirror': ('FAT_MIRROR', 'REVIEW FILESYSTEM TREE REPAIR', 'Combined repair cancelled;'),
    'conflict': ('DIR_DUPLICATE', 'REVIEW NAME CONFLICTS', 'Name repair cancelled;'),
    'unsupported': ('FAT_RANGE', '', ''),
    'fsinfo': ('FSINFO_SIGNATURE', 'REVIEW FSINFO REPAIR', 'Metadata repair cancelled;'),
    'boot': ('BOOT_BACKUP', 'REVIEW BACKUP BOOT REPAIR', 'Metadata repair cancelled;'),
}
with tempfile.TemporaryDirectory(prefix='magicdisk-finding-repairs-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    for bits, kind in cases:
        volume = root / 'VOLUME.IMG'
        create(volume, bits)
        data = bytearray(volume.read_bytes())
        struct.pack_into('<HHI', data, 24, 63, 16, 63)
        if bits == 32:
            data[6 * 512:7 * 512] = data[:512]
        layout = Layout(data, bits)
        _, entry = entries(data, bits)[0]
        if kind == 'file':
            struct.pack_into('<I', data, entry + 28, layout.spc * 512 + 1)
        elif kind == 'chain':
            set_fat(data, bits, layout.cluster(entry), 1)
        elif kind == 'directory':
            struct.pack_into('<I', data, layout.child() + 28, 1)
        elif kind == 'timestamp':
            data[layout.child() + 13] = 255
        elif kind == 'header':
            for copy in range(data[16]):
                start = (layout.reserved + copy * layout.fatsz) * 512
                data[start:start + 3] = bytes(3)
        elif kind == 'mirror':
            data[(layout.reserved + layout.fatsz) * 512 + 1500] ^= 1
        elif kind == 'conflict':
            data = duplicate_alias(data, bits)
        elif kind == 'unsupported':
            assert layout.fat(1000) == 0
            set_fat(data, bits, 1000, 1)
        elif kind == 'fsinfo':
            data[512:1024] = bytes(512)
        elif kind == 'boot':
            data[6 * 512:7 * 512] = bytes(512)
        volume.write_bytes(data)
        scan = subprocess.run([str(host), str(volume)], capture_output=True, text=True)
        assert scan.returncode == 1, scan.stdout + scan.stderr
        codes = re.findall(r'^  ([A-Z_]+)\s+\d+\s', scan.stdout, re.M)
        code, review, cancelled = info[kind]
        assert code in codes, scan.stdout
        sectors = len(data) // 512
        mbr = bytearray(63 * 512)
        partition(mbr, sectors, {12: 0x11, 16: 0x16, 32: 0x1c}[bits])
        target_cylinders = (63 + sectors + 1007) // 1008 + 1
        original = mbr + data + bytes(target_cylinders * 1008 * 512 - len(mbr) - len(data))
        hard = root / 'TARGET.IMG'
        hard.write_bytes(original)
        keys = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81') +
                step('Allocation view ready.', 'f3') + step('Check finished.', 'f6'))
        for previous in codes[:codes.index(code)]:
            keys += step(previous, 'down', True)
        if kind == 'unsupported':
            keys += (step(code, ord('p'), True, absent='P: review volume repair') +
                     step('INSPECTION FINDINGS', 'esc') + step('Check finished.', 'esc'))
        else:
            keys += step('P: review volume repair', ord('p'))
            if kind == 'mirror':
                keys += typing('SOURCE FAT NUMBER TO REPAIR AND KEEP', '1')
            keys += step(review, 'esc')
            if kind == 'file':
                keys += (step(cancelled, 'f6') +
                         step('FILE-REPAIR PREVIEW', ord('p'), absent='P: review volume repair') +
                         step('FILE-REPAIR PREVIEW', 'esc'))
            if args.commit_files:
                keys += (step(cancelled, 'f3') + step('Check finished.', 'f6') +
                         step('P: review volume repair', ord('p')) + step(review, 'enter') +
                         typing('TYPE REPAIR TO CORRECT FILE CHAINS', 'REPAIR') +
                         typing('NEW JOURNAL FILE ON A SEPARATE DISK', f'C:\\F{bits}.MDJ') +
                         step('BIOS REPAIR REQUIREMENTS', 'enter') + step('Repair verified.', 'esc'))
            else:
                keys += step(cancelled, 'esc')
        put(system_image, text_file(root, 'REPAIR.KEY', keys))
        batch = ['@echo off', 'C:', 'cd \\', 'KEYPLAY REPAIR.KEY', 'MAGICDSK --tui']
        if args.commit_files:
            batch += ['MAGICDSK --bios 81 --partition 1 > A:\\CLEAN.TXT',
                      'if errorlevel 1 echo FAIL >> A:\\CLEAN.TXT',
                      f'MAGICDSK --bios 81 --partition 1 --restore-journal C:\\F{bits}.MDJ --apply > A:\\UNDO.TXT',
                      'if errorlevel 1 echo FAIL >> A:\\UNDO.TXT']
        batch += ['EXITTEST.COM']
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, hard, target_cylinders)
        if args.commit_files:
            clean = command('mtype', '-i', str(boot), '::/CLEAN.TXT').stdout.decode()
            undo = command('mtype', '-i', str(boot), '::/UNDO.TXT').stdout.decode()
            assert 'FAIL' not in clean + undo and '0 issues' in clean, clean + undo
            assert 'restored and verified' in undo, undo
            assert command('mtype', '-i', system_image, f'::/F{bits}.MDJ').stdout
        assert hard.read_bytes() == original
        print(f'PASS: {args.kernel} FAT{bits} {code}: finding repair routing, cancellation, exact source')
assert digest(args.boot) == source_hash
