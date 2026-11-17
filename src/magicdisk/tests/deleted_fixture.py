"""Deleted records created with mtools, plus independent stale/allocation cases."""
from pathlib import Path
import struct
import tempfile
from directory_fixture import create, command, Layout, PARENT, CHILD
from chain_fixture import set_fat

DELETED_NAME = 'Deleted relatório with a long name.bin'


def fixture(path, bits):
    layout = create(path, bits)
    length = layout.spc * 512
    name = DELETED_NAME
    payload = bytes((i * 17 + 3) % 251 for i in range(length * 2 + 73))
    with tempfile.TemporaryDirectory(prefix='magicdisk-deleted-data-') as directory:
        source = Path(directory) / name
        source.write_bytes(payload)
        command('mcopy', '-i', str(path), str(source), f'::/{PARENT}/{CHILD}/{name}')
        source.write_bytes(b'')
        for index in range(10):
            command('mcopy', '-i', str(path), str(source), f'::/CASE{index}.BIN')
    layout = Layout(path.read_bytes(), bits)
    child = layout.cluster(layout.child(layout.cluster(layout.child())))
    parts, removed = next((parts, entry) for parts, entry in layout.long_names(child)
                          if layout.decoded_name(parts) == name)
    first = layout.cluster(removed)
    assert layout.fat(first) == first + 1 and layout.fat(first + 1) == first + 2
    command('mdel', '-i', str(path), f'::/{PARENT}/{CHILD}/{name}')
    data = bytearray(path.read_bytes())
    expected = {removed: (payload, 'declared-length')}
    evidence = {offset: bytes(data[offset:offset + 32]) for offset in parts}
    slots = [next(e for e in layout.entries() if layout.data[e:e + 11] == f'CASE{i}   BIN'.encode())
             for i in range(10)]
    live_entry = next(e for e in layout.entries() if layout.data[e + 11] == 32 and layout.cluster(e))
    live_cluster = layout.cluster(live_entry)
    eoc = {12: 0xfff, 16: 0xffff, 32: 0x0fffffff}[bits]
    for cluster in range(1000, 1020):
        assert not layout.fat(cluster)
        start = (layout.data_sector + (cluster - 2) * layout.spc) * 512
        data[start:start + length] = bytes([cluster % 251]) * length
    for cluster, value in [(1010, 1012), (1012, eoc), (1006, eoc),
                           (1015, 1016), (1016, 1015), (1018, eoc)]:
        set_fat(data, bits, cluster, value)
    cases = [(1000, length * 2 + 17, bytes([1000 % 251]) * length +
              bytes([1001 % 251]) * length + bytes([1002 % 251]) * 17, 'declared-length'),
             (1010, length + 37, bytes([1010 % 251]) * length + bytes([1012 % 251]) * 37,
              'declared-length'),
             (live_cluster, 34, b'', 'live-owned-cluster'),
             (1005, length * 2, bytes([1005 % 251]) * length, 'allocation-conflict'),
             (1015, length * 3, bytes([1015 % 251]) * length + bytes([1016 % 251]) * length,
              'chain-loop'),
             (1018, length * 2, bytes([1018 % 251]) * length, 'early-chain-end'),
             (0, 0, b'', 'declared-length'), (0, 100, b'', 'invalid-cluster')]
    for offset, (start, size, output, stop) in zip(slots, cases):
        data[offset] = 0xe5
        struct.pack_into('<H', data, offset + 26, start & 0xffff)
        if bits == 32:
            struct.pack_into('<H', data, offset + 20, start >> 16)
        struct.pack_into('<I', data, offset + 28, size)
        expected[offset] = (output, stop)
    # Deleted directory and volume-label evidence must never become payload files.
    for offset, attr in zip(slots[8:], [16, 8]):
        data[offset] = 0xe5
        data[offset + 11] = attr
        # Keep invalid-directory metadata separate from the payload retry sector.
        struct.pack_into('<H', data, offset + 26, 1008)
        struct.pack_into('<I', data, offset + 28, length)
        evidence[offset] = bytes(data[offset:offset + 32])
    path.write_bytes(data)
    return bytes(data), expected, evidence
