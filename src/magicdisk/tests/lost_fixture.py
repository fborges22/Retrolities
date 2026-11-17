"""Build orphan topology without changing the independent directory fixture."""
import struct
from directory_fixture import Layout


def lost_fixture(data, bits):
    image = bytearray(data)
    layout = Layout(image, bits)
    end = {12: 0xfff, 16: 0xffff, 32: 0x0fffffff}[bits]
    bad = end - 8
    links = {1000: 1001, 1001: 1002, 1002: end, 1003: 1004, 1004: 1003,
             1005: 1006, 1006: end, 1007: 1006, 1008: 1008, 1009: 2,
             1010: 1, 1011: bad, 1013: end, 1014: 1012}
    payloads = {}
    for cluster, value in links.items():
        assert layout.fat(cluster) == 0
        for copy in range(image[16]):
            base = (layout.reserved + copy * layout.fatsz) * 512
            if bits == 12:
                offset = base + cluster * 3 // 2
                pair = struct.unpack_from('<H', image, offset)[0]
                value12 = (pair & 15) | (value << 4) if cluster & 1 else (pair & 0xf000) | value
                struct.pack_into('<H', image, offset, value12)
            else:
                struct.pack_into('<H' if bits == 16 else '<I', image, base + cluster * (bits // 8), value)
        start = (layout.data_sector + (cluster - 2) * layout.spc) * 512
        content = b''.join(struct.pack('<II', cluster, sector) * 64 for sector in range(layout.spc))
        image[start:start + len(content)] = content
        if value != bad:
            payloads[cluster] = content
    return image, payloads
