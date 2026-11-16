"""A malformed root label and independent expected root/boot label changes."""
from label_fixture import create_labels


def fixture(path, bits):
    data, first, _ = create_labels(path, bits, duplicate=False)
    data[first:first + 11] = b' A?B\tC:D E '
    expected = bytearray(data)
    name = b'_A_B_C_D E '
    expected[first:first + 11] = name
    field = 71 if bits == 32 else 43
    expected[field:field + 11] = name
    if bits == 32:
        expected[6 * 512 + field:6 * 512 + field + 11] = name
    path.write_bytes(data)
    return bytes(data), bytes(expected), first
