"""Redundant dot records in roots and nested directories, with intact LFNs."""
from directory_fixture import create
import struct


def fixture(path, bits):
    layout = create(path, bits)
    data = bytearray(layout.data)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    removed = []
    for cluster, up in [(layout.root, 0), (parent, 0), (child, parent)]:
        slots = layout.slots(cluster)
        index = next(i for i, p in enumerate(slots) if not data[p])
        assert index + 2 < len(slots)
        for i, name, target in [(0, b'.          ', cluster), (1, b'..         ', up)]:
            offset = slots[index + i]
            entry = bytearray(32)
            entry[:11] = name
            entry[11] = 16
            struct.pack_into('<H', entry, 26, target & 0xffff)
            if bits == 32:
                struct.pack_into('<H', entry, 20, target >> 16)
            data[offset:offset + 32] = entry
            removed.append(offset)
        data[slots[index + 2]] = 0
    expected = bytearray(data)
    for offset in removed:
        expected[offset] = 0xe5
    path.write_bytes(data)
    return bytes(data), bytes(expected), removed
