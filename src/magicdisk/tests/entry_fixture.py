"""Corrupt only directory sizes and the two existing dot-entry metadata fields."""
import struct
from directory_fixture import Layout


def entry_fixture(data, bits):
    layout = Layout(data, bits)
    parent_entry = layout.child()
    parent = layout.cluster(parent_entry)
    child_entry = layout.child(parent)
    child = layout.cluster(child_entry)
    broken = bytearray(data)
    for entry, cluster in [(parent_entry, parent), (child_entry, child)]:
        struct.pack_into('<I', broken, entry + 28, 123456)
        prefix = (layout.data_sector + (cluster - 2) * layout.spc) * 512
        for offset in [prefix, prefix + 32]:
            broken[offset + 11] = 0xc0
            struct.pack_into('<H', broken, offset + 26, 999)
            if bits == 32:
                struct.pack_into('<H', broken, offset + 20, 1)
            struct.pack_into('<I', broken, offset + 28, 987654)
    return broken, bytearray(data)


def missing_dot_fixture(data, bits):
    """Deleted dot slots plus entry metadata damage, with independent rebuilt bytes."""
    layout = Layout(data, bits)
    broken, expected = entry_fixture(data, bits)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    for cluster, owner in [(parent, 0), (child, parent)]:
        prefix = (layout.data_sector + (cluster - 2) * layout.spc) * 512
        for index, target in [(0, cluster), (1, owner)]:
            offset = prefix + index * 32
            broken[offset] = 0xe5
            expected[offset:offset + 32] = bytes(32)
            expected[offset:offset + 11] = b'.          ' if not index else b'..         '
            expected[offset + 11] = 16
            struct.pack_into('<H', expected, offset + 26, target & 0xffff)
            if bits == 32:
                struct.pack_into('<H', expected, offset + 20, target >> 16)
    return broken, expected


def reversed_dot_fixture(data, bits):
    """Swap complete dot entries while also corrupting determinable metadata."""
    layout = Layout(data, bits)
    broken, expected = entry_fixture(data, bits)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    for cluster in [parent, child]:
        prefix = (layout.data_sector + (cluster - 2) * layout.spc) * 512
        broken[prefix:prefix + 32], broken[prefix + 32:prefix + 64] = (
            broken[prefix + 32:prefix + 64], broken[prefix:prefix + 32])
    return broken, expected


def damaged_dot_pair_fixture(data, bits):
    """A duplicated self entry and a moved surviving self entry in a nested tree."""
    layout = Layout(data, bits)
    broken, expected = entry_fixture(data, bits)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    for cluster, owner in [(parent, 0), (child, parent)]:
        prefix = (layout.data_sector + (cluster - 2) * layout.spc) * 512
        broken[prefix + 32:prefix + 64] = broken[prefix:prefix + 32]
        if cluster == child:
            broken[prefix] = 0xe5
        offset = prefix + 32
        expected[offset:offset + 32] = bytes(32)
        expected[offset:offset + 11] = b'..         '
        expected[offset + 11] = 16
        struct.pack_into('<H', expected, offset + 26, owner & 0xffff)
        if bits == 32:
            struct.pack_into('<H', expected, offset + 20, owner >> 16)
    return broken, expected
