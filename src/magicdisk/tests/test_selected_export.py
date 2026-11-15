#!/usr/bin/env python3
"""Select by any cluster, preserve LFN/data, refuse absent owners without output."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from chain_fixture import set_fat
from directory_fixture import UNICODE_FILE
from selected_export_fixture import create_selected

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-selected-export-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'FAT{bits}.IMG'
        original, layout, chain, payload, entry = create_selected(path, bits)
        def run(cluster, data=original, code=0):
            path.write_bytes(data)
            destination = root / f'out-{bits}-{len(list(root.glob("out-*")))}'
            destination.mkdir()
            result = subprocess.run([str(exe), str(path), '--export-cluster', str(cluster), str(destination)],
                                    capture_output=True, text=True)
            assert result.returncode == code, result.stdout + result.stderr
            assert path.read_bytes() == data
            return destination
        for cluster in chain:
            directory = run(cluster) / 'REC00001'
            manifest = (directory / 'RECOVERY.TXT').read_text()
            assert manifest.count('LIVE_BEGIN ') == 1
            assert 'path_hex=' + UNICODE_FILE.encode().hex().upper() in manifest
            assert f'SELECTION cluster={cluster} ' in manifest
            files = list(directory.glob('*.CHK'))
            assert len(files) == 1 and files[0].read_bytes() == payload
        for cluster in [0, 1, -1, 4294967296, 1000, layout.cluster(layout.child())]:
            assert not list(run(cluster, code=2).iterdir())
        damaged = bytearray(original)
        set_fat(damaged, bits, chain[1], {12: 0xfff, 16: 0xffff, 32: 0x0fffffff}[bits])
        directory = run(chain[1], damaged, 1) / 'REC00001'
        assert list(directory.glob('*.CHK'))[0].read_bytes() == payload[:2 * layout.spc * 512]
        assert 'mode=allocated-prefix' in (directory / 'RECOVERY.TXT').read_text()
        assert not list(run(chain[-1], damaged, 2).iterdir())
        different = bytearray(original)
        different[(layout.reserved + layout.fatsz) * 512 + 20] ^= 1
        assert not list(run(chain[0], different, 2).iterdir())
        if len(sys.argv) > 2:
            path.write_bytes(original)
            for mode in ['gap', 'transient', 'retry-cancel', 'cancel', 'scan-cancel',
                         'selection-cancel', 'fat-read', 'destination']:
                destination = root / f'fault-{bits}-{mode}'
                destination.mkdir()
                subprocess.run([sys.argv[2], str(path), str(destination), mode,
                                'selected', str(chain[0])], check=True, capture_output=True)
                assert path.read_bytes() == original
                directories = list(destination.iterdir())
                if mode in ['gap', 'transient', 'retry-cancel', 'cancel']:
                    manifest = (directories[0] / 'RECOVERY.TXT').read_text()
                    assert manifest.count('LIVE_BEGIN ') == 1
                    assert ('FINAL COMPLETE_WITH_GAPS' if mode == 'gap' else
                            'FINAL COMPLETE ' if mode == 'transient' else 'FINAL INCOMPLETE') in manifest
                else:
                    assert not directories
        print(f'PASS: FAT{bits} selected file heads/tails, UTF-8 names, salvage prefixes, absent owners and mirror refusal')
