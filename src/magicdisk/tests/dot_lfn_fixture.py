"""Independent layouts with a complete live LFN group in the leading dot slots."""
from pathlib import Path
import struct
from directory_fixture import create, command, Layout

NAME = 'Relatório de entrada.txt'
PAYLOAD = b'Preserve long-name records and file contents.\r\n'


def occupied_lfn_fixture(path, bits, mode='first', name=NAME, directory=False):
    path = Path(path)
    create(path, bits)
    command('mmd', '-i', str(path), '::/Empty')
    payload = path.with_name('payload.bin')
    payload.write_bytes(PAYLOAD)
    if mode == 'file':
        command('mcopy', '-i', str(path), str(payload), '::/Empty/FIRST.TXT')
    if directory:
        command('mmd', '-i', str(path), '::/Empty/' + name)
    command('mcopy', '-i', str(path), str(payload),
            '::/Empty/' + name + ('/DATA.TXT' if directory else ''))
    clean = path.read_bytes()
    layout = Layout(clean, bits)
    _, parent_entry = next((p, e) for p, e in layout.long_names(layout.root)
                           if layout.decoded_name(p) == 'Empty')
    cluster = layout.cluster(parent_entry)
    prefix = (layout.data_sector + (cluster - 2) * layout.spc) * 512
    parts, entry = next((p, e) for p, e in layout.long_names(cluster)
                        if layout.decoded_name(p) == name)
    group = bytearray(b''.join(clean[offset:offset + 32] for offset in parts + [entry]))
    if directory:
        struct.pack_into('<I', group, len(group) - 4, 123456)
    slots = layout.slots(cluster)
    count = len(group) // 32
    first = 0 if mode == 'first' else 1
    damaged = bytearray(clean)
    for offset in parts + [entry]:
        damaged[offset] = 0xe5
    if mode == 'file':
        damaged[prefix:prefix + 32] = clean[prefix + 64:prefix + 96]
        damaged[prefix + 64] = 0xe5
    for index in range(count):
        offset = slots[first + index]
        damaged[offset:offset + 32] = group[index * 32:(index + 1) * 32]
    if directory:
        child = layout.cluster(entry)
        child_prefix = (layout.data_sector + (child - 2) * layout.spc) * 512
        struct.pack_into('<H', damaged, child_prefix + 26, 0)
    expected = bytearray(damaged)
    capacity = 16 if count + 2 + (mode == 'file') <= 16 else len(slots)
    if count + 2 + (mode == 'file') <= capacity:
        destination = first + count if first + 2 * count < capacity else 2
        for index in range(count):
            offset = slots[destination + index]
            expected[offset:offset + 32] = group[index * 32:(index + 1) * 32]
        if directory:
            struct.pack_into('<I', expected, slots[destination + count - 1] + 28, 0)
            expected[child_prefix + 26:child_prefix + 28] = clean[child_prefix + 26:child_prefix + 28]
        for index in range(first, first + count):
            if not destination <= index < destination + count:
                expected[slots[index]] = 0xe5
        if mode == 'file':
            vacant = next(index for index in range(2, capacity)
                          if expected[slots[index]] in (0, 0xe5))
            offset = slots[vacant]
            expected[offset:offset + 32] = damaged[prefix:prefix + 32]
        for index, target in [(0, cluster), (1, 0)]:
            if mode == 'self' and not index:
                continue
            offset = prefix + index * 32
            expected[offset:offset + 32] = bytes(32)
            expected[offset:offset + 11] = b'.          ' if not index else b'..         '
            expected[offset + 11] = 16
            struct.pack_into('<H', expected, offset + 26, target & 0xffff)
            if bits == 32:
                struct.pack_into('<H', expected, offset + 20, target >> 16)
    return damaged, expected, prefix
