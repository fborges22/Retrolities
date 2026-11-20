#!/usr/bin/env python3
"""Build the Polaris demo machine used for tests and screenshots.

    python3 mkdemo.py FREEDOS_FLOPPY.img [OUTDIR]

Creates in OUTDIR (default build/polaris/demo):
  disk0.img      4 GB IDE disk, classic 63-sector alignment, Polaris installed:
                 P1 FAT16 with a real FreeDOS 1.4 (SYS'd inside QEMU),
                 P2 "NTFS" (demo boot sector), P3 ext2 + demo GRUB sector,
                 P4 extended: L5 FAT32 "DATA", L6 "HPFS" (demo OS/2 sector)
  disk1.img      1 GB SCSI disk: FreeBSD slice (demo) + FAT16 "BACKUP"
  freedos-cd.iso El Torito floppy emulation, FreeDOS 1.4 live
  noemu.iso      El Torito no-emulation test loader
  menu.json      the demo boot menu
"""
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
TOOLS = HERE.parent / 'tools'
sys.path.insert(0, str(TOOLS))
import plimage  # noqa: E402


def run(*a, **k):
    return subprocess.run([str(x) for x in a], check=True, **k)


def asm(src, out):
    run('nasm', '-f', 'bin', '-o', out, src)
    return pathlib.Path(out).read_bytes()


def demo_vbr(template, message, oem=b'', patch=None):
    b = bytearray(template)
    m = message.encode() + b'\0'
    assert len(m) <= 0x1FE - 0x1A0, message
    b[0x1A0:0x1A0 + len(m)] = m
    if oem:
        b[3:3 + len(oem)] = oem
    if patch:
        for off, data in patch:
            b[off:off + len(data)] = data
    return bytes(b)


def write(img, lba, data):
    with open(img, 'r+b') as f:
        f.seek(lba * 512)
        f.write(data)


def part_table(img):
    info = json.loads(subprocess.check_output(['sfdisk', '-J', str(img)]))
    return info['partitiontable']['partitions']


