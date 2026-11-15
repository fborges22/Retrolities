#!/usr/bin/env python3
"""Recover deleted trees without revisiting directories or exporting live data."""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from deleted_directory_fixture import fixture, wide_fixture
from directory_fixture import Layout
from chain_fixture import set_fat


def verify(directory, expected, directories=2, long_names=True):
    manifest = (directory / 'RECOVERY.TXT').read_text()
    assert 'FINAL COMPLETE_UNVERIFIED' in manifest
    assert manifest.count('DIRECTORY_BEGIN ') == directories
    found = {}
    for block in manifest.split('DELETED_BEGIN ')[1:]:
        match = re.match(r'entry_lba=(\d+) offset=(\d+)', block)
        offset = int(match[1]) * 512 + int(match[2])
        names = re.findall(r'^FILE_BEGIN (\S+)', block, re.M)
        found[offset] = b''.join((directory / name).read_bytes() for name in names)
        decoded = re.findall(r'NAME_CANDIDATE .* name_hex=(\w+)', block)
        if long_names:
            assert len(decoded) == 1
            assert bytes.fromhex(decoded[0]).decode('utf-8') == expected[offset].split(b'\r\n')[0].decode()
        assert 'parent_hex=' + 'deleted-directory@'.encode().hex().upper() in block
    assert found == expected, (len(found), len(expected))
    assert 'stop=end-marker' in manifest or 'stop=end-of-chain' in manifest


