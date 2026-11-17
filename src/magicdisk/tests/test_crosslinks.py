#!/usr/bin/env python3
"""Cross-link copies, lost-chain adoption and one-step autofix with exact undo."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from chain_fixture import set_fat
from directory_fixture import Layout
from xlink_fixture import build, start_of, chain, fsck, read_file

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0, contains=None):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, (args, result.returncode, result.stdout + result.stderr)
    if contains:
        assert contains in result.stdout, result.stdout
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-xlink-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        work = root / f'w{bits}'
        work.mkdir()
        path = root / f'fat{bits}.img'
        payload = build(path, bits, work)
        clean = path.read_bytes()
        layout = Layout(clean, bits)
        unit = layout.spc * 512
        _, alpha = start_of(layout, 'ALPHA.BIN')
        _, bravo = start_of(layout, 'BRAVO.BIN')
        charlie_entry, charlie = start_of(layout, 'CHARLIE.BIN')
        a_chain, b_chain = chain(layout, alpha), chain(layout, bravo)
        c_chain = chain(layout, charlie)
        assert len(a_chain) == len(b_chain) == len(c_chain) == 3
        broken = bytearray(clean)
        # BRAVO's second link now enters ALPHA's tail; CHARLIE starts inside ALPHA.
        set_fat(broken, bits, b_chain[0], a_chain[1])
        struct.pack_into('<H', broken, charlie_entry + 26, a_chain[0] & 0xffff)
        if bits == 32:
            struct.pack_into('<H', broken, charlie_entry + 20, a_chain[0] >> 16)
        broken = bytes(broken)
        path.write_bytes(broken)
        scan = run(path, code=1)
        assert 'CHAIN_SHARED' in scan and 'LOST_CLUSTERS' in scan

        # Cross-link copies keep every byte the later files referenced.
        preview = run(path, '--repair-crosslinks', contains='Copy cross-linked clusters for 2 files')
        assert preview.count('XLINK_COPY') == 2
        assert path.read_bytes() == broken
        x_journal = root / f'x{bits}.mdj'
        run(path, '--repair-crosslinks', '--apply', '--journal', str(x_journal),
            contains='committed')
        version = struct.unpack_from('<I', x_journal.read_bytes(), 8)[0]
        assert version == 24
        after = run(path, code=1)
        assert 'CHAIN_SHARED' not in after and 'LOST_CLUSTERS' in after
        a, b = payload['ALPHA.BIN'], payload['BRAVO.BIN']
        assert read_file(path, 'ALPHA.BIN') == a
        assert read_file(path, 'BRAVO.BIN') == b[:unit] + a[unit:]
        assert read_file(path, 'CHARLIE.BIN') == a
        copied = path.read_bytes()
        if len(sys.argv) > 2 and bits == 12:
            (root / 'fault-broken.img').write_bytes(broken)
            (root / 'fault-copied.img').write_bytes(copied)
            faults = subprocess.run([sys.argv[2], str(root / 'fault-broken.img'), str(a_chain[1]),
                                     str(root / 'fault.mdj'), str(root / 'fault-copied.img'),
                                     str(start_of(layout, 'DELTA.BIN')[1]), str(root)],
                                    capture_output=True, text=True)
            assert faults.returncode == 0, faults.stdout + faults.stderr

        # Lost chains (old BRAVO tail, all of CHARLIE) become root .CHK files.
        lost_preview = run(path, '--convert-lost', contains='root FILEnnnn.CHK files')
        assert lost_preview.count('LOST_TO_FILE') == 2
        l_journal = root / f'l{bits}.mdj'
        run(path, '--convert-lost', '--apply', '--journal', str(l_journal), contains='committed')
        assert struct.unpack_from('<I', l_journal.read_bytes(), 8)[0] == 25
        run(path, contains=' 0 issues')
        fsck(path)
        chk = {read_file(path, 'FILE0000.CHK'), read_file(path, 'FILE0001.CHK')}
        assert chk == {b[unit:], payload['CHARLIE.BIN']}, [len(x) for x in chk]
        run(path, '--convert-lost', contains='already up to date')

        # Exact undo in reverse order; journals are refused out of order.
        run(path, '--restore-journal', str(x_journal), '--apply', code=2)
        run(path, '--restore-journal', str(l_journal), '--apply')
        assert path.read_bytes() == copied
        run(path, '--restore-journal', str(x_journal), '--apply')
        assert path.read_bytes() == broken

        # One-step autofix reaches the same clean state with per-step journals.
        preview = run(path, '--autofix', code=1, contains='Copy cross-linked clusters')
        assert 'PLAN' in preview and path.read_bytes() == broken
        journals = root / f'j{bits}'
        journals.mkdir()
        fixed = run(path, '--autofix', '--apply', '--journal', str(journals))
        assert 'After: complete, 0 issues' in fixed, fixed
        fsck(path)
        assert read_file(path, 'BRAVO.BIN') == b[:unit] + a[unit:]
        names = sorted(p.name for p in journals.iterdir())
        expected_names = ['MDFX0001.MDJ', 'MDFX0002.MDJ'] + (['MDFX0003.MDJ'] if bits == 32 else [])
        assert names == expected_names, (names, fixed)
        if bits == 32:
            assert 'Rebuild FAT32 FSInfo' in fixed
        again = run(path, '--autofix', '--apply', '--journal', str(journals))
        assert 'Committed 0 repairs' in again
        for name in reversed(names):
            run(path, '--restore-journal', str(journals / name), '--apply')
        assert path.read_bytes() == broken

        # Lost-chain policy: free instead of adopt.
        path.write_bytes(clean)
        lost = bytearray(clean)
        set_fat(lost, bits, b_chain[0], 0xfff if bits == 12 else 0xffff if bits == 16 else 0x0fffffff)
        path.write_bytes(lost)
        free_dir = root / f'f{bits}'
        free_dir.mkdir()
        out = run(path, '--autofix', '--lost-chains', 'free', '--apply', '--journal', str(free_dir))
        assert 'After: complete, 0 issues' in out, out
        assert 'FILE0000.CHK' not in subprocess.run(['mdir', '-i', str(path), '::/'],
                                                    capture_output=True, text=True).stdout
        keep_dir = root / f'k{bits}'
        keep_dir.mkdir()
        path.write_bytes(lost)
        run(path, '--autofix', '--lost-chains', 'keep', '--apply', '--journal', str(keep_dir),
            code=1, contains='Committed 1 repairs')

        # Full fixed root (FAT12/16) refuses adoption; FAT32 grows its root chain.
        full = bytearray(copied)
        full_layout = Layout(bytes(full), bits)
        if bits != 32:
            for offset in full_layout.slots():
                if full[offset] in (0, 0xe5):
                    full[offset:offset + 32] = b'ZZ%06d   ' % (offset % 1000000) + bytes([0x20]) + bytes(20)
            path.write_bytes(full)
            run(path, '--convert-lost', code=2, contains='Root directory is full')
        else:
            # Fill every root slot so adoption must allocate a new root cluster.
            for offset in full_layout.slots():
                if full[offset] in (0, 0xe5):
                    full[offset:offset + 32] = b'ZZ%06d   ' % (offset % 1000000) + bytes([0x20]) + bytes(20)
            path.write_bytes(full)
            g_journal = root / 'grow.mdj'
            run(path, '--convert-lost', '--apply', '--journal', str(g_journal), contains='committed')
            run(path, contains=' 0 issues')
            fsck(path)
            assert len(chain(Layout(path.read_bytes(), bits), full_layout.root)) == 2
            run(path, '--restore-journal', str(g_journal), '--apply')
            assert path.read_bytes() == full
    print('Cross-link, lost-chain and autofix tests passed')
