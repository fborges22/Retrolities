"""Two distinct live files sharing one DOS alias, with valid VFAT checksums."""
import struct
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


def checksum(alias):
    value = 0
    for byte in alias:
        value = ((value >> 1) | ((value & 1) << 7)) + byte
        value &= 255
    return value


def alias_name(data, entry):
    base = data[entry:entry + 8].decode('ascii').rstrip()
    extension = data[entry + 8:entry + 11].decode('ascii').rstrip()
    return base + ('.' + extension if extension else '')


def long_name(data, parts, short, name):
    fields = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
    units = list(struct.unpack('<' + 'H' * (len(name.encode('utf-16-le')) // 2), name.encode('utf-16-le')))
    count = (len(units) + 12) // 13
    assert count <= len(parts)
    if len(units) < count * 13:
        units += [0] + [0xffff] * (count * 13 - len(units) - 1)
    for part in parts[:-count]:
        data[part] = 0xe5
    for index, part in enumerate(parts[-count:]):
        ordinal = count - index
        data[part:part + 32] = bytes(32)
        data[part] = ordinal | (0x40 if index == 0 else 0)
        data[part + 11] = 15
        data[part + 13] = checksum(data[short:short + 11])
        for field, value in zip(fields, units[(ordinal - 1) * 13:ordinal * 13]):
            struct.pack_into('<H', data, part + field, value)


CASE_PAIRS = [
    ('Résumé.txt', 'RÉSUMÉ.TXT'),
    ('άλφα.txt', 'ΆΛΦΑ.TXT'),
    ('Москва.txt', 'МОСКВА.TXT'),
    ('ſample.txt', 'SAMPLE.TXT'),
    ('𐐨file.txt', '𐐀FILE.TXT'),
]


def unicode_collisions(data, bits):
    layout = Layout(data, bits)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    names = list(layout.long_names(child))
    changed = bytearray(data)
    for index, (lower, upper) in enumerate(CASE_PAIRS):
        for position, name in [(index, lower), (index + 40, upper)]:
            parts, short = names[position]
            long_name(changed, parts, short, name)
    return changed
