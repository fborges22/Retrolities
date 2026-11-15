#!/usr/bin/env python3
"""Independent file-cluster relocation bytes, payload preservation and exact undo."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import create, Layout, command
from chain_fixture import set_fat

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *map(str, args)], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-relocate-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'{bits}.img'
        layout = create(path, bits)
        payload = bytes(range(256)) * (layout.spc * 6 - 1)
        source = root / 'Relocation payload with long name.bin'
        source.write_bytes(payload)
        command('mcopy', '-i', str(path), str(source), '::/')
        original = bytearray(path.read_bytes())
        layout = Layout(original, bits)
        entry = next(short for parts, short in layout.long_names(layout.root)
                     if layout.decoded_name(parts) == source.name)
        chain = [layout.cluster(entry)]
        end = {12: 0xff8, 16: 0xfff8, 32: 0x0ffffff8}[bits]
        while layout.fat(chain[-1]) < end:
            chain.append(layout.fat(chain[-1]))
        assert len(chain) == 3
        replacement = next(c for c in range(2, 1000) if layout.fat(c) == 0)
        destination = (layout.data_sector + (replacement - 2) * layout.spc) * 512
        length = layout.spc * 512
        original[destination:destination + length] = b'\xa5' * length
        for index, cluster in enumerate(chain):
            path.write_bytes(original)
            expected = bytearray(original)
            start = (layout.data_sector + (cluster - 2) * layout.spc) * 512
            expected[destination:destination + length] = original[start:start + length]
            set_fat(expected, bits, replacement, layout.fat(cluster))
            set_fat(expected, bits, cluster, end - 1)
            if index:
                set_fat(expected, bits, chain[index - 1], replacement)
            else:
                struct.pack_into('<H', expected, entry + 26, replacement & 0xffff)
                if bits == 32:
                    struct.pack_into('<H', expected, entry + 20, replacement >> 16)
            if bits == 32:
                for sector in [1, 7]:
                    old = struct.unpack_from('<I', original, 512 + 488)[0]
                    struct.pack_into('<II', expected, sector * 512 + 488, old - 1, 0xffffffff)
            preview = run(path, '--relocate-cluster', cluster)
            assert 'Preview only' in preview and path.read_bytes() == original
            journal = root / f'{bits}-{index}.mdj'
            run(path, '--relocate-cluster', cluster, '--apply', '--journal', journal)
            actual = path.read_bytes()
            assert actual == expected, (bits, index, [(i, a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b][:20])
            assert command('mtype', '-i', str(path), '::/' + source.name).stdout == payload
            assert '0 issues' in run(path)
            command('fsck.fat', '-n', str(path))
            raw = journal.read_bytes()
            assert struct.unpack_from('<I', raw, 8)[0] == 11
            count = struct.unpack_from('<I', raw, 12)[0]
            sectors = [struct.unpack_from('<I', raw, 1088 + i * 1032)[0] for i in range(count)]
            assert sectors[:layout.spc] == list(range(destination // 512, destination // 512 + layout.spc))
            assert not set(sectors).intersection(range(start // 512, start // 512 + layout.spc))
            changed = bytearray(expected)
            changed[destination] ^= 1
            path.write_bytes(changed)
            run(path, '--restore-journal', journal, '--apply', code=2)
            assert path.read_bytes() == changed
            path.write_bytes(expected)
            run(path, '--restore-journal', journal, '--apply')
            assert path.read_bytes() == original
            # Every prefix models a stopped transaction, including before allocation changes.
            partial = bytearray(original)
            for i, sector in enumerate(sectors):
                record = 1088 + i * 1032
                partial[sector * 512:(sector + 1) * 512] = raw[record + 516:record + 1028]
                path.write_bytes(partial)
                run(path, '--restore-journal', journal, '--apply')
                assert path.read_bytes() == original
        for invalid in [0, 1, replacement, layout.cluster(layout.child())]:
            path.write_bytes(original)
            unused = root / 'unused.mdj'
            run(path, '--relocate-cluster', invalid, '--apply', '--journal', unused, code=2)
            assert path.read_bytes() == original and not unused.exists()
print('PASS: FAT12/16/32 first/middle/last file cluster relocation, LFN payload, exact patches and partial undo')
