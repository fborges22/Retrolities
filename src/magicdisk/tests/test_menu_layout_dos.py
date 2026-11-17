#!/usr/bin/env python3
"""Inspect actual VGA menu borders, label padding and focus in QEMU DOS."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import time
from bios_fixture import digest, prepare, put, text_file
from directory_fixture import create, command

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
parser.add_argument('--captures', type=Path)
args = parser.parse_args()
original_boot = digest(args.boot)
# Anchors and specific labels are expectations of the public menu layout.
menus = [
    ('disk', 1, 22, 'Open image...', 'Exit             Esc', ['Select volume...   F2', 'Search lost volumes...', 'Reconstruct MBR...', 'Undo MBR reconstruction...']),
    ('diagnose', 9, 26, 'Filesystem check  F3', 'Clear invalid timestamps...',
     ['Repair directory entries...', 'Repair directory chains...', 'Remove extra dot entries...']),
    ('view', 21, 7, 'Allocation map    F5', 'Cluster owner...   O', ['Retest map block   T']),
    ('report', 29, 5, 'Save report...    F7', 'Find orphan directories...',
     ['Recover interrupted write...', 'Recover deleted files...']),
    ('help', 40, 2, 'Keyboard / guide  F1', 'About MagicDisk...', ['About MagicDisk...']),
]
with tempfile.TemporaryDirectory(prefix='magicdisk-menu-layout-') as directory:
    root = Path(directory)
    boot, disk, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    volume = str(disk) + '@@32256'
    source = root / 'SOURCE.IMG'
    create(source, 12)
    original = source.read_bytes()
    put(volume, source)
    put(volume, text_file(root, 'RUN.BAT', '@echo off\nC:\ncd \\\nMAGICDSK --tui SOURCE.IMG\nEXITTEST.COM\n'))
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

        def check(raw, menu):
            name, x, count, first, last, labels = menu
            for other in menus:
                attr = raw[(80 + other[1] + 1) * 2 + 1]
                assert attr == (0x3f if other[0] == name else 0x70), (name, other[0], attr)
            visible = min(count, 19)
            characters = raw[::2]
            assert characters[2 * 80 + x] == 218
            right = characters.index(191, 2 * 80 + x + 1, 3 * 80) - 2 * 80
            assert right < 79
            for row in range(3, visible + 3):
                line = characters[row * 80:(row + 1) * 80]
                assert line[x] == 179 and line[right] == 179, (name, row, line)
                assert line[x + 1] == 32 and line[right - 1] == 32, (name, row, line)
            assert characters[(visible + 3) * 80 + x] == 192
            assert characters[(visible + 3) * 80 + right] == 217
            content = characters.decode('cp437')
            for label in [first, last] + labels:
                if label not in content:
                    assert count > visible and label in [first, last]
                    continue
                index = content.index(label)
                assert x + 2 <= index % 80 and index % 80 + len(label) < right
            if count > visible:
                assert characters[2 * 80 + right - 2] == (196 if first in content else ord('^'))
                assert characters[(visible + 3) * 80 + right - 2] == (196 if last in content else ord('v'))
            assert raw[23 * 160:] == status, 'Menu obscures the status bar'
            if args.captures:
                args.captures.mkdir(parents=True, exist_ok=True)
                (args.captures / f'{name}.bin').write_bytes(raw)
                (args.captures / f'{name}.txt').write_text('\n'.join(content[i:i + 80] for i in range(0, 2000, 80)) + '\n')
                target = (args.captures / f'{name}.ppm').resolve()
                monitor(f'screendump "{target}"')
                deadline = time.monotonic() + 3
                while not target.exists() and time.monotonic() < deadline:
                    time.sleep(.05)
                assert target.exists()

        try:
            initial = wait_screen('Allocation view ready.')
            status = initial[23 * 160:]
            raw = press('f10', menus[0][3])
            for index, menu in enumerate(menus):
                if index:
                    raw = press('right', menu[3])
                check(raw, menu)
                if menu[2] > 20:
                    check(press('pgdn', 'Undo interrupted record...' if menu[0] == 'disk' else 'Repair reserved attributes...'), menu)
                check(press('pgdn', menu[4]), menu)
                if menu[2] > 20:
                    check(press('pgup', 'DOS volume...' if menu[0] == 'disk' else 'Recover primary boot...'), menu)
                check(press('pgup', menu[3]), menu)
                check(press('end', menu[4]), menu)
                check(press('home', menu[3]), menu)
                check(press('up', menu[4]), menu)
                check(press('down', menu[3]), menu)
                print(f'PASS: {args.kernel} {menu[0]} menu: complete labels, borders, padding, status and Home/End/PageUp/PageDown/wrap', flush=True)
            press('esc', 'Allocation view ready.', False)
            press('f10', menus[0][3])
            press('end', menus[0][4])
            monitor('sendkey ret 20')
            process.wait(timeout=10)
            assert process.returncode == 33, process.stderr.read().decode(errors='replace')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    assert command('mtype', '-i', volume, '::/SOURCE.IMG').stdout == original
assert digest(args.boot) == original_boot
