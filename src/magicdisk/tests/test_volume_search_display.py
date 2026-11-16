#!/usr/bin/env python3
"""QEMU visual and read-only controls for a multi-page discovery result."""
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
    original = bytes(63 * 512) + source.read_bytes() * 12
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


        def search():
            press('f10', 'Open image...')
            for cue in ['BIOS device...', 'DOS volume...', 'Select volume...', 'Search lost volumes...']:
                press('down', cue)
            press('ret', 'VOLUME SEARCH: FIRST DISK LBA', False)
            monitor('sendkey 0 20')
            time.sleep(.1)
            press('ret', 'VOLUME SEARCH: SECTOR COUNT', False)
            monitor('sendkey 0 20')
            time.sleep(.1)
            return press('ret', 'Search complete: 12 candidates.', False)

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
            initial = wait_screen('Missing boot signature')
            status = initial[24 * 160:]
            raw = search()
            content = raw[::2]
            assert content[22 * 80 + 73] == ord('v')
            assert raw[24 * 160:] == status
            for row in range(5, 22):
                assert content[row * 80 + 3] == 179 and content[row * 80 + 76] == 179
            capture('discovery-first', raw)
            raw = press('pgdn', '11   12')
            assert raw[::2][4 * 80 + 73] == ord('^')
            raw = press('end', '12   12')
            assert raw[::2][22 * 80 + 73] == 196
            capture('discovery-last', raw)
            press('pgup', '2   12')
            press('home', '1   12')
            press('esc', 'Volume search dismissed;', False)
            search()
            raw = press('ret', 'Discovered volume: read-only inspection', False)
            assert b'READ ONLY' in raw[::2]
            capture('discovered-map', raw)
            press('f10', 'Open image...')
            raw = press('right', 'Filesystem check')
            content = raw[::2].decode('cp437')
            for label in ['Mark bad sector...', 'Unmark bad sector...', 'Restore journal...', 'Recover primary boot...']:
                index = content.index(label)
                assert raw[index * 2 + 1] == 0x18, label
            capture('discovered-disabled-repairs', raw)
            press('down', 'Surface read')
            press('down', 'Retest marked bad')
            monitor('sendkey down 20')
            time.sleep(.15)
            monitor('sendkey ret 20')
            raw = wait_screen('Mark bad sector...')
            assert b'MARK BAD: ABSOLUTE' not in raw[::2]
            press('esc', 'Discovered volume: read-only inspection', False)
            press('f3', 'Check finished.', False)
            monitor('sendkey esc 20')
            process.wait(timeout=10)
            assert process.returncode == 33, process.stderr.read().decode(errors='replace')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    assert command('mtype', '-i', system, '::/SOURCE.IMG').stdout == original
assert digest(args.boot) == original_boot
print('PASS: discovery pages, count, borders, status, cancellation, disabled writes and exact image')
