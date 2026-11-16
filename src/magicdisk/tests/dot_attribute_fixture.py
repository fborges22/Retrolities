"""Extra dots whose reserved attribute bits prevent either cleanup running first."""
from extra_dot_fixture import fixture as extra_dots


def fixture(path, bits):
    attributes_fixed, repaired, removed = extra_dots(path, bits)
    damaged = bytearray(attributes_fixed)
    for offset in removed:
        damaged[offset + 11] |= 0xc0
    path.write_bytes(damaged)
    return bytes(damaged), attributes_fixed, repaired, removed
