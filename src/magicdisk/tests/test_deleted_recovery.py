#!/usr/bin/env python3
"""Deleted-file exports exclude live data and disclose every recovery assumption."""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from deleted_fixture import fixture, DELETED_NAME
from chain_fixture import chain_fixture


def verify(directory, expected, evidence):
    manifest = (directory / 'RECOVERY.TXT').read_text()
    assert 'FINAL COMPLETE_UNVERIFIED' in manifest
    assert 'METHOD CONTIGUOUS_GUESS' in manifest
    assert 'METHOD RETAINED_UNOWNED_CHAIN' in manifest
    names = re.findall(r'NAME_CANDIDATE source=deleted-lfn-order-inferred confidence=UNVERIFIED '
                       r'short_first_hex=([0-9A-F]{2}) name_hex=([0-9A-Fa-f]+)', manifest)
    assert [(first, bytes.fromhex(name).decode('utf-8')) for first, name in names] == [('44', DELETED_NAME)]
    found = {}
    for block in manifest.split('DELETED_BEGIN ')[1:]:
        entry = re.match(r'entry_lba=(\d+) offset=(\d+)', block)
        offset = int(entry[1]) * 512 + int(entry[2])
        files = re.findall(r'^FILE_BEGIN (\S+)', block, re.M)
        end = re.search(r'DELETED_END recovered=(\d+) missing=(\d+) stop=(\S+)', block)
        payload = b''.join((directory / name).read_bytes() for name in files)
        assert len(payload) == int(end[1])
        found[offset] = payload, end[3]
        assert 'confidence=UNVERIFIED' in block
    assert found == expected, {k: (len(v[0]), v[1]) for k, v in found.items()}
    for offset, raw in evidence.items():
        assert f'DELETED_SLOT entry_lba={offset // 512} offset={offset % 512} raw_hex={raw.hex().upper()}' in manifest


if __name__ == '__main__':
    exe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='magicdisk-deleted-') as directory:
        root = Path(directory)
        for bits in [12, 16, 32]:
            path = root / f'{bits}.img'
            original, expected, evidence = fixture(path, bits)
            output = root / str(bits)
            output.mkdir()
            result = subprocess.run([str(exe), str(path), '--recover-deleted', str(output)],
                                    capture_output=True, text=True)
            assert result.returncode == 1, result.stdout + result.stderr
            verify(output / 'REC00001', expected, evidence)
            assert path.read_bytes() == original
            if len(sys.argv) > 2:
                fault = str(Path(sys.argv[2]).resolve())
                for mode in ['gap', 'transient', 'cancel', 'retry-cancel',
                             'scan-cancel', 'fat-read', 'destination']:
                    output = root / f'{bits}-{mode}'
                    output.mkdir()
                    result = subprocess.run([fault, str(path), str(output), mode, 'deleted'],
                                            capture_output=True, text=True)
                    assert result.returncode == 0, result.stdout + result.stderr
                    assert path.read_bytes() == original
                    if mode == 'transient':
                        verify(output / 'REC00001', expected, evidence)
                    elif mode == 'gap':
                        changed = dict(expected)
                        first = min(expected)
                        payload, stop = changed[first]
                        changed[first] = bytes(512) + payload[512:], stop
                        verify(output / 'REC00001', changed, evidence)
                    elif mode in ['cancel', 'retry-cancel']:
                        manifest = (output / 'REC00001' / 'RECOVERY.TXT').read_text()
                        assert 'FINAL INCOMPLETE' in manifest
                        data = (output / 'REC00001' / 'F0000001.CHK').read_bytes()
                        assert len(data) == (512 if mode == 'cancel' else 0)
                    else:
                        assert not list(output.iterdir())
            ambiguous = root / f'ambiguous{bits}.img'
            ambiguous.write_bytes(chain_fixture(original, bits)[0])
            refused = root / f'refused{bits}'
            refused.mkdir()
            result = subprocess.run([str(exe), str(ambiguous), '--recover-deleted', str(refused)],
                                    capture_output=True, text=True)
            assert result.returncode == 2 and not list(refused.iterdir())
            if bits == 32:
                partitioned = root / 'partitioned.img'
                header = bytearray(63 * 512)
                header[446 + 4] = 0x0c
                struct.pack_into('<II', header, 446 + 8, 63, len(original) // 512)
                header[510:512] = b'\x55\xaa'
                partitioned.write_bytes(header + original)
                output = root / 'partitioned'
                output.mkdir()
                result = subprocess.run([str(exe), str(partitioned), '--partition', '1',
                                         '--recover-deleted', str(output)],
                                        capture_output=True, text=True)
                assert result.returncode == 1, result.stdout + result.stderr
                verify(output / 'REC00001', {k + 63 * 512: v for k, v in expected.items()},
                       {k + 63 * 512: v for k, v in evidence.items()})
                assert partitioned.read_bytes() == header + original
    print('PASS: FAT12/16/32 deleted candidates, free guesses, retained chains, live exclusion, raw LFN and source preservation')