def main():
    floppy = pathlib.Path(sys.argv[1])
    out = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / 'build' / 'polaris' / 'demo'
    out.mkdir(parents=True, exist_ok=True)
    tmp = out / 'tmp'
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir()
    vbr = asm(HERE / 'demo_vbr.asm', tmp / 'vbr.bin')
    noemu = asm(HERE / 'noemu.asm', tmp / 'noemu.bin')

    # ---- disk 0 ----------------------------------------------------------
    d0 = out / 'disk0.img'
    if d0.exists():
        d0.unlink()
    run('truncate', '-s', str(8_388_608 * 512), d0)
    script = ('label: dos\nunit: sectors\n'
              '63,1048512,6,*\n'
              '1048575,3145728,7\n'
              '4194303,1048576,83\n'
              '5242879,3145729,5\n'
              '5242942,1048513,c\n'
              '6291519,2097089,7\n')
    run('sfdisk', '-q', '--no-reread', '--no-tell-kernel', d0, input=script.encode())
    parts = part_table(d0)
    lba = [p['start'] for p in parts]
    size = [p['size'] for p in parts]
    print('disk0 partitions:', list(zip(lba, size)))
    # P1: FAT16, made bootable with FreeDOS SYS below
    run('mkfs.fat', '-F', '16', '-n', 'FREEDOS', '-h', '63', '--offset', lba[0], d0, size[0] // 2,
        stdout=subprocess.DEVNULL)
    # P2: NTFS-looking demo
    write(d0, lba[1], demo_vbr(vbr, '  Windows 2000 Professional (demo partition)\r\n  NTLDR would load here.',
                               b'NTFS    '))
    # P3: ext2 with a demo boot sector in its first block
    run('mke2fs', '-q', '-t', 'ext2', '-L', 'debian', '-E', f'offset={lba[2] * 512}', d0,
        f'{size[2] // 2}k')
    write(d0, lba[2], demo_vbr(vbr, '  Debian GNU/Linux 12 (demo partition)\r\n  GRUB would load here.'))
    # L5: FAT32 data volume (not bootable - shows the warning path)
    run('mkfs.fat', '-F', '32', '-n', 'DATA', '-h', lba[4], '--offset', lba[4], d0, size[4] // 2,
        stdout=subprocess.DEVNULL)
    # L6: HPFS-looking demo
    hpfs = demo_vbr(vbr, '  OS/2 Warp 4 (demo partition)\r\n  OS2LDR would load here.', b'IBM 20.0',
                    [(0x0B, b'\x00\x02'), (0x36, b'HPFS    '), (0x100, b'OS2LDR')])
    write(d0, lba[5], hpfs)

    # FreeDOS onto P1: boot the floppy in QEMU with an automatic SYS C:
    sysfloppy = tmp / 'sys.img'
    shutil.copy(floppy, sysfloppy)
    cfg = tmp / 'FDCONFIG.SYS'
    cfg.write_bytes(b'LASTDRIVE=Z\r\nFILES=20\r\nSHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:1024 /P=\\FDAUTO.BAT\r\n')
    auto = tmp / 'FDAUTO.BAT'
    auto.write_bytes(b'@echo off\r\nSET PATH=A:\\FREEDOS\\BIN\r\nSYS C:\r\necho POLARIS-SYS-DONE\r\n')
    run('mcopy', '-o', '-i', sysfloppy, cfg, auto, '::/')
    p1 = f'{d0}@@{lba[0] * 512}'
    vm = subprocess.Popen(['qemu-system-i386', '-accel', 'tcg', '-m', '32', '-display', 'none',
                           '-monitor', 'none', '-serial', 'none', '-boot', 'a', '-no-reboot',
                           '-drive', f'file={sysfloppy},format=raw,if=floppy',
                           '-drive', f'file={d0},format=raw,if=ide,index=0'])
    ok = False
    for _ in range(120):
        time.sleep(1)
        r = subprocess.run(['mdir', '-b', '-i', p1, '::/'], capture_output=True, text=True)
        if 'KERNEL.SYS' in r.stdout:
            time.sleep(4)
            ok = True
            break
    vm.kill()
    vm.wait()
    if not ok:
        sys.exit('FreeDOS SYS did not finish')
    for name in ['FREEDOS/BIN/COMMAND.COM', 'FREEDOS/BIN/MEM.EXE']:
        run('mcopy', '-o', '-i', sysfloppy, '::/' + name, tmp / pathlib.Path(name).name)
    (tmp / 'C_CONFIG.SYS').write_bytes(b'FILES=30\r\nBUFFERS=20\r\nLASTDRIVE=Z\r\nSHELL=C:\\COMMAND.COM C:\\ /E:1024 /P=C:\\FDAUTO.BAT\r\n')
    (tmp / 'C_AUTO.BAT').write_bytes(
        b'@echo off\r\ncls\r\necho.\r\necho  FreeDOS 1.4 started by Polaris from hard disk 80h, primary partition 1\r\n'
        b'echo.\r\nver\r\necho.\r\nmem\r\n')
    run('mcopy', '-o', '-i', p1, tmp / 'COMMAND.COM', tmp / 'MEM.EXE', '::/')
    run('mcopy', '-o', '-i', p1, tmp / 'C_CONFIG.SYS', '::/FDCONFIG.SYS')
    run('mcopy', '-o', '-i', p1, tmp / 'C_AUTO.BAT', '::/FDAUTO.BAT')

    # ---- disk 1 (SCSI) ----------------------------------------------------
    d1 = out / 'disk1.img'
    if d1.exists():
        d1.unlink()
    run('truncate', '-s', str(2_097_152 * 512), d1)
    run('sfdisk', '-q', '--no-reread', '--no-tell-kernel', d1,
        input=b'label: dos\nunit: sectors\n63,1048513,a5,*\n1048576,1048576,6\n')
    parts1 = part_table(d1)
    l1 = [p['start'] for p in parts1]
    s1 = [p['size'] for p in parts1]
    write(d1, l1[0], demo_vbr(vbr, '  FreeBSD 4.11 on SCSI disk 81h (demo slice)\r\n'
                                   '  Started as drive 80h by the Polaris swap.'))
    run('mkfs.fat', '-F', '16', '-n', 'BACKUP', '-h', l1[1], '--offset', l1[1], d1, s1[1] // 2,
        stdout=subprocess.DEVNULL)

    # ---- CDs --------------------------------------------------------------
    cdroot = tmp / 'cd'
    cdroot.mkdir()
    live = cdroot / 'fdlive.img'
    shutil.copy(floppy, live)
    (tmp / 'L_AUTO.BAT').write_bytes(
        b'@echo off\r\nSET PATH=A:\\FREEDOS\\BIN\r\ncls\r\necho.\r\n'
        b'echo  FreeDOS 1.4 booted from an ATAPI CD-ROM by Polaris\r\n'
        b'echo  (El Torito 1.44 MB floppy emulation, resident INT 13h driver)\r\necho.\r\nver\r\necho.\r\n'
        b'mem\r\necho.\r\ndir A:\\ /W\r\n')
    run('mcopy', '-o', '-i', live, cfg, '::/FDCONFIG.SYS')
    run('mcopy', '-o', '-i', live, tmp / 'L_AUTO.BAT', '::/FDAUTO.BAT')
    (cdroot / 'README.TXT').write_text('Polaris demo CD\n')
    run('genisoimage', '-quiet', '-V', 'FREEDOS_LIVE', '-b', 'fdlive.img', '-c', 'boot.cat',
        '-o', out / 'freedos-cd.iso', cdroot)
    ne = tmp / 'ne'
    ne.mkdir()
    (ne / 'noemu.bin').write_bytes(noemu)
    run('genisoimage', '-quiet', '-V', 'NOEMU_TEST', '-no-emul-boot', '-boot-load-size', '4',
        '-b', 'noemu.bin', '-c', 'boot.cat', '-o', out / 'noemu.iso', ne)

    # ---- menu and install -------------------------------------------------
    menu = {
        'title': 'Choose an operating system', 'timeout': 15, 'theme': 0, 'default': 0,
        'flags': [],
        'entries': [
            {'name': 'FreeDOS 1.4', 'kind': 'part', 'drive': 0x80, 'pindex': 1, 'lba': lba[0],
             'sectors': size[0], 'ptype': 6, 'icon': 'dos', 'hotkey': '1', 'flags': ['active', 'unhide']},
            {'name': 'Windows 2000 Professional', 'kind': 'part', 'drive': 0x80, 'pindex': 2,
             'lba': lba[1], 'sectors': size[1], 'ptype': 7, 'icon': 'winnt', 'hotkey': '2',
             'flags': ['active', 'unhide'], 'hide': [1]},
            {'name': 'Debian GNU/Linux 12', 'kind': 'part', 'drive': 0x80, 'pindex': 3,
             'lba': lba[2], 'sectors': size[2], 'ptype': 0x83, 'icon': 'linux', 'hotkey': '3'},
            {'name': 'OS/2 Warp 4', 'kind': 'part', 'drive': 0x80, 'pindex': 6, 'lba': lba[5],
             'sectors': size[5], 'ptype': 7, 'icon': 'os2', 'hotkey': '4', 'password': 'warp'},
            {'name': 'Removable media', 'kind': 'separator', 'icon': 'star'},
            {'name': 'FreeDOS Live CD', 'kind': 'cdrom', 'io': 0x170, 'ctl': 0x376, 'drive': 0,
             'icon': 'cdrom', 'hotkey': 'C'},
            {'name': 'Iomega ZIP 100', 'kind': 'atapidisk', 'io': 0x170, 'ctl': 0x376, 'drive': 1,
             'icon': 'zip', 'hotkey': 'Z', 'emu': 'hdd'},
            {'name': 'Floppy disk A:', 'kind': 'floppy', 'drive': 0, 'icon': 'floppy', 'hotkey': 'A'},
            {'name': 'More disks', 'kind': 'separator', 'icon': 'star'},
            {'name': 'FreeBSD 4.11 (SCSI disk)', 'kind': 'part', 'drive': 0x81, 'pindex': 1,
             'lba': l1[0], 'sectors': s1[0], 'ptype': 0xA5, 'icon': 'bsd', 'hotkey': 'B',
             'flags': ['swap', 'fixbpb']},
            {'name': 'Network boot (PXE)', 'kind': 'biosnext', 'icon': 'net', 'hotkey': 'N'},
        ]}
    (out / 'menu.json').write_text(json.dumps(menu, indent=1))
    plimage.install_hdd(d0, menu)
    shutil.rmtree(tmp)
    print('demo ready in', out)


if __name__ == '__main__':
    main()
