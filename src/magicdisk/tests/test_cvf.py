#!/usr/bin/env python3
"""Read-only DoubleSpace/DriveSpace CVF checks on volumes built from the
documented layout (Tech Help! CVF regions). Real DBLSPACE output may differ
in reserved fields; the checker resolves sector conventions from the stamps."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()
FIRST_DATA = 3


class Cvf:
    def __init__(self, oem=b'MSDSP6.0', logical_heap=True):
        self.pages = 1                       # BitFAT: 1 x 2 KB page = 4 sectors (1..4)
        self.mdfat_start = 5                 # logical; file sector 6 (after Res1 at 5)
        self.mdfat_sectors = 8               # 1024 entries
        self.boot = 6 + self.mdfat_sectors + 31   # Res2 = 31 sectors
        self.res, self.fatsz, self.clusters = 2, 2, 120
        self.root = self.boot + self.res + self.fatsz
        self.heap = self.root + 32 + 2
        self.heap_size = 400
        self.total_sectors = self.heap + self.heap_size + 1
        self.data = bytearray(self.total_sectors * 512)
        self.fat = [0] * (self.clusters + 2)
        self.fat[0], self.fat[1] = 0xff8, 0xfff
        self.mdfat = {}
        self.next_heap = self.heap
        self.root_entries = []
        md = self.data
        md[0:3] = b'\xeb\x3c\x90'
        md[3:11] = oem
        inner_total = self.res + 2 * self.fatsz + 32 + self.clusters * 16
        struct.pack_into('<HBHBHHBH', md, 11, 512, 16, self.res, 2, 512, inner_total, 0xf8,
                         self.fatsz)
        struct.pack_into('<H', md, 0x24, self.mdfat_start)
        md[0x26] = 9
        struct.pack_into('<H', md, 0x27, self.boot)
        struct.pack_into('<H', md, 0x29, self.root - self.boot)
        struct.pack_into('<H', md, 0x2b, self.heap - 1 if logical_heap else self.heap)
        struct.pack_into('<H', md, 0x2d, FIRST_DATA)
        md[0x2f] = self.pages
        md[0x32] = 4
        md[0x3d] = 1
        struct.pack_into('<H', md, 0x3e, 2)
        boot = self.boot * 512
        self.data[boot:boot + 62] = md[0:62]
        self.data[boot + 3:boot + 11] = b'MSDOS5.0'
        struct.pack_into('<H', self.data, boot + 510, 0xaa55)
        self.data[(self.boot + 1) * 512:(self.boot + 1) * 512 + 4] = b'\xf8DR\x00'
        last = (self.total_sectors - 1) * 512
        self.data[last:last + 4] = b'MDR\x00'

    def store(self, cluster, payload, compressed=False, sparse=False):
        if sparse:
            self.mdfat[cluster] = 0
            return
        count = max(1, (len(payload) + 511) // 512)
        location = self.next_heap
        self.next_heap += count
        start = location * 512
        if compressed:
            payload = b'MD\x00\x02' + payload
        self.data[start:start + len(payload)] = payload
        raw = 0 if compressed else 1
        self.mdfat[cluster] = ((1 << 31) | (raw << 30) | (15 << 26) | ((count - 1) << 22) |
                               (location - 1))

    def file(self, name, size, clusters, directory=False, entries=None, compressed=False):
        for index, cluster in enumerate(clusters):
            self.fat[cluster] = clusters[index + 1] if index + 1 < len(clusters) else 0xfff
            if directory:
                self.store(cluster, b''.join(entries), compressed=compressed)
            else:
                self.store(cluster, bytes([65 + index]) * 600, compressed=compressed)
        return entry(name, 0x10 if directory else 0x20, clusters[0] if clusters else 0, size)

    def finish(self, root):
        for index, record in enumerate(root):
            offset = self.root * 512 + index * 32
            self.data[offset:offset + 32] = record
        fat = bytearray(self.fatsz * 512)
        for cluster, value in enumerate(self.fat):
            offset = cluster + cluster // 2
            pair = struct.unpack_from('<H', fat, offset)[0]
            pair = (pair & 0xf) | (value << 4) if cluster & 1 else (pair & 0xf000) | value
            struct.pack_into('<H', fat, offset, pair)
        start = (self.boot + self.res) * 512
        self.data[start:start + len(fat)] = fat
        for cluster, value in self.mdfat.items():
            struct.pack_into('<I', self.data, 6 * 512 + (cluster + FIRST_DATA) * 4, value)
        for sector in range(self.heap, self.next_heap):
            bit = sector - self.heap
            word = 512 + (bit // 16) * 2
            value = struct.unpack_from('<H', self.data, word)[0] | (0x8000 >> (bit % 16))
            struct.pack_into('<H', self.data, word, value)
        return bytes(self.data)


def entry(name, attribute, cluster, size):
    stem, _, ext = name.partition('.')
    record = bytearray(32)
    record[0:11] = stem.ljust(8).encode() + ext.ljust(3).encode()
    record[11] = attribute
    struct.pack_into('<HI', record, 26, cluster, size)
    return bytes(record)


def volume(compressed_dir=False, **options):
    cvf = Cvf(**options)
    inner = [entry('.', 0x10, 10, 0), entry('..', 0x10, 0, 0),
             cvf.file('INNER.TXT', 9000, [11, 12])]
    root = [cvf.file('ONE.TXT', 600, [2]),
            cvf.file('TWO.BIN', 16000, [3, 5], compressed=True),
            cvf.file('ZERO.DAT', 8192, [6]),
            cvf.file('SUB', 0, [10], directory=True, entries=inner, compressed=compressed_dir)]
    cvf.store(6, b'', sparse=True)
    return cvf, root


def run(path, code, *contains):
    result = subprocess.run([str(exe), '--cvf', str(path)], capture_output=True, text=True)
    assert result.returncode == code, (code, result.returncode, result.stdout)
    for text in contains:
        assert text in result.stdout, (text, result.stdout)
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-cvf-') as directory:
    root_dir = Path(directory)
    path = root_dir / 'DRVSPACE.000'
    for options in [{}, {'oem': b'MSDBL6.0'}, {'logical_heap': False}]:
        cvf, root = volume(**options)
        good = cvf.finish(root)
        path.write_bytes(good)
        out = run(path, 0, 'complete: 0 issues', '4 files, 1 directories', '1 sparse',
                  'Nothing was written')
        assert ('DoubleSpace' if options.get('oem') else 'DriveSpace') in out
        assert path.read_bytes() == good

    cvf, root = volume()
    good = bytearray(cvf.finish(root))
    md_entry = lambda c: 6 * 512 + (c + FIRST_DATA) * 4

    def damaged(change, code, *expect):
        data = bytearray(good)
        change(data)
        path.write_bytes(data)
        run(path, code, *expect)
        assert path.read_bytes() == data

    def clear_used(d):
        struct.pack_into('<I', d, md_entry(2), struct.unpack_from('<I', d, md_entry(2))[0] & 0x7fffffff)
    damaged(clear_used, 1, 'CVF_MDFAT', 'allocated in the FAT but unused')

    def stale(d):
        struct.pack_into('<I', d, md_entry(40), (1 << 31) | (1 << 30) | (cvf.heap + 300 - 1))
    damaged(stale, 1, 'CVF_MDFAT_STALE')

    def crosslink(d):
        struct.pack_into('<I', d, md_entry(3), struct.unpack_from('<I', d, md_entry(2))[0])
    damaged(crosslink, 1, 'CVF_HEAP_CROSSLINK')

    def outside(d):
        struct.pack_into('<I', d, md_entry(2), (1 << 31) | (1 << 30) | (cvf.boot - 1))
    damaged(outside, 1, 'CVF_HEAP_RANGE')

    def reserved_bit(d):
        struct.pack_into('<I', d, md_entry(2), struct.unpack_from('<I', d, md_entry(2))[0] | (1 << 21))
    damaged(reserved_bit, 1, 'CVF_MDFAT', 'reserved bit 21')

    def tag(d):
        location = (struct.unpack_from('<I', d, md_entry(3))[0] & 0x1fffff) + 1
        d[location * 512:location * 512 + 4] = b'XXXX'
    damaged(tag, 1, 'CVF_TAG')

    def stamp2(d):
        d[-512:-508] = b'\0\0\0\0'
    damaged(stamp2, 1, 'CVF_STAMP', 'MdStamp2')

    def stamp1(d):
        d[(cvf.boot + 1) * 512] = 0
    damaged(stamp1, 1, 'CVF_STAMP', 'MdStamp1')

    def size(d):
        struct.pack_into('<I', d, cvf.root * 512 + 28, 9000)
    damaged(size, 1, 'CVF_FILE_SIZE', 'ONE.TXT')

    def lost(d):
        start = (cvf.boot + cvf.res) * 512
        offset = start + 50 + 50 // 2
        pair = struct.unpack_from('<H', d, offset)[0]
        struct.pack_into('<H', d, offset, (pair & 0xf000) | 0xfff)
        struct.pack_into('<I', d, md_entry(50), (1 << 31) | (1 << 30) | (cvf.heap + 350 - 1))
    damaged(lost, 1, 'CVF_LOST', '1 allocated clusters')

    def xlinked_fat(d):
        struct.pack_into('<H', d, cvf.root * 512 + 26 + 32, 2)
    damaged(xlinked_fat, 1, 'CVF_CROSSLINK')

    def bitfat(d):
        d[512] ^= 0x80
    damaged(bitfat, 0, 'CVF_BITFAT', 'rebuilds the BitFAT')

    def header(d):
        d[(cvf.boot + cvf.res) * 512] = 0xf0
    damaged(header, 1, 'CVF_FAT_HEADER')

    # A compressed directory cannot be expanded here: the check is partial.
    cvf, root = volume(compressed_dir=True)
    path.write_bytes(cvf.finish(root))
    run(path, 2, 'CVF_PARTIAL', 'check PARTIAL: 0 issues')

    # Not a CVF: refused without findings.
    plain = root_dir / 'PLAIN.IMG'
    plain.write_bytes(bytes(512 * 200))
    run(plain, 2, 'not a DoubleSpace or DriveSpace volume')
print('PASS: CVF MDBPB/stamps, MDFAT use, heap range/cross-links, tags, BitFAT, inner FAT '
      'chains, sizes and lost clusters (read-only)')
