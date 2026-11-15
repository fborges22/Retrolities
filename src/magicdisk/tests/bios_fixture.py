"""Boot infrastructure for disposable BIOS-only QEMU tests."""
import hashlib
import shutil
import struct
import subprocess
from boot_fixture import copy_boot
from directory_fixture import command


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def put(image, source, name=None):
    command('mcopy', '-o', '-i', str(image), str(source), '::/' + (name or source.name))


def text_file(root, name, text):
    path = root / name
    path.write_bytes(text.replace('\n', '\r\n').encode('ascii'))
    return path


def partition(mbr, count, kind=6):
    mbr[446], mbr[450] = 0x80, kind
    for offset, lba in [(447, 63), (451, count + 62)]:
        cylinder, rest = divmod(lba, 1008)
        head, sector = divmod(rest, 63)
        mbr[offset:offset + 3] = bytes([head, sector + 1 | (cylinder >> 2 & 0xc0), cylinder & 255])
    struct.pack_into('<II', mbr, 454, 63, count)
    mbr[510:512] = b'\x55\xaa'


def prepare(root, source_boot, kernel, probe, name):
    boot, volume, disk = [root / n for n in ['BOOT.IMG', 'VOL.IMG', 'SYSTEM.IMG']]
    copy_boot(source_boot, boot)
    with volume.open('wb') as file:
        file.truncate(32 * 1024 * 1024)
    command('mkfs.fat', '-F', '16', str(volume))
    put(volume, probe, name)
    exit_file = root / 'EXITTEST.COM'
    exit_file.write_bytes(bytes.fromhex('b4 0d cd 21 ba f4 00 b0 10 ee eb fe'))
    put(volume, exit_file)
    config = 'FILES=30\nBUFFERS=10\nLASTDRIVE=Z\n'
    startup = '@echo off\nC:\\RUN.BAT\n'
    if kernel == 'freedos':
        config += 'SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\n'
        put(boot, text_file(root, 'FDCONFIG.SYS', config))
        put(boot, text_file(root, 'FDAUTO.BAT', startup))
    else:
        put(boot, text_file(root, 'CONFIG.SYS', config))
        put(boot, text_file(root, 'AUTOEXEC.BAT', startup))
    with volume.open('r+b') as file:
        data = bytearray(file.read(512))
        struct.pack_into('<HHI', data, 24, 63, 16, 63)
        file.seek(0)
        file.write(data)
    count = volume.stat().st_size // 512
    mbr = bytearray(63 * 512)
    partition(mbr, count)
    cylinders = (count + 63 + 1007) // 1008
    with disk.open('wb') as file:
        file.write(mbr)
        with volume.open('rb') as source:
            shutil.copyfileobj(source, file)
        file.truncate(cylinders * 1008 * 512)
    return boot, disk, cylinders


def run_vm(boot, disk, cylinders, hard, hard_cylinders, floppy=None, protected=False, backend=None,
           target_bus="ide.1", target_unit=0, system_backend=None, target_kind="ide", timeout=120):
    assert target_kind in ["ide", "virtio"]
    argv = ['qemu-system-i386', '-accel', 'tcg', '-m', '64', '-display', 'none',
            '-serial', 'none', '-monitor', 'stdio', '-no-reboot', '-boot', 'a',
            '-drive', f'file={boot},format=raw,if=floppy,index=0',
            '-drive', f'file={system_backend or disk},format=raw,if=none,id=systemdisk',
            '-device', f'ide-hd,drive=systemdisk,bus=ide.0,unit=0,cyls={cylinders},heads=16,secs=63,bios-chs-trans=none',
            '-drive', f'file={backend or hard},format=raw,if=none,id=testdisk',
            '-device', (f'ide-hd,drive=testdisk,bus={target_bus},unit={target_unit},cyls={hard_cylinders},heads=16,secs=63,bios-chs-trans=none'
                        if target_kind == 'ide' else 'virtio-blk-pci,drive=testdisk'),
            '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']
    if floppy:
        argv += ['-drive', f'file={floppy},format=raw,if=floppy,index=1' +
                 (',readonly=on' if protected else '')]
    with subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
        try:
            process.wait(timeout=timeout)
            _, error = process.communicate()
        except subprocess.TimeoutExpired:
            # Keep the monitor pipe available until timeout so screen-driven
            # tests report the actual DOS prompt instead of only a deadline.
            screen = hard.parent / 'timeout-screen.bin'
            request = f'pmemsave 0xb8000 4000 "{screen}"\n'.encode()
            try:
                process.communicate(input=request, timeout=2)
            except subprocess.TimeoutExpired:
                pass
            process.kill()
            _, error = process.communicate()
            if screen.exists():
                cells = screen.read_bytes()[::2].decode('cp437')
                print('QEMU timeout screen:\n' + '\n'.join(
                    cells[row:row + 80] for row in range(0, len(cells), 80)), flush=True)
            raise AssertionError('QEMU BIOS test timed out: ' + error.decode(errors='replace'))
        assert process.returncode == 33, error.decode(errors='replace')
