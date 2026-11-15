"""Independent short-alias corruption with matching, valid VFAT checksums."""
from directory_fixture import Layout
from duplicate_fixture import checksum


def entries(data, bits):
    layout = Layout(data, bits)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    return list(layout.long_names(child))


def replace_alias(data, parts, short, alias):
    assert len(alias) == 11
    data[short:short + 11] = alias
    for part in parts:
        data[part + 13] = checksum(alias)


def invalid_aliases(original, bits):
    data = bytearray(original)
    cases = [(2, value) for value in range(32)]
    cases += [(9, value) for value in b'"*+,./:;<=>?[\\]|']
    cases += [(0, ord(' ')), (0, ord('.'))]
    expected = []
    names = entries(original, bits)
    assert len(names) >= len(cases)
    for (parts, short), (byte, value) in zip(names, cases):
        alias = bytearray(data[short:short + 11])
        alias[byte] = value
        replace_alias(data, parts, short, alias)
        expected.append((short + byte, value))
    return data, expected


def valid_aliases(original, bits):
    data = bytearray(original)
    aliases = [b'NORMAL  TXT', b'lower   txt', b'\x05ABC    TXT',
               b'\x81\\ABC   TXT', b'ACCENT\x82 TXT', b'$%\'-_@~ TXT']
    for (parts, short), alias in zip(entries(original, bits), aliases):
        replace_alias(data, parts, short, alias)
    return data
