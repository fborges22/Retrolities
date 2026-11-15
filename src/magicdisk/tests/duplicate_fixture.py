"""Two distinct live files sharing one DOS alias, with valid VFAT checksums."""
from directory_fixture import Layout


def duplicate_alias(data, bits):
    layout = Layout(data, bits)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    names = list(layout.long_names(child))
    _, first = names[0]
    parts, second = names[40]
    broken = bytearray(data)
    broken[second:second + 11] = broken[first:first + 11]
    checksum = 0
    for byte in broken[second:second + 11]:
        checksum = (((checksum >> 1) | ((checksum & 1) << 7)) + byte) & 255
    for part in parts:
        broken[part + 13] = checksum
    return broken
