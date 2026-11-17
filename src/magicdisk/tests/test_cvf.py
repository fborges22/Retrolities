#!/usr/bin/env python3
"""Read-only DoubleSpace/DriveSpace CVF checks on volumes built from the
documented layout (Tech Help! CVF regions). Real DBLSPACE output may differ
in reserved fields; the checker resolves sector conventions from the stamps."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from cvf_fixture import FIRST_DATA, entry, volume

exe = Path(sys.argv[1]).resolve()


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
        assert 'CVF_' not in out, out
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

    def loop(d):
        location = (struct.unpack_from('<I', d, md_entry(10))[0] & 0x1fffff) + 1
        d[location * 512 + 96:location * 512 + 128] = entry('LOOP', 0x10, 10, 0)
    damaged(loop, 1, 'CVF_CROSSLINK', 'LOOP')

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