if __name__ == '__main__':
    exe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='magicdisk-deleted-tree-') as directory:
        root = Path(directory)
        for bits in [12, 16, 32]:
            for retained in [False, True]:
                path = root / f'{bits}-{retained}.img'
                original, expected, outer, inner = fixture(path, bits, retained)
                output = root / f'{bits}-{retained}'
                output.mkdir()
                result = subprocess.run([str(exe), str(path), '--recover-deleted', str(output)],
                                        capture_output=True, text=True)
                assert result.returncode == 1, result.stdout + result.stderr
                verify(output / 'REC00001', expected)
                assert path.read_bytes() == original
                layout = Layout(original, bits)
                first = (layout.data_sector + (outer - 2) * layout.spc) * 512
                if len(sys.argv) > 2:
                    for mode in ['directory-cancel', 'directory-read', 'directory-transient', 'directory-retry-cancel']:
                        path.write_bytes(original)
                        fault_output = root / f'{mode}-{bits}-{retained}'
                        fault_output.mkdir()
                        result = subprocess.run([str(Path(sys.argv[2]).resolve()), str(path),
                                                 str(fault_output), mode, 'deleted', str(first // 512)],
                                                capture_output=True, text=True, timeout=30)
                        assert result.returncode == 0, result.stdout + result.stderr
                        manifest = (fault_output / 'REC00001' / 'RECOVERY.TXT').read_text()
                        if mode == 'directory-transient':
                            verify(fault_output / 'REC00001', expected)
                            assert f'DIRECTORY_RETRY entry_lba={first // 512} attempts=3 state=readable' in manifest
                        elif mode == 'directory-read':
                            assert 'stop=unreadable-dot-sector' in manifest
                            assert f'DIRECTORY_RETRY entry_lba={first // 512} attempts=3 state=unreadable' in manifest
                        else:
                            assert 'FINAL INCOMPLETE' in manifest
                        assert path.read_bytes() == original
                for mode in ['self-reference', 'wrong-parent-alias', 'live-owned', 'cycle']:
                    if mode == 'cycle' and not retained:
                        continue
                    data = bytearray(original)
                    if mode == 'cycle':
                        for offset in layout.slots(outer):
                            if not data[offset]:
                                data[offset:offset + 11] = b'\xe5ABEL      '
                                data[offset + 11] = 8
                        set_fat(data, bits, outer, outer)
                    elif mode == 'live-owned':
                        offset = next(o for o in layout.slots()
                                      if data[o] == 0xe5 and data[o + 11] == 16)
                        target = layout.cluster(layout.child())
                        struct.pack_into('<H', data, offset + 26, target & 0xffff)
                        if bits == 32:
                            struct.pack_into('<H', data, offset + 20, target >> 16)
                    else:
                        slots = (list(range(first, first + layout.spc * 512, 32))
                                 if mode == 'self-reference' else layout.slots())
                        offset = next(o for o in slots if not data[o])
                        target = outer if mode == 'self-reference' else inner
                        data[offset:offset + 11] = b'\xe5LIAS      '
                        data[offset + 11] = 16
                        struct.pack_into('<H', data, offset + 26, target & 0xffff)
                        if bits == 32:
                            struct.pack_into('<H', data, offset + 20, target >> 16)
                    path.write_bytes(data)
                    variant = root / f'{mode}-{bits}-{retained}'
                    variant.mkdir()
                    result = subprocess.run([str(exe), str(path), '--recover-deleted', str(variant)],
                                            capture_output=True, text=True, timeout=30)
                    assert result.returncode == 1, result.stdout + result.stderr
                    manifest = (variant / 'REC00001' / 'RECOVERY.TXT').read_text()
                    if mode == 'live-owned':
                        assert 'state=live-owned-cluster' in manifest and 'DIRECTORY_BEGIN' not in manifest
                    else:
                        verify(variant / 'REC00001', expected)
                        reason = {'self-reference': 'state=already-queued',
                                  'wrong-parent-alias': 'stop=invalid-dot-entries',
                                  'cycle': 'stop=visited-directory-cluster'}[mode]
                        assert reason in manifest
                    assert path.read_bytes() == data
                for missing in [(0,), (32,), (0, 32)]:
                    data = bytearray(original)
                    for offset in missing:
                        data[first + offset:first + offset + 32] = bytes(32)
                    path.write_bytes(data)
                    variant = root / f'missing-{bits}-{retained}-{missing}'
                    variant.mkdir()
                    result = subprocess.run([str(exe), str(path), '--recover-deleted', str(variant)],
                                            capture_output=True, text=True, timeout=30)
                    assert result.returncode == 1, result.stdout + result.stderr
                    manifest = (variant / 'REC00001' / 'RECOVERY.TXT').read_text()
                    if retained and len(missing) == 1:
                        verify(variant / 'REC00001', expected)
                        evidence = (f'DIRECTORY_DOT_EVIDENCE start={outer} '
                                    f'self_valid={int(0 not in missing)} '
                                    f'parent_valid={int(32 not in missing)} '
                                    'confidence=UNVERIFIED raw_hex=' +
                                    data[first:first + 64].hex().upper())
                        assert evidence in manifest
                    else:
                        assert 'stop=invalid-dot-entries' in manifest
                        assert 'DELETED_BEGIN ' not in manifest
                    assert path.read_bytes() == data
                damaged = bytearray(original)
                struct.pack_into('<H', damaged, first + 26, 0)
                path.write_bytes(damaged)
                rejected = root / f'rejected-{bits}-{retained}'
                rejected.mkdir()
                result = subprocess.run([str(exe), str(path), '--recover-deleted', str(rejected)],
                                        capture_output=True, text=True)
                assert result.returncode == 1, result.stdout + result.stderr
                manifest = (rejected / 'REC00001' / 'RECOVERY.TXT').read_text()
                assert 'stop=invalid-dot-entries' in manifest
                assert 'DELETED_BEGIN ' not in manifest
                assert path.read_bytes() == damaged
        path = root / 'wide.img'
        original, expected = wide_fixture(path)
        output = root / 'wide'
        output.mkdir()
        result = subprocess.run([str(exe), str(path), '--recover-deleted', str(output)],
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 1, result.stdout + result.stderr
        verify(output / 'REC00001', expected, directories=41, long_names=False)
        assert path.read_bytes() == original
    print('PASS: deleted nested trees, free/retained directory chains, surviving child entries and dot guards')
