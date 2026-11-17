#!/usr/bin/env python3
"""Directory relocation preserves entries and repairs self/child parent pointers."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import create, Layout, command, PARENT, CHILD, UNICODE_FILE
from chain_fixture import set_fat

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *map(str, args)], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


def set_start(data, bits, entry, cluster):
    struct.pack_into('<H', data, entry + 26, cluster & 0xffff)
    if bits == 32:
        struct.pack_into('<H', data, entry + 20, cluster >> 16)


with tempfile.TemporaryDirectory(prefix='magicdisk-dir-relocate-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'{bits}.img'
        layout = create(path, bits)
        original = path.read_bytes()
        parent_entry = layout.child()
        parent = layout.cluster(parent_entry)
        child_entry = layout.child(parent)
        child = layout.cluster(child_entry)
        end = {12: 0xff8, 16: 0xfff8, 32: 0x0ffffff8}[bits]
        chain = [child]
        while layout.fat(chain[-1]) < end:
            chain.append(layout.fat(chain[-1]))
        assert len(chain) > 1
        cases = [(parent_entry, [parent], 0)]
        cases += [(child_entry, chain, i) for i in sorted({0, len(chain) // 2, len(chain) - 1})]
        if bits == 32:
            root_chain = [layout.root]
            while layout.fat(root_chain[-1]) < end:
                root_chain.append(layout.fat(root_chain[-1]))
            assert len(root_chain) > 1
            cases += [(None, root_chain, i) for i in range(len(root_chain))]
        replacement = next(c for c in range(2, 1000) if layout.fat(c) == 0)
        to = (layout.data_sector + (replacement - 2) * layout.spc) * 512
        length = layout.spc * 512
        for case, (entry, clusters, index) in enumerate(cases):
            target = clusters[index]
            start = (layout.data_sector + (target - 2) * layout.spc) * 512
            expected = bytearray(original)
            expected[to:to + length] = original[start:start + length]
            set_fat(expected, bits, replacement, layout.fat(target))
            set_fat(expected, bits, target, end - 1)
            if index:
                set_fat(expected, bits, clusters[index - 1], replacement)
            elif entry is None:
                for sector in [0, 6]:
                    struct.pack_into('<I', expected, sector * 512 + 44, replacement)
            else:
                set_start(expected, bits, entry, replacement)
                set_start(expected, bits, to, replacement)
                if target == parent:
                    child_sector = (layout.data_sector + (child - 2) * layout.spc) * 512
                    set_start(expected, bits, child_sector + 32, replacement)
            if bits == 32:
                free = struct.unpack_from('<I', original, 512 + 488)[0]
                for sector in [1, 7]:
                    struct.pack_into('<II', expected, sector * 512 + 488, free - 1, 2 if entry is None else 0xffffffff)
            path.write_bytes(original)
            run(path, '--relocate-cluster', target)
            assert path.read_bytes() == original
            journal = root / f'{bits}-{case}.mdj'
            run(path, '--relocate-cluster', target, '--apply', '--journal', journal)
            assert path.read_bytes() == expected
            assert '0 issues' in run(path)
            command('fsck.fat', '-n', str(path))
            assert command('mtype', '-i', str(path), f'::/{PARENT}/{CHILD}/{UNICODE_FILE}').stdout == b'MagicDisk LFN preservation test.\r\n'
            raw = journal.read_bytes()
            version, count = struct.unpack_from('<II', raw, 8)
            assert version == (13 if entry is None else 12)
            sectors = [struct.unpack_from('<I', raw, 1088 + i * 1032)[0] for i in range(count)]
            assert sectors[:layout.spc] == list(range(to // 512, to // 512 + layout.spc))
            assert not set(sectors).intersection(range(start // 512, start // 512 + layout.spc))
            if entry is None and not index:
                for byte in [3, 6 * 512 + 3, to + 12]:
                    changed = bytearray(expected)
                    changed[byte] ^= 1
                    path.write_bytes(changed)
                    run(path, '--restore-journal', journal, '--apply', code=2)
                    assert path.read_bytes() == changed
                path.write_bytes(expected)
            run(path, '--restore-journal', journal, '--apply')
            assert path.read_bytes() == original
            partial = bytearray(original)
            for i, sector in enumerate(sectors):
                record = 1088 + i * 1032
                partial[sector * 512:(sector + 1) * 512] = raw[record + 516:record + 1028]
                path.write_bytes(partial)
                run(path, '--restore-journal', journal, '--apply')
                assert path.read_bytes() == original
        if bits == 32:
            # Whole-disk image: the MBR identity remains fixed while the volume BPB changes.
            expected = bytearray(original)
            start = (layout.data_sector + (layout.root - 2) * layout.spc) * 512
            expected[to:to + length] = original[start:start + length]
            set_fat(expected, bits, replacement, layout.fat(layout.root))
            set_fat(expected, bits, layout.root, end - 1)
            for sector in [0, 6]:
                struct.pack_into('<I', expected, sector * 512 + 44, replacement)
            free = struct.unpack_from('<I', original, 512 + 488)[0]
            for sector in [1, 7]:
                struct.pack_into('<II', expected, sector * 512 + 488, free - 1, 2)
            mbr = bytearray(63 * 512)
            mbr[450] = 0x0c
            struct.pack_into('<II', mbr, 454, 63, len(original) // 512)
            mbr[510:512] = b'\x55\xaa'
            whole = root / 'partitioned.img'
            whole.write_bytes(mbr + original)
            journal = root / 'partitioned.mdj'
            run(whole, '--partition', 1, '--relocate-cluster', layout.root,
                '--apply', '--journal', journal)
            assert whole.read_bytes() == mbr + expected
            assert '0 issues' in run(whole, '--partition', 1)
            run(whole, '--partition', 1, '--restore-journal', journal, '--apply')
            assert whole.read_bytes() == mbr + original
print('PASS: FAT12/16/32 directory head/middle/tail relocation, child parent links, LFN contents and partial undo')
