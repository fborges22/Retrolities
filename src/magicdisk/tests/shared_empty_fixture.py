"""Independent expected bytes for replacing fully shared directory references."""
import struct
from directory_fixture import create, Layout, command
from chain_fixture import set_fat


def shared_empty_fixture(path, bits, count=1, nested=False):
    create(path, bits)
    container = 'Container' if nested else ''
    if nested:
        command('mmd', '-i', str(path), '::/' + container)
    names = [(container + '/' if nested else '') + f'Shared alias {i}' for i in range(count)]
    for name in names:
        command('mmd', '-i', str(path), '::/' + name)
    clean = path.read_bytes()
    layout = Layout(clean, bits)
    owner_entry = layout.child()
    owner = layout.cluster(owner_entry)
    parent = 0
    if nested:
        _, parent_entry = next((parts, entry) for parts, entry in layout.long_names(layout.root)
                              if layout.decoded_name(parts) == container)
        parent = layout.cluster(parent_entry)
    aliases = [entry for parts, entry in layout.long_names(parent or layout.root)
               if layout.decoded_name(parts).startswith('Shared alias ')]
    assert len(aliases) == count
    original = bytearray(clean)
    for entry in aliases:
        # The fixture's initially empty alias chain becomes available again.
        set_fat(original, bits, layout.cluster(entry), 0)
        struct.pack_into('<H', original, entry + 26, owner & 0xffff)
        if bits == 32:
            struct.pack_into('<H', original, entry + 20, owner >> 16)
    total = (len(original) // 512 - layout.data_sector) // layout.spc
    freed = Layout(original, bits)
    available = [c for c in range(2, total + 2) if not freed.fat(c)]
    added = available[:count]
    for cluster in added:
        start = (layout.data_sector + (cluster - 2) * layout.spc) * 512
        original[start:start + layout.spc * 512] = bytes(layout.spc * 512)
        original[start:start + 512] = b'\xa5' * 512
    if bits == 32:
        info, backup = struct.unpack_from('<HH', original, 48)
        for sector in [info, backup + info]:
            struct.pack_into('<II', original, sector * 512 + 488, len(available), available[0])
    expected = bytearray(original)
    for entry, cluster in zip(aliases, added):
        set_fat(expected, bits, cluster, {12: 0xfff, 16: 0xffff, 32: 0x0fffffff}[bits])
        struct.pack_into('<H', expected, entry + 26, cluster & 0xffff)
        if bits == 32:
            struct.pack_into('<H', expected, entry + 20, cluster >> 16)
        start = (layout.data_sector + (cluster - 2) * layout.spc) * 512
        expected[start:start + layout.spc * 512] = bytes(layout.spc * 512)
        for index, target in [(0, cluster), (1, parent)]:
            offset = start + index * 32
            expected[offset:offset + 11] = b'.          ' if not index else b'..         '
            expected[offset + 11] = 16
            struct.pack_into('<H', expected, offset + 26, target & 0xffff)
            if bits == 32:
                struct.pack_into('<H', expected, offset + 20, target >> 16)
    if bits == 32:
        for sector in [info, backup + info]:
            struct.pack_into('<II', expected, sector * 512 + 488, len(available) - count, 0xffffffff)
    return original, expected, added, aliases, owner_entry
