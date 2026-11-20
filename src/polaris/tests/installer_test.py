#!/usr/bin/env python3
"""End-to-end test of POLARIS.EXE under FreeDOS in QEMU.

    python3 installer_test.py FREEDOS_FLOPPY.img [SHOTDIR]

1. Boots FreeDOS from A:, runs the interactive installer, installs Polaris on
   a blank IDE disk (63-sector alignment) and creates a Polaris floppy in B:.
2. Boots the disk and the floppy and checks that the Polaris menu appears.
Screenshots are written to SHOTDIR when given.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

import qemu

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
EXE = ROOT / 'build' / 'dos' / 'bin' / 'POLARIS.EXE'


def run(*a, **k):
    return subprocess.run([str(x) for x in a], check=True, **k)


def is_menu(img):
    """the Polaris header uses the deep-space colour across the top rows"""
    px = img.getpixel((600, 5))
    return px[2] > px[0] and img.getpixel((60, 20)) != (0, 0, 0)


def main():
    floppy = pathlib.Path(sys.argv[1])
    shots = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else None
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='polaris-inst-'))
    dos = tmp / 'dos.img'
    shutil.copy(floppy, dos)
    (tmp / 'FDCONFIG.SYS').write_bytes(
        b'LASTDRIVE=Z\r\nFILES=20\r\nSHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:1024 /P=\\FDAUTO.BAT\r\n')
    (tmp / 'FDAUTO.BAT').write_bytes(b'@echo off\r\nSET PATH=A:\\FREEDOS\\BIN\r\nPOLARIS\r\n')
    run('mcopy', '-o', '-i', dos, tmp / 'FDCONFIG.SYS', tmp / 'FDAUTO.BAT', EXE, '::/')
    disk = tmp / 'target.img'
    run('truncate', '-s', str(2 * 1024 ** 3), disk)
    run('sfdisk', '-q', '--no-reread', '--no-tell-kernel', disk,
        input=b'label: dos\nunit: sectors\n63,2000000,6,*\n')
    run('mkfs.fat', '-F', '16', '-n', 'DOSDISK', '-h', '63', '--offset', '63', disk, '1000000',
        stdout=subprocess.DEVNULL)
    fb = tmp / 'b.img'
    run('mkfs.fat', '-C', fb, '1440', stdout=subprocess.DEVNULL)

    def shot(vm, name):
        return vm.shot(shots / name if shots else tmp / name)

    vm = qemu.VM(['-boot', 'a', '-drive', f'file={dos},format=raw,if=floppy,index=0',
                  '-drive', f'file={fb},format=raw,if=floppy,index=1',
                  '-drive', f'file={disk},format=raw,if=ide,index=0'])
    time.sleep(14)
    shot(vm, 'installer-main.png')
    vm.key('ret')                     # Install on a hard disk
    time.sleep(1)
    shot(vm, 'installer-pick.png')
    vm.key('ret')                     # disk 80h
    time.sleep(1)
    shot(vm, 'installer-confirm.png')
    vm.key('ret')                     # Install
    time.sleep(6)
    shot(vm, 'installer-done.png')
    vm.key('ret')                     # OK
    time.sleep(1)
    vm.key('l')                       # configure later
    time.sleep(1)
    vm.key('down')                    # Create a Polaris boot floppy
    vm.key('ret')
    time.sleep(1)
    vm.key('down')                    # Drive B:
    vm.key('ret')
    time.sleep(1)
    vm.key('ret')                     # Write
    time.sleep(10)
    shot(vm, 'installer-floppy.png')
    vm.key('ret')
    time.sleep(1)
    shot(vm, 'installer-after.png')
    vm.quit()

    ok = True
    vm = qemu.VM(['-boot', 'c', '-drive', f'file={disk},format=raw,if=ide,index=0'])
    time.sleep(6)
    img = shot(vm, 'installed-disk-menu.png')
    vm.quit()
    if not is_menu(img):
        print('FAIL: the installed disk does not show the Polaris menu')
        ok = False
    vm = qemu.VM(['-boot', 'a', '-drive', f'file={fb},format=raw,if=floppy,index=0',
                  '-drive', f'file={disk},format=raw,if=ide,index=0'])
    time.sleep(8)
    img = shot(vm, 'floppy-menu.png')
    vm.quit()
    if not is_menu(img):
        print('FAIL: the Polaris floppy does not show the menu')
        ok = False
    r = subprocess.run(['mdir', '-i', str(fb), '::/'], capture_output=True, text=True)
    if 'POLARIS' not in r.stdout:
        print('FAIL: the Polaris floppy is not a readable FAT12 disk')
        ok = False
    print('installer test', 'passed' if ok else 'FAILED', '-', tmp)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
