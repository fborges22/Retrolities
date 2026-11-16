"""Independent file pairs with both alias and LFN collisions."""
from directory_fixture import create
from duplicate_fixture import long_name


def fixture(path, bits, count=1):
    assert 1 <= count <= 20
    layout = create(path, bits)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    names = list(layout.long_names(child))
    data = bytearray(layout.data)
    for index in range(count):
        first_parts, first = names[index]
        parts, second = names[30 + index]
        data[second:second + 11] = data[first:first + 11]
        long_name(data, parts, second, layout.decoded_name(first_parts))
    expected = bytearray(data)
    for index in range(count):
        parts, second = names[30 + index]
        expected[second:second + 11] = f'R{index + 1:07d}CHK'.encode()
        long_name(expected, parts, second, f'R{index + 1:07d}.CHK')
    path.write_bytes(data)
    return bytes(data), bytes(expected), count
