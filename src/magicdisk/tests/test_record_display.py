#!/usr/bin/env python3
"""QEMU visual and read-only controls for a multi-page discovery result."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import struct
import time
from bios_fixture import digest, prepare, put, text_file
from directory_fixture import create, command

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', required=True, choices=['msdos622', 'freedos', 'win98'])
parser.add_argument('--captures', type=Path, required=True)
args = parser.parse_args()
original_boot = digest(args.boot)
args.captures.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='magicdisk-discovery-screen-') as directory:
    root = Path(directory)
    boot, disk, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system = str(disk) + '@@32256'
    source = root / 'SOURCE.IMG'
    create(source, 12)
    data = source.read_bytes()
    sectors = len(data) // 512
    records = [63 + i * (sectors + 63) for i in range(11)]
    total = records[-1] + 63 + sectors
    original = bytearray(total * 512)
    original[450] = 15
    struct.pack_into('<II', original, 454, 63, total - 63)
    original[510:512] = b'\x55\xaa'
    for i, record in enumerate(records):
        original[(record + 63) * 512:(record + 63 + sectors) * 512] = data
        original[record * 512 + 450] = 1
        struct.pack_into('<II', original, record * 512 + 454, 63, sectors)
        original[record * 512 + 510:record * 512 + 512] = b'\x55\xaa'
        if i < 10:
            original[record * 512 + 466] = 15
            struct.pack_into('<II', original, record * 512 + 470, records[i+1] - 63, total - records[i+1])
    original[63*512:64*512] = bytes(512)
    original = bytes(original)
    source.write_bytes(original)
    put(system, source)
    put(system, text_file(root, 'RUN.BAT', '@echo off\nC:\ncd \\\nMAGICDSK --tui SOURCE.IMG\nEXITTEST.COM\n'))
    argv = ['qemu-system-i386', '-accel', 'tcg', '-m', '64', '-display', 'none',
            '-serial', 'none', '-monitor', 'stdio', '-no-reboot', '-boot', 'a',
            '-drive', f'file={boot},format=raw,if=floppy,index=0',
            '-drive', f'file={disk},format=raw,if=none,id=systemdisk',
            '-device', f'ide-hd,drive=systemdisk,bus=ide.0,unit=0,cyls={cylinders},heads=16,secs=63,bios-chs-trans=none',
            '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']
    with subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE) as process:
        def monitor(value):
            process.stdin.write((value + '\n').encode())
            process.stdin.flush()

        def wait_screen(cue, focus=False):
            deadline = time.monotonic() + 30
            previous = None
            while time.monotonic() < deadline:
                assert process.poll() is None, 'QEMU exited before screen cue: ' + cue
                memory = root / 'SCREEN.BIN'
                memory.unlink(missing_ok=True)
                monitor(f'pmemsave 0xb8000 4000 "{memory}"')
                time.sleep(.1)
                if not memory.exists() or memory.stat().st_size != 4000:
                    continue
                raw = memory.read_bytes()
                text = raw[::2].decode('cp437')
                index = text.find(cue)
                if index >= 0 and (not focus or raw[index * 2 + 1] == 0x3f) and raw == previous:
                    return raw
                previous = raw
            raise AssertionError('Missing screen cue/focus: ' + cue)

        def press(key, cue, focus=True):
            monitor(f'sendkey {key} 20')
            return wait_screen(cue, focus)


        def number(value, next_cue):
            for char in str(value):
                monitor(f'sendkey {char} 20')
                time.sleep(.08)
            return press('ret', next_cue, False)

        def review():
            press('f10', 'Open image...')
            press('end', 'Exit')
            press('up', 'OEM code page...')
            press('up', 'Undo interrupted record...')
            press('up', 'Undo whole EBR...')
            press('up', 'Rebuild whole EBR...')
            press('ret', 'RECORD: EBR DISK LBA', False)
            number(63, 'RECORD: VOLUME DISK LBA')
            number(126, 'RECORD: NEXT EBR LBA')
            number(records[1], 'RECORD: BOOT SOURCE')
            number(0, 'RECORD: BIOS GEOMETRY HEADS')
            number(16, 'RECORD: BIOS SECTORS PER TRACK')
            return number(63, 'Records 1-9 of 11')

        def capture(name, raw):
            (args.captures / (name + '.bin')).write_bytes(raw)
            content = raw[::2].decode('cp437')
            (args.captures / (name + '.txt')).write_text('\n'.join(content[i:i+80] for i in range(0,2000,80)))
            path = (args.captures / (name + '.ppm')).resolve()
            path.unlink(missing_ok=True)
            monitor(f'screendump "{path}"')
            deadline = time.monotonic() + 3
            while not path.exists() and time.monotonic() < deadline:
                time.sleep(.05)
            assert path.exists()

        try:
            initial = wait_screen('SOURCE.IMG')
            raw = review()
            content = raw[::2]
            assert b'EBR 63 -> volume 126' in content and b'boot +0' in content
            capture('record-debug', raw)
            for row in range(4, 21):
                assert content[row * 80 + 5] == 179 and content[row * 80 + 74] == 179, (row, content[row*80:(row+1)*80])
            capture('record-first', raw)
            raw = press('pgdn', 'Records 3-11 of 11', False)
            assert str(records[-1]).encode() in raw[::2][16*80:17*80]
            capture('record-last', raw)
            press('home', 'Records 1-9 of 11', False)
            press('down', 'Records 2-10 of 11', False)
            press('up', 'Records 1-9 of 11', False)
            press('end', 'Records 3-11 of 11', False)
            press('pgup', 'Records 1-9 of 11', False)
            raw = press('esc', 'SOURCE.IMG', False)
            assert b'REVIEW WHOLE EBR RECORD' not in raw[::2]
            monitor('sendkey esc 20')
            process.wait(timeout=10)
            assert process.returncode == 33, process.stderr.read().decode(errors='replace')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    assert command('mtype', '-i', system, '::/SOURCE.IMG').stdout == original
assert digest(args.boot) == original_boot
print('PASS: eleven-volume whole-record review, scrolling, borders, cancellation and exact image')
