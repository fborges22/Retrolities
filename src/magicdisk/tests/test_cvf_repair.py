#!/usr/bin/env python3
"""Journaled CVF repairs: each damage is fixed so the independent checker
reports zero issues, and the journal restores the damaged bytes exactly."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from cvf_fixture import FIRST_DATA, entry, volume

exe = Path(sys.argv[1]).resolve()


def cli(*args):
    return subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix='magicdisk-cvf-repair-') as directory:
    root = Path(directory)
    path = root / 'DBLSPACE.000'
    cvf, entries = volume()
    good = cvf.finish(entries)
    md_entry = lambda c: 6 * 512 + (c + FIRST_DATA) * 4
    entry_of = lambda d, c: struct.unpack_from('<I', d, md_entry(c))[0]

    def fat_set(d, cluster, value):
        offset = (cvf.boot + cvf.res) * 512 + cluster + cluster // 2
        pair = struct.unpack_from('<H', d, offset)[0]
        pair = (pair & 0xf) | (value << 4) if cluster & 1 else (pair & 0xf000) | value
        struct.pack_into('<H', d, offset, pair)

    def clear_used(d):
        struct.pack_into('<I', d, md_entry(2), entry_of(d, 2) & 0x7fffffff)

    def stale(d):
        struct.pack_into('<I', d, md_entry(40), (1 << 31) | (1 << 30) | (cvf.heap + 300 - 1))

    def heap_crosslink(d):
        struct.pack_into('<I', d, md_entry(3), entry_of(d, 2))

    def reserved_bit(d):
        struct.pack_into('<I', d, md_entry(2), entry_of(d, 2) | (1 << 21))

    def size(d):
        struct.pack_into('<I', d, cvf.root * 512 + 28, 9000)

    def lost(d):
        fat_set(d, 50, 0xfff)
        struct.pack_into('<I', d, md_entry(50), (1 << 31) | (1 << 30) | (cvf.heap + 350 - 1))

    def file_crosslink(d):
        struct.pack_into('<H', d, cvf.root * 512 + 32 + 26, 2)   # TWO.BIN starts at ONE.TXT

    def bitfat(d):
        d[512] ^= 0x80

    def header(d):
        d[(cvf.boot + cvf.res) * 512] = 0xf0

    def invalid_chain(d):
        fat_set(d, 3, 70)                                        # TWO.BIN -> free cluster

    damages = {'clear_used': clear_used, 'stale': stale, 'heap_crosslink': heap_crosslink,
               'reserved_bit': reserved_bit, 'size': size, 'lost': lost,
               'file_crosslink': file_crosslink, 'bitfat': bitfat, 'header': header,
               'invalid_chain': invalid_chain}
    damages['everything'] = lambda d: [f(d) for f in (stale, reserved_bit, size, lost, bitfat,
                                                       header)]
    for name, damage in damages.items():
        data = bytearray(good)
        damage(data)
        data = bytes(data)
        path.write_bytes(data)
        before = cli('--cvf', path)
        assert before.returncode in (0, 1), before.stdout
        preview = cli('--cvf', path, '--repair')
        assert preview.returncode == 0, (name, preview.stdout)
        assert 'preview only' in preview.stdout and path.read_bytes() == data, name
        journal = root / f'{name}.mdj'
        applied = cli('--cvf', path, '--repair', '--apply', '--journal', journal)
        assert applied.returncode == 0 and 'written and verified' in applied.stdout, \
            (name, applied.stdout)
        after = cli('--cvf', path)
        assert after.returncode == 0 and 'complete: 0 issues' in after.stdout, (name, after.stdout)
        assert 'CVF_' not in after.stdout, (name, after.stdout)
        repaired = path.read_bytes()
        if name == 'heap_crosslink':
            moved = (entry_of(repaired, 3) & 0x1fffff) + 1
            original = (entry_of(good, 3) & 0x1fffff) + 1
            shared = (entry_of(good, 2) & 0x1fffff) + 1
            assert moved != shared
            assert repaired[moved * 512:(moved + 1) * 512] == good[shared * 512:(shared + 1) * 512]
            assert original
        if name == 'lost':
            assert b'FILE0000CHK' in repaired[cvf.root * 512:(cvf.root + 32) * 512]
        if name == 'file_crosslink':
            two = struct.unpack_from('<H', repaired, cvf.root * 512 + 32 + 26)[0]
            assert two not in (2,), two
            src = (entry_of(repaired, 2) & 0x1fffff) + 1
            dst = (entry_of(repaired, two) & 0x1fffff) + 1
            assert src != dst and repaired[dst * 512:dst * 512 + 600] == repaired[src * 512:src * 512 + 600]
        again = cli('--cvf', path, '--repair')
        assert '0 repairs' in again.stdout, (name, again.stdout)
        stamp = journal.read_bytes()
        assert cli('--cvf', path, '--repair', '--apply', '--journal', journal).returncode == 0
        assert journal.read_bytes() == stamp, 'nothing to write must not touch the journal'
        restored = cli('--cvf', path, '--restore-journal', journal, '--apply')
        assert restored.returncode == 0, (name, restored.stdout)
        assert path.read_bytes() == data, name
        cli('--cvf', path, '--restore-journal', journal, '--apply')
        assert path.read_bytes() == data, name

    # Unrelated later changes block undo without writing.
    data = bytearray(good)
    size(data)
    path.write_bytes(bytes(data))
    journal = root / 'guard.mdj'
    assert cli('--cvf', path, '--repair', '--apply', '--journal', journal).returncode == 0
    changed = bytearray(path.read_bytes())
    changed[cvf.root * 512 + 28] ^= 1
    path.write_bytes(bytes(changed))
    refused = cli('--cvf', path, '--restore-journal', journal, '--apply')
    assert refused.returncode == 2 and 'unrelated changes' in refused.stdout
    assert path.read_bytes() == bytes(changed)

    # An existing journal is never overwritten.
    data = bytearray(good)
    size(data)
    path.write_bytes(bytes(data))
    out = cli('--cvf', path, '--repair', '--apply', '--journal', journal)
    assert out.returncode == 2 and 'already exists' in out.stdout, out.stdout
    assert path.read_bytes() == bytes(data)

    # Data that cannot be recovered is reported, not invented.
    data = bytearray(good)
    struct.pack_into('<I', data, md_entry(2), (1 << 30) | (cvf.boot - 1))
    path.write_bytes(bytes(data))
    out = cli('--cvf', path, '--repair')
    assert out.returncode == 1 and 'CVF_UNRESOLVED' in out.stdout, out.stdout

    # Option validation.
    assert cli('--cvf', path, '--repair', '--apply').returncode == 2
    assert cli('--cvf', path, '--apply').returncode == 2
    # Every write position fails once: the CVF must come back byte-identical.
    faulty = Path(sys.argv[2]).resolve()
    data = bytearray(good)
    damages['everything'](data)
    data = bytes(data)
    path.write_bytes(data)
    preview = cli('--cvf', path, '--repair').stdout
    sectors = int(preview.split(' sectors ')[0].split()[-1])
    assert sectors > 3, preview
    for position in range(1, sectors + 1):
        path.write_bytes(data)
        journal = root / f'fault{position}.mdj'
        result = subprocess.run([str(faulty), '--cvf', str(path), '--repair', '--apply',
                                 '--journal', str(journal)], capture_output=True, text=True,
                                env={'MAGICDISK_CVF_FAIL_WRITE': str(position), 'PATH': '/usr/bin:/bin'})
        assert result.returncode == 2, (position, result.stdout)
        assert 'original CVF sectors restored and verified' in result.stdout, result.stdout
        assert path.read_bytes() == data, position
        assert journal.exists()
print('PASS: CVF repairs (MDFAT flags, stale entries, heap and file cross-link copies, chains, '
      'sizes, lost chains, BitFAT, FAT header) with exact journal undo and verified rollback '
      'at every write position')
