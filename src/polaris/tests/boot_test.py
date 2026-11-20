#!/usr/bin/env python3
"""Boot every kind of demo entry through Polaris and check the result on screen.

Requires the demo machine: python3 mkdemo.py FREEDOS_FLOPPY.img
"""
import sys
import time

from demo_vm import demo_vm

CASES = [
    # (description, cd image, keys, text expected after booting)
    ('menu appears', 'freedos-cd.iso', [], 'Choose an operating system'),
    ('FreeDOS from primary partition 1', 'freedos-cd.iso', ['1'],
     'FreeDOS 1.4 started by Polaris from hard disk 80h'),
    ('chain to P2 with P1 hidden', 'freedos-cd.iso', ['2'], 'partition LBA 000FFFFF'),
    ('logical partition 6 with password', 'freedos-cd.iso', ['4', 'w', 'a', 'r', 'p', 'ret'],
     'OS/2 Warp 4 (demo partition)'),
    ('SCSI disk 81h swapped to 80h', 'freedos-cd.iso', ['shift-b'], 'boot drive 80h, partition LBA 0000003F'),
    ('CD-ROM, El Torito floppy emulation', 'freedos-cd.iso', ['shift-c'],
     'Volume in drive A is FD14-BOOT'),
    ('CD-ROM, El Torito no emulation', 'noemu.iso', ['shift-c'],
     'The CD was booted without any BIOS CD-ROM support'),
    ('missing ZIP drive reports an error', 'freedos-cd.iso', ['shift-z'], 'was not found'),
    ('wrong password is refused', 'freedos-cd.iso', ['4', 'x', 'ret', 'x', 'ret', 'x', 'ret'],
     'Wrong password'),
]


def main():
    failed = 0
    for name, cd, keys, expect in CASES:
        vm = demo_vm(cd=cd)
        try:
            vm.wait_text('Boot menu', 30)
            time.sleep(0.5)
            for k in keys:
                vm.key(k, 0.4)
            vm.wait_text(expect, 60)
            print('ok  ', name)
        except AssertionError as e:
            print('FAIL', name, '\n', e)
            failed += 1
        finally:
            vm.quit()
    print(f'{len(CASES) - failed}/{len(CASES)} boot tests passed')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
