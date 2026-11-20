"""Start the Polaris demo machine (see mkdemo.py) under QEMU."""
import pathlib
import qemu

ROOT = pathlib.Path(__file__).resolve().parents[3]
DEMO = ROOT / 'build' / 'polaris' / 'demo'


def demo_vm(cd='freedos-cd.iso', floppy=None, extra=()):
    args = ['-drive', f'file={DEMO / "disk0.img"},format=raw,if=ide,index=0',
            '-drive', f'file={DEMO / cd},format=raw,media=cdrom,if=ide,index=2',
            '-device', 'lsi53c895a,id=scsi0',
            '-drive', f'file={DEMO / "disk1.img"},format=raw,if=none,id=sd1',
            '-device', 'scsi-hd,drive=sd1,bus=scsi0.0',
            '-boot', 'c']
    if floppy:
        args += ['-drive', f'file={floppy},format=raw,if=floppy,index=0']
    return qemu.VM(args + list(extra))
