#!/usr/bin/env python3
"""Verify live export contents, path mapping, salvage lengths and source preservation."""
from pathlib import Path
import resource
import signal
import subprocess
import sys
import tempfile
from directory_fixture import create, command, UNICODE_FILE
from chain_fixture import chain_fixture
from entry_fixture import entry_fixture

exe = Path(sys.argv[1]).resolve()


def run(image, destination, code=0):
    result = subprocess.run([str(exe), str(image), '--export-files', str(destination)],
                            capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


def limit_output():
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))


def exports(root):
    result = {}
    manifest = (root / 'RECOVERY.TXT').read_text()
    path = None
    for line in manifest.splitlines():
        if line.startswith('LIVE_BEGIN '):
            path = bytes.fromhex(line.split('path_hex=')[1]).decode('utf-8')
            assert path not in result
            result[path] = bytearray()
        elif line.startswith('FILE_BEGIN '):
            result[path] += (root / line.split()[1]).read_bytes()
    return result, manifest


with tempfile.TemporaryDirectory(prefix='magicdisk-live-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        image = root / f'fat{bits}.img'
        layout = create(image, bits)
        empty = root / 'EMPTY.TXT'
        empty.write_bytes(b'')
        command('mcopy', '-i', str(image), str(empty), '::/EMPTY.TXT')
        original = image.read_bytes()
        destination = root / f'export{bits}'
        destination.mkdir()
        run(image, destination)
        files, manifest = exports(destination / 'REC00001')
        assert len(files) == 55
        assert files.pop('EMPTY.TXT') == b''
        assert any(name.endswith(UNICODE_FILE) for name in files)
        assert all(data == b'MagicDisk LFN preservation test.\r\n' for data in files.values())
        assert 'FINAL COMPLETE ' in manifest
        assert image.read_bytes() == original
        # Export again never overwrites a prior recovery directory.
        run(image, destination)
        assert exports(destination / 'REC00002')[0]['EMPTY.TXT'] == b''
        broken, _, entries = chain_fixture(original, bits)
        image.write_bytes(broken)
        run(image, destination, 1)
        salvaged, manifest = exports(destination / 'REC00003')
        assert len(salvaged) == 55
        assert 'mode=allocated-prefix' in manifest and 'FINAL COMPLETE_WITH_GAPS' in manifest
        lengths = [len(data) for name, data in salvaged.items() if 'Long report number' in name]
        assert lengths.count(layout.spc * 512) >= 4
        assert lengths.count(0) >= 2
        assert image.read_bytes() == broken
        limited = root / f'limited{bits}'
        limited.mkdir()
        failure = subprocess.run([str(exe), str(image), '--export-files', str(limited)],
                                 capture_output=True, preexec_fn=limit_output)
        assert failure.returncode == 2, failure.stdout
        partial = limited / 'REC00001' / 'RECOVERY.TXT'
        assert partial.exists() and 'FINAL COMPLETE' not in partial.read_text()
        assert image.read_bytes() == broken
        # Disagreeing allocation authorities must not silently choose file data.
        different = bytearray(original)
        different[(layout.reserved + layout.fatsz) * 512 + 20] ^= 1
        image.write_bytes(different)
        run(image, destination, 2)
        assert not (destination / 'REC00004').exists()
        assert image.read_bytes() == different
        # Metadata-only damage must never be reported as a clean backup.
        for kind in ['lfn', 'directory']:
            damaged = bytearray(original)
            if kind == 'lfn':
                parts, _ = next(layout.long_names(layout.root))
                damaged[parts[0] + 13] ^= 1
            else:
                damaged, _ = entry_fixture(original, bits)
            image.write_bytes(damaged)
            warning_destination = root / f'{kind}{bits}'
            warning_destination.mkdir()
            run(image, warning_destination, 1)
            files, manifest = exports(warning_destination / 'REC00001')
            assert 'FINAL COMPLETE_WITH_FINDINGS' in manifest
            assert ('ISSUE code=LFN_CHECKSUM' if kind == 'lfn' else 'ISSUE code=DIR_DOT_METADATA') in manifest
            for line in manifest.splitlines():
                if line.startswith('ISSUE '):
                    assert bytes.fromhex(line.split('message_hex=')[1]).decode('utf-8')
            assert len(files) == 55
            assert all(not value or value == b'MagicDisk LFN preservation test.\r\n' for value in files.values())
            assert image.read_bytes() == damaged
    if len(sys.argv) > 2:
        image.write_bytes(original)
        for mode in ['gap', 'transient', 'retry-cancel', 'cancel', 'scan-cancel', 'fat-read', 'destination']:
            destination = root / mode
            destination.mkdir()
            subprocess.run([sys.argv[2], str(image), str(destination), mode, 'live'], check=True)
            directories = list(destination.iterdir())
            if mode in ['gap', 'transient', 'retry-cancel', 'cancel']:
                files, manifest = exports(directories[0])
                if mode == 'transient':
                    assert 'FINAL COMPLETE ' in manifest and 'unreadable=0' in manifest
                    assert 'attempt=2 result=failed' in manifest
                    assert 'attempt=3 result=readable' in manifest
                    assert 'UNREADABLE' not in manifest
                    assert all(not data or data == b'MagicDisk LFN preservation test.\r\n' for data in files.values())
                else:
                    assert ('FINAL COMPLETE_WITH_GAPS' if mode == 'gap' else 'FINAL INCOMPLETE') in manifest
                if mode == 'retry-cancel':
                    assert 'unreadable=0' in manifest and all(not data for data in files.values())
                if mode == 'gap':
                    assert 'UNREADABLE' in manifest
                    assert any(data and not any(data) for data in files.values())
            else:
                assert not directories
            assert image.read_bytes() == original
print('PASS: FAT12/16/32 live-file contents, UTF-8 paths, empty files, salvage prefixes and unchanged source')
