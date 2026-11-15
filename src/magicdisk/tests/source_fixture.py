"""Independent FSInfo expectation for a selected FAT's repaired allocation."""
import struct
from directory_fixture import Layout


def rebuilt_info(data, bits):
    result = bytearray(data)
    if bits != 32:
        return result
    layout = Layout(result, bits)
    clusters = (len(result) // 512 - layout.data_sector) // layout.spc
    free = [c for c in range(2, clusters + 2) if layout.fat(c) == 0]
    info, backup = struct.unpack_from('<HH', result, 48)
    rebuilt = bytearray(512)
    struct.pack_into('<I', rebuilt, 0, 0x41615252)
    struct.pack_into('<IIII', rebuilt, 484, 0x61417272, len(free),
                     free[0] if free else 0xffffffff, 0)
    struct.pack_into('<I', rebuilt, 508, 0xaa550000)
    for sector in [info, backup + info]:
        result[sector * 512:(sector + 1) * 512] = rebuilt
    return result
