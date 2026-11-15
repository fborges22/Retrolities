"""Real deleted directory trees, with optional surviving directory FAT links."""
from pathlib import Path
import tempfile
from directory_fixture import create, command, Layout
from chain_fixture import set_fat

OUTER = 'Removed folder'
INNER = 'Nested removed folder'


def fixture(path, bits, retained=False):
    create(path, bits)
    command('mmd', '-i', str(path), f'::/{OUTER}')
    command('mmd', '-i', str(path), f'::/{OUTER}/{INNER}')
    names = [f'Deleted document {i:02d}.txt' for i in range(70 if retained else 2)]
    payloads = {name: (name + '\r\n').encode() * 3 for name in names}
    with tempfile.TemporaryDirectory(prefix='magicdisk-deleted-dirs-data-') as directory:
        for name, payload in payloads.items():
            source = Path(directory) / name
            source.write_bytes(payload)
            command('mcopy', '-i', str(path), str(source), f'::/{OUTER}/{INNER}/{name}')
    before = Layout(path.read_bytes(), bits)
    _, outer_entry = next((p, e) for p, e in before.long_names(before.root)
                          if before.decoded_name(p) == OUTER)
    outer = before.cluster(outer_entry)
    _, inner_entry = next((p, e) for p, e in before.long_names(outer)
                          if before.decoded_name(p) == INNER)
    inner = before.cluster(inner_entry)
    entries = {before.decoded_name(p): (p, e) for p, e in before.long_names(inner)}
    expected = {entries[name][1]: payload for name, payload in payloads.items()}
    for name in names:
        command('mdel', '-i', str(path), f'::/{OUTER}/{INNER}/{name}')
    command('mrd', '-i', str(path), f'::/{OUTER}/{INNER}')
    command('mrd', '-i', str(path), f'::/{OUTER}')
    data = bytearray(path.read_bytes())
    if retained:
        eoc = {12: 0xff8, 16: 0xfff8, 32: 0x0ffffff8}[bits]
        for start in [outer, inner]:
            cluster = start
            while cluster < eoc:
                value = before.fat(cluster)
                set_fat(data, bits, cluster, value)
                cluster = value
        # Partial deletion: one child's alias and LFN ordinals remain intact.
        parts, entry = entries[names[-1]]
        for offset in parts + [entry]:
            data[offset:offset + 32] = before.data[offset:offset + 32]
    path.write_bytes(data)
    return bytes(data), expected, outer, inner


def wide_fixture(path, count=40):
    """A queue growth case: one removed parent with many removed children."""
    create(path, 12)
    command('mmd', '-i', str(path), f'::/{OUTER}')
    names = [f'Removed child {i:02d}' for i in range(count)]
    with tempfile.TemporaryDirectory(prefix='magicdisk-deleted-wide-') as directory:
        source = Path(directory) / 'LEAF.TXT'
        for name in names:
            command('mmd', '-i', str(path), f'::/{OUTER}/{name}')
            source.write_bytes((name + '\r\n').encode())
            command('mcopy', '-i', str(path), str(source), f'::/{OUTER}/{name}/LEAF.TXT')
    before = Layout(path.read_bytes(), 12)
    _, entry = next((p, e) for p, e in before.long_names(before.root)
                    if before.decoded_name(p) == OUTER)
    outer = before.cluster(entry)
    directories = [outer]
    expected = {}
    for parts, entry in before.long_names(outer):
        name = before.decoded_name(parts)
        cluster = before.cluster(entry)
        directories.append(cluster)
        leaf = next(e for e in before.entries(cluster) if before.data[e:e + 11] == b'LEAF    TXT')
        expected[leaf] = (name + '\r\n').encode()
    for name in names:
        command('mdel', '-i', str(path), f'::/{OUTER}/{name}/LEAF.TXT')
        command('mrd', '-i', str(path), f'::/{OUTER}/{name}')
    command('mrd', '-i', str(path), f'::/{OUTER}')
    data = bytearray(path.read_bytes())
    for cluster in directories:
        while cluster < 0xff8:
            value = before.fat(cluster)
            set_fat(data, 12, cluster, value)
            cluster = value
    path.write_bytes(data)
    return bytes(data), expected
