"""Independent mtools directory trees, shared by native and real-DOS tests."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile

PARENT = 'Pasta de projetos'
CHILD = 'Relatórios longos'
UNICODE_FILE = 'Relatório anual de manutenção.txt'


def command(*args):
    result = subprocess.run(args, capture_output=True,
                            env=dict(os.environ, LC_ALL='C.UTF-8'))
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace') + result.stdout.decode(errors='replace'))
    return result


def create(path, bits, size=None):
    path = Path(path)
    size = size or {12: 1440 * 1024, 16: 32 * 1024 * 1024, 32: 64 * 1024 * 1024}[bits]
    with path.open('wb') as file:
        file.truncate(size)
    command('mkfs.fat', '-F', str(bits), str(path))
    command('mmd', '-i', str(path), '::/' + PARENT)
    command('mmd', '-i', str(path), f'::/{PARENT}/{CHILD}')
    with tempfile.TemporaryDirectory(prefix='magicdisk-names-') as directory:
        names = [f'Long report number {i:02d}.txt' for i in range(50)]
        names += ['a' * 13, 'b' * 26, 'x' * 255, UNICODE_FILE]
        files = []
        for name in names:
            file = Path(directory) / name
            file.write_bytes(b'MagicDisk LFN preservation test.\r\n')
            files.append(str(file))
        for file in files:
            try:
                command('mcopy', '-i', str(path), file, '::/' if len(Path(file).name) == 255 else f'::/{PARENT}/{CHILD}/')
            except RuntimeError as error:
                raise RuntimeError(Path(file).name + ': ' + str(error)) from error
    return Layout(path.read_bytes(), bits)


def damaged_names(layout):
    """Keep names valid but break sizes, so findings must identify both LFNs."""
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    damaged = bytearray(layout.data)
    for cluster, name in [(layout.root, 'x' * 255), (child, UNICODE_FILE)]:
        _, entry = next((parts, short) for parts, short in layout.long_names(cluster)
                        if layout.decoded_name(parts) == name)
        struct.pack_into('<I', damaged, entry + 28, layout.spc * 512 + 1)
    return damaged


class Layout:
    def __init__(self, data, bits):
        self.data, self.bits = data, bits
        self.spc = data[13]
        self.reserved = struct.unpack_from('<H', data, 14)[0]
        self.fatsz = (struct.unpack_from('<H', data, 22)[0] or
                      struct.unpack_from('<I', data, 36)[0])
        self.root_sector = self.reserved + data[16] * self.fatsz
        self.root_size = (struct.unpack_from('<H', data, 17)[0] * 32 + 511) // 512
        self.data_sector = self.root_sector + self.root_size
        self.root = struct.unpack_from('<I', data, 44)[0] if bits == 32 else 0

    def cluster(self, offset):
        low = struct.unpack_from('<H', self.data, offset + 26)[0]
        return low | (struct.unpack_from('<H', self.data, offset + 20)[0] << 16
                      if self.bits == 32 else 0)

    def fat(self, cluster):
        offset = self.reserved * 512
        if self.bits == 12:
            pair = struct.unpack_from('<H', self.data, offset + cluster * 3 // 2)[0]
            return (pair >> 4 if cluster & 1 else pair) & 0xfff
        return struct.unpack_from('<H' if self.bits == 16 else '<I', self.data,
                                  offset + cluster * (self.bits // 8))[0] & 0x0fffffff

    def slots(self, cluster=None):
        cluster = self.root if cluster is None else cluster
        if not cluster:
            return list(range(self.root_sector * 512,
                              (self.root_sector + self.root_size) * 512, 32))
        result, seen = [], set()
        eoc = {12: 0xff8, 16: 0xfff8, 32: 0x0ffffff8}[self.bits]
        while cluster < eoc:
            assert cluster >= 2 and cluster not in seen
            seen.add(cluster)
            start = (self.data_sector + (cluster - 2) * self.spc) * 512
            result.extend(range(start, start + self.spc * 512, 32))
            cluster = self.fat(cluster)
        return result

    def entries(self, cluster=None):
        for offset in self.slots(cluster):
            if self.data[offset] == 0:
                break
            if self.data[offset] != 0xe5:
                yield offset

    def child(self, cluster=None):
        return next(offset for offset in self.entries(cluster)
                    if self.data[offset + 11] == 16 and self.data[offset] != ord('.'))

    def long_names(self, cluster):
        pending = []
        for offset in self.entries(cluster):
            if self.data[offset + 11] == 15:
                pending.append(offset)
            else:
                if pending:
                    yield pending, offset
                pending = []

    def decoded_name(self, offsets):
        fields = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
        raw = b''.join(self.data[offset + field:offset + field + 2]
                       for offset in reversed(offsets) for field in fields)
        return raw.decode('utf-16-le').split('\0')[0].rstrip('\uffff')
