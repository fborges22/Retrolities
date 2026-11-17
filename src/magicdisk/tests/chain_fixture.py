"""Independent corruptions and expected repairs of files with intact VFAT names."""
import struct
from directory_fixture import Layout


def set_fat(data, bits, cluster, value):
    layout = Layout(data, bits)
    for copy in range(data[16]):
        base = (layout.reserved + copy * layout.fatsz) * 512
        if bits == 12:
            offset = base + cluster * 3 // 2
            old = struct.unpack_from('<H', data, offset)[0]
            pair = (old & 15) | (value << 4) if cluster & 1 else (old & 0xf000) | value
            struct.pack_into('<H', data, offset, pair)
        elif bits == 16:
            struct.pack_into('<H', data, base + cluster * 2, value)
        else:
            offset = base + cluster * 4
            old = struct.unpack_from('<I', data, offset)[0]
            struct.pack_into('<I', data, offset, (old & 0xf0000000) | value)


def chain_fixture(data, bits):
    layout = Layout(data, bits)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    entries = [pos for pos in layout.entries(child) if data[pos + 11] == 32][:9]
    assert len(entries) == 9
    clusters = [layout.cluster(pos) for pos in entries]
    end = {12: 0xfff, 16: 0xffff, 32: 0x0fffffff}[bits]
    assert all(layout.fat(c) >= end - 7 for c in clusters)
    assert layout.fat(1000) == 0
    broken = bytearray(data)
    size = layout.spc * 512
    struct.pack_into('<I', broken, entries[0] + 28, size + 1)
    struct.pack_into('<I', broken, entries[1] + 28, 0)
    set_fat(broken, bits, clusters[2], clusters[2])
    set_fat(broken, bits, clusters[3], 1000)
    set_fat(broken, bits, clusters[4], 1)
    set_fat(broken, bits, clusters[5], clusters[6])
    set_fat(broken, bits, clusters[7], end - 8)
    set_fat(broken, bits, clusters[8], 0)
    if bits == 32:
        for sector in [1, 7]:
            free = struct.unpack_from('<I', broken, sector * 512 + 488)[0]
            if free != 0xffffffff:
                struct.pack_into('<I', broken, sector * 512 + 488, free + 1)
    expected = bytearray(broken)
    for index in [2, 3, 4]:
        set_fat(expected, bits, clusters[index], end)
    for index, length in [(0, size), (1, size), (5, size * 2), (6, 0), (7, 0), (8, 0)]:
        struct.pack_into('<I', expected, entries[index] + 28, length)
    for index in [6, 7, 8]:
        struct.pack_into('<H', expected, entries[index] + 26, 0)
        if bits == 32:
            struct.pack_into('<H', expected, entries[index] + 20, 0)
    return broken, expected, entries
