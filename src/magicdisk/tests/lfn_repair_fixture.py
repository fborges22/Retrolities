"""Malformed LFN groups with independently specified deletion bytes."""
from directory_fixture import create
from duplicate_fixture import long_name


def fixture(path, bits):
    layout = create(path, bits)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    groups = list(layout.long_names(child))
    broken = bytearray(layout.data)
    removed = []
    for index, (field, value) in enumerate([(13, 0), (12, 1), (26, 1), (0, 0x60), (11, 0x4f)]):
        parts, entry = groups[index]
        broken[parts[0] + field] = value
        removed.extend(parts)
    # Invalid UTF-16 and padding, with an otherwise correct sequence/checksum.
    parts, entry = groups[5]
    broken[parts[-1] + 1:parts[-1] + 3] = b'\x00\xd8'
    removed.extend(parts)
    # An orphan prefix followed by a valid new last-ordinal must preserve that suffix.
    parts, entry = groups[6]
    assert len(parts) >= 2
    long_name(broken, parts[1:], entry, 'KEPT.TXT')
    removed.append(parts[0])
    # An invalid group spanning a sector boundary remains a single cleanup.
    parts, entry = next((p, e) for p, e in groups[8:] if p[0] // 512 != p[-1] // 512)
    broken[parts[0] + 12] = 1
    removed.extend(parts)
    # Root group: maximal-length VFAT name, retained alias and file bytes.
    parts, entry = next((p, e) for p, e in layout.long_names(layout.root)
                        if len(layout.decoded_name(p)) == 255)
    broken[parts[0] + 13] ^= 1
    removed.extend(parts)
    expected = bytearray(broken)
    for offset in removed:
        expected[offset] = 0xe5
    path.write_bytes(broken)
    return bytes(broken), bytes(expected), removed
