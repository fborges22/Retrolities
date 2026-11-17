"""Independent occupied-dot-slot fixtures and expected directory bytes."""
import struct
from directory_fixture import Layout


def occupied_dot_fixture(data, bits, deleted=False, count=2):
    layout = Layout(data, bits)
    parent = layout.cluster(layout.child())
    child_entry = layout.child(parent)
    child = layout.cluster(child_entry)
    prefix = (layout.data_sector + (parent - 2) * layout.spc) * 512
    damaged = bytearray(data)
    for index, (parts, entry) in enumerate(list(layout.long_names(child))[:count]):
        record = bytearray(data[entry:entry + 32])
        record[:11] = [b'ALPHA   BIN', b'BETA    BIN'][index]
        damaged[prefix + index * 32:prefix + (index + 1) * 32] = record
        for offset in parts + [entry]:
            damaged[offset] = 0xe5
    vacant = next(i for i in range(2, 16) if not damaged[prefix + i * 32])
    if deleted:
        damaged[prefix + vacant * 32] = 0xe5
        damaged[prefix + (vacant + 1) * 32] = 0xe5
        entry = prefix + (vacant + 2) * 32
        damaged[entry:entry + 11] = b'KEEP    TXT'
        damaged[entry + 11] = 32
    struct.pack_into('<I', damaged, child_entry + 28, 123456)
    expected = bytearray(damaged)
    expected[prefix + vacant * 32:prefix + (vacant + count) * 32] = damaged[prefix:prefix + count * 32]
    expected[prefix:prefix + count * 32] = bytes(count * 32)
    for index, cluster in [(0, parent), (1, 0)]:
        offset = prefix + index * 32
        expected[offset:offset + 11] = [b'.          ', b'..         '][index]
        expected[offset + 11] = 16
        struct.pack_into('<H', expected, offset + 26, cluster & 0xffff)
        if bits == 32:
            struct.pack_into('<H', expected, offset + 20, cluster >> 16)
    struct.pack_into('<I', expected, child_entry + 28, 0)
    return damaged, expected, prefix, vacant

