"""Reserved bits, misplaced labels and alias/LFN conflicts in one directory tree."""
from directory_fixture import Layout
from duplicate_fixture import long_name
from test_label_repair import fixture as labels


def fixture(path, bits):
    labeled, cleaned, first_label, removed = labels(path, bits)
    layout = Layout(labeled, bits)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    names = list(layout.long_names(child))
    first_parts, first = names[0]
    parts, second = names[30]
    stages = []
    for source in [labeled, cleaned]:
        data = bytearray(source)
        data[second:second + 11] = data[first:first + 11]
        long_name(data, parts, second, layout.decoded_name(first_parts))
        stages.append(bytes(data))
    attributes_fixed, labels_fixed = stages
    damaged = bytearray(attributes_fixed)
    for offset in [first_label, *removed, second]:
        damaged[offset + 11] |= 0xc0
    repaired = bytearray(labels_fixed)
    repaired[second:second + 11] = b'R0000001CHK'
    long_name(repaired, parts, second, 'R0000001.CHK')
    path.write_bytes(damaged)
    return bytes(damaged), attributes_fixed, labels_fixed, bytes(repaired)
