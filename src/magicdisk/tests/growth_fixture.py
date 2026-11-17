"""A full directory needs additional allocated space to rebuild its dots."""
import struct
from directory_fixture import Layout
from dot_lfn_fixture import occupied_lfn_fixture
from chain_fixture import set_fat

NAME = 'Diretório ' + 'x' * 210


def growth_fixture(path, bits, child=True, referenced=False, name=NAME):
    original, repaired, prefix = occupied_lfn_fixture(path, bits, 'file', name, directory=child)
    layout = Layout(original, bits)
    parent = (prefix // 512 - layout.data_sector) // layout.spc + 2
    slots = layout.slots(parent)
    count = (len(name) + 12) // 13 + 1
    group = [bytes(original[slots[i]:slots[i] + 32]) for i in range(1, count + 1)]
    short = bytes(original[prefix:prefix + 32])
    for index in range(count + 1, len(slots)):
        offset = slots[index]
        original[offset:offset + 32] = bytes(32)
        original[offset:offset + 11] = f'E{index:07d}TXT'.encode()
        original[offset + 11] = 32
    needed = (count + 1 + layout.spc * 16 - 1) // (layout.spc * 16)
    total = (len(original) // 512 - layout.data_sector) // layout.spc
    available = [c for c in range(2, total + 2) if not layout.fat(c)]
    blocked_entry = None
    if referenced:
        blocked = available.pop(0)
        blocked_entry = next(offset for offset in layout.slots() if not original[offset])
        original[blocked_entry:blocked_entry + 32] = bytes(32)
        original[blocked_entry:blocked_entry + 11] = b'FREEPTR TXT'
        original[blocked_entry + 11] = 32
        struct.pack_into('<H', original, blocked_entry + 26, blocked & 0xffff)
        if bits == 32:
            struct.pack_into('<H', original, blocked_entry + 20, blocked >> 16)
        start = (layout.data_sector + (blocked - 2) * layout.spc) * 512
        original[start:start + layout.spc * 512] = b'\x5a' * (layout.spc * 512)
    added = available[:needed]
    assert len(added) == needed
    new_slots = []
    for cluster in added:
        start = (layout.data_sector + (cluster - 2) * layout.spc) * 512
        original[start:start + layout.spc * 512] = bytes(layout.spc * 512)
        original[start:start + 512] = b'\xa5' * 512
        new_slots.extend(range(start, start + layout.spc * 512, 32))
    expected = bytearray(original)
    if blocked_entry is not None:
        struct.pack_into('<H', expected, blocked_entry + 26, 0)
        if bits == 32:
            struct.pack_into('<H', expected, blocked_entry + 20, 0)
    for offset in new_slots:
        expected[offset:offset + 32] = bytes(32)
    for index, record in enumerate(group):
        offset = new_slots[index]
        expected[offset:offset + 32] = record
    for index in range(1, count + 1):
        expected[slots[index]] = 0xe5
    expected[prefix:prefix + 64] = repaired[prefix:prefix + 64]
    expected[slots[2]:slots[2] + 32] = short
    if child:
        child_cluster = layout.cluster(slots[count])
        child_prefix = (layout.data_sector + (child_cluster - 2) * layout.spc) * 512
        expected[child_prefix + 26:child_prefix + 28] = repaired[child_prefix + 26:child_prefix + 28]
        struct.pack_into('<I', expected, new_slots[count - 1] + 28, 0)
    last = (slots[-1] // 512 - layout.data_sector) // layout.spc + 2
    end = {12: 0xfff, 16: 0xffff, 32: 0x0fffffff}[bits]
    for previous, following in zip([last] + added, added + [end]):
        set_fat(expected, bits, previous, following)
    if bits == 32:
        updated = Layout(expected, bits)
        free = sum(not updated.fat(c) for c in range(2, total + 2))
        info, backup = struct.unpack_from('<HH', original, 48)
        for sector in [info, backup + info]:
            struct.pack_into('<II', expected, sector * 512 + 488, free, 0xffffffff)
    return original, expected, added
