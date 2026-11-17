"""A valid LFN whose DOS alias and reserved attribute bits are both damaged."""
from directory_fixture import create
from short_name_fixture import replace_alias


def fixture(path, bits):
    layout = create(path, bits)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    parts, entry = list(layout.long_names(child))[10]
    damaged = bytearray(layout.data)
    alias = bytearray(damaged[entry:entry + 11])
    alias[2] = ord('?')
    replace_alias(damaged, parts, entry, alias)
    damaged[entry + 11] |= 0xc0
    attributes_fixed = bytearray(damaged)
    attributes_fixed[entry + 11] &= 0x3f
    repaired = bytearray(attributes_fixed)
    replace_alias(repaired, parts, entry, b'R0000001CHK')
    path.write_bytes(damaged)
    return bytes(damaged), bytes(attributes_fixed), bytes(repaired)
