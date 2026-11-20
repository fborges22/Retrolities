#!/usr/bin/env python3
"""Capture the documentation screenshots (src/polaris/docs) from the demo machine.

    python3 shots.py FREEDOS_FLOPPY.img
"""
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from PIL import Image

import plimage_path  # noqa: F401  (adds tools/ to sys.path)
import plimage
from demo_vm import demo_vm, DEMO

HERE = pathlib.Path(__file__).resolve().parent
DOCS = HERE.parent / 'docs'


def save(img, name):
    """640x400 text mode shown at 4:3, as on a CRT"""
    big = img.resize((1280, 800), Image.NEAREST).resize((1280, 960), Image.LANCZOS)
    big.save(DOCS / name, optimize=True)
    print('wrote', name)


def set_theme(theme):
    img = DEMO / 'disk0.img'
    with open(img, 'r+b') as f:
        f.seek(512)
        c = bytearray(f.read(2048))
        c[15] = theme
        struct.pack_into('<H', c, 22, plimage.checksum(c))
        f.seek(512)
        f.write(c)


def shot(vm, name, wait=1.0):
    time.sleep(wait)
    tmp = pathlib.Path(tempfile.mkstemp(suffix='.png')[1])
    save(vm.shot(tmp), name)


def main():
    DOCS.mkdir(exist_ok=True)
    # themes
    for n, name in enumerate(['night', 'aurora', 'ember', 'glacier', 'classic']):
        set_theme(n)
        vm = demo_vm()
        vm.wait_text('Boot menu')
        if n == 0:
            shot(vm, 'menu.png', 2)
        else:
            vm.key('down'); vm.key('down')
            shot(vm, f'theme-{name}.png')
        vm.quit()
    set_theme(0)

    vm = demo_vm()
    vm.wait_text('Boot menu')
    vm.key('down'); vm.key('down'); vm.key('down')
    shot(vm, 'details-os2.png')
    vm.key('ret')
    shot(vm, 'password.png')
    vm.key('esc')
    vm.key('f3')
    time.sleep(3)
    for _ in range(10):
        vm.key('down', 0.15)
    shot(vm, 'devices.png')
    vm.key('esc')
    vm.key('f4')
    shot(vm, 'sysinfo.png')
    vm.key('esc')
    vm.key('f2')
    shot(vm, 'setup.png')
    vm.key('down')
    vm.key('ret')
    shot(vm, 'entry-editor.png')
    vm.key('esc')
    vm.key('f5')
    vm.key('down')
    shot(vm, 'options.png')
    vm.key('esc'); vm.key('esc'); vm.key('d')
    vm.key('f10')
    shot(vm, 'power.png')
    vm.quit()

    boots = [('boot-freedos-hdd.png', 'freedos-cd.iso', ['1'], 'C:\\>'),
             ('boot-cdrom-floppy-emulation.png', 'freedos-cd.iso', ['shift-c'], 'A:\\>'),
             ('boot-cdrom-no-emulation.png', 'noemu.iso', ['shift-c'], 'without any BIOS'),
             ('boot-scsi-swap.png', 'freedos-cd.iso', ['shift-b'], 'boot drive 80h')]
    for name, cd, keys, expect in boots:
        vm = demo_vm(cd=cd)
        vm.wait_text('Boot menu')
        for k in keys:
            vm.key(k)
        vm.wait_text(expect, 60)
        shot(vm, name)
        vm.quit()
    # installer screens
    tmp = pathlib.Path(tempfile.mkdtemp())
    subprocess.run([sys.executable, str(HERE / 'installer_test.py'), sys.argv[1], str(tmp)], check=True)
    for n in ['installer-main', 'installer-pick', 'installer-confirm', 'installer-done',
              'installer-floppy', 'installer-after', 'installed-disk-menu', 'floppy-menu']:
        save(Image.open(tmp / f'{n}.png').convert('RGB'), f'{n}.png')
    shutil.rmtree(tmp)


if __name__ == '__main__':
    main()
