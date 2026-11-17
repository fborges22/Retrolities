"""Independent attribute damage combined with label or filename conflicts."""
from directory_fixture import Layout
from conflict_fixture import fixture as conflicts
from test_label_repair import fixture as labels


def fixture(path, bits, kind):
    if kind == 'conflicts':
        attributes_fixed, repaired, _ = conflicts(path, bits)
        layout = Layout(attributes_fixed, bits)
        parent = layout.cluster(layout.child())
        child = layout.cluster(layout.child(parent))
        entries = [list(layout.long_names(child))[30][1]]
    else:
        assert kind == 'labels'
        attributes_fixed, repaired, first, removed = labels(path, bits)
        entries = [first, *removed]
    damaged = bytearray(attributes_fixed)
    for offset in entries:
        damaged[offset + 11] |= 0xc0
    path.write_bytes(damaged)
    return bytes(damaged), attributes_fixed, repaired, entries
