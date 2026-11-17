"""Shared FAT12/16/32 cross-link and lost-chain images built with mtools."""
import os
import struct
import subprocess
from chain_fixture import set_fat
from directory_fixture import Layout, command

SIZES = {12: 1440 * 1024, 16: 32 * 1024 * 1024, 32: 64 * 1024 * 1024}


def fsck(path):
    check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
    assert check.returncode == 0, check.stdout + check.stderr


def read_file(path, name):
    result = subprocess.run(['mtype', '-i', str(path), '::/' + name], capture_output=True,
                            env=dict(os.environ, LC_ALL='C.UTF-8'))
    assert result.returncode == 0, result.stderr
    return result.stdout


def build(path, bits, work):
    with path.open('wb') as file:
        file.truncate(SIZES[bits])
    command('mkfs.fat', '-F', str(bits), str(path))
    layout = Layout(path.read_bytes(), bits)
    unit = layout.spc * 512
    payload = {}
    for index, name in enumerate(['ALPHA.BIN', 'BRAVO.BIN', 'CHARLIE.BIN', 'DELTA.BIN']):
        data = bytes((index * 37 + i * 7) & 255 for i in range(3 * unit))
        data = bytes([65 + index]) * 16 + data[16:]
        (work / name).write_bytes(data)
        command('mcopy', '-i', str(path), str(work / name), '::/')
        payload[name] = data
    return payload


def start_of(layout, name):
    stem, ext = name.split('.')
    key = stem.ljust(8).encode() + ext.ljust(3).encode()
    for offset in layout.entries():
        if bytes(layout.data[offset:offset + 11]) == key:
            return offset, layout.cluster(offset)
    raise KeyError(name)


def chain(layout, cluster):
    result = []
    eoc = {12: 0xff8, 16: 0xfff8, 32: 0x0ffffff8}[layout.bits]
    while cluster < eoc:
        result.append(cluster)
        cluster = layout.fat(cluster)
    return result



def crosslinked(path, bits, work):
    """BRAVO's second link enters ALPHA's tail; CHARLIE starts inside ALPHA."""
    payload = build(path, bits, work)
    clean = path.read_bytes()
    layout = Layout(clean, bits)
    _, alpha = start_of(layout, 'ALPHA.BIN')
    _, bravo = start_of(layout, 'BRAVO.BIN')
    charlie_entry, _ = start_of(layout, 'CHARLIE.BIN')
    a_chain, b_chain = chain(layout, alpha), chain(layout, bravo)
    broken = bytearray(clean)
    set_fat(broken, bits, b_chain[0], a_chain[1])
    struct.pack_into('<H', broken, charlie_entry + 26, a_chain[0] & 0xffff)
    if bits == 32:
        struct.pack_into('<H', broken, charlie_entry + 20, a_chain[0] >> 16)
    path.write_bytes(broken)
    return payload, clean, bytes(broken), layout, a_chain, b_chain
