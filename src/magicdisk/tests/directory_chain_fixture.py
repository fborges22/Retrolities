"""Directory loops and invalid endings without altering any directory contents."""
from directory_fixture import Layout
from chain_fixture import set_fat


def directory_chain_fixture(data, bits):
    layout = Layout(data, bits)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    starts = [parent, child] + ([layout.root] if bits == 32 else [])
    ends = []
    end = {12: 0xff8, 16: 0xfff8, 32: 0x0ffffff8}[bits]
    for start in starts:
        current = start
        while layout.fat(current) < end:
            current = layout.fat(current)
        ends.append(current)
    broken = bytearray(data)
    set_fat(broken, bits, ends[0], parent)
    set_fat(broken, bits, ends[1], 1)
    if bits == 32:
        assert layout.fat(1000) == 0
        set_fat(broken, bits, ends[2], 1000)
    expected = bytearray(broken)
    for last in ends:
        set_fat(expected, bits, last, end + 7)
    return broken, expected, starts, ends
