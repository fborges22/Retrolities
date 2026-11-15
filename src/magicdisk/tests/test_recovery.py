#!/usr/bin/env python3
"""Recover orphan graphs, preserving source bytes and recording full provenance."""
from pathlib import Path
import subprocess
import resource
import signal
import sys
import tempfile
from directory_fixture import create
from lost_fixture import lost_fixture
from recovery_check import verify

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0, contains=None):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    if contains:
        assert contains in result.stdout, result.stdout
    return result.stdout




def limit_output():
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))


with tempfile.TemporaryDirectory(prefix='magicdisk-recover-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'fat{bits}.img'
        layout = create(path, bits)
        clean = path.read_bytes()
        destination = root / f'output{bits}'
        destination.mkdir()
        run(path, '--recover-lost', str(destination), contains='0/0 clusters')
        assert list(destination.iterdir()) == []
        broken, payloads = lost_fixture(clean, bits)
        if bits == 32:
            broken[512:1024] = bytes(512)
            broken[7 * 512 + 8] = 1
        path.write_bytes(broken)
        run(path, '--recover-lost', str(destination), contains='13/13 clusters')
        first = destination / 'REC00001'
        manifest = verify(first, payloads)
        assert path.read_bytes() == broken
        run(path, '--recover-lost', str(destination), contains='13/13 clusters')
        verify(destination / 'REC00002', payloads)
        assert (first / 'RECOVERY.TXT').read_text() == manifest
        assert path.read_bytes() == broken
        # Inconsistent mirrors or damaged live file chains make ownership unreliable.
        unsafe = bytearray(broken)
        unsafe[layout.reserved * 512 + 100] ^= 1
        path.write_bytes(unsafe)
        run(path, '--recover-lost', str(destination), code=2, contains='complete directory scan')
        assert not (destination / 'REC00003').exists() and path.read_bytes() == unsafe
        path.write_bytes(broken)
        run(path, '--recover-lost', str(root / 'missing' / 'directory'), code=2, contains='Cannot create')
        assert path.read_bytes() == broken
        run(path, '--recover-lost', str(destination), '--apply', '--journal', str(root / 'bad.mdj'), code=2)
        assert not (root / 'bad.mdj').exists()

        limited = root / f'limited{bits}'
        limited.mkdir()
        failure = subprocess.run([str(exe), str(path), '--recover-lost', str(limited)],
                                 capture_output=True, text=True, preexec_fn=limit_output)
        assert failure.returncode == 2, failure.stdout + failure.stderr
        assert 'INCOMPLETE' in failure.stdout
        partial = limited / 'REC00001'
        assert partial.is_dir() and list(partial.glob('*.CHK'))
        assert 'FINAL COMPLETE' not in (partial / 'RECOVERY.TXT').read_text()
        assert path.read_bytes() == broken

        if len(sys.argv) > 2:
            for mode in ['gap', 'cancel', 'scan-cancel', 'fat-read', 'destination']:
                output = root / f'{bits}-{mode}'
                output.mkdir()
                subprocess.run([str(Path(sys.argv[2]).resolve()), str(path), str(output), mode], check=True)
                assert path.read_bytes() == broken
                if mode in ['gap', 'cancel']:
                    manifest = (output / 'REC00001' / 'RECOVERY.TXT').read_text()
                    data = (output / 'REC00001' / 'F0000001.CHK').read_bytes()
                    if mode == 'gap':
                        assert 'FINAL COMPLETE_WITH_GAPS' in manifest and 'unreadable=1' in manifest
                        assert 'UNREADABLE file=F0000001.CHK offset=0 ' in manifest
                        assert data[:512] == bytes(512)
                        assert data[512:len(payloads[1000])] == payloads[1000][512:]
                        assert data[len(payloads[1000]):] == payloads[1001] + payloads[1002]
                    else:
                        assert 'FINAL INCOMPLETE' in manifest and 'clusters=1 ' in manifest
                        assert data == payloads[1000]
                else:
                    assert list(output.iterdir()) == []

print('PASS: FAT12/16/32 lost chains, cycles, shared tails, invalid links, exact payloads, no overwrite and source preservation')
