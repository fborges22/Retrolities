"""Volume labels separated by a sector boundary on clean FAT volumes."""
from pathlib import Path
from directory_fixture import command, Layout


def create_labels(path, bits, duplicate=True, same=False):
    path = Path(path)
    size = {12: 1440 * 1024, 16: 32 * 1024 * 1024, 32: 128 * 1024 * 1024}[bits]
    with path.open('wb') as file:
        file.truncate(size)
    command('mkfs.fat', '-F', str(bits), '-s', '2', '-n', 'FIRST', str(path))
    data = bytearray(path.read_bytes())
    layout = Layout(data, bits)
    first = (layout.data_sector + (layout.root - 2) * layout.spc) * 512 if bits == 32 else layout.root_sector * 512
    assert data[first:first + 11] == b'FIRST      ' and data[first + 11] == 8
    for offset in range(first + 32, first + 512, 32):
        data[offset] = 0xe5
    second = first + 512
    if duplicate:
        data[second:second + 32] = data[first:first + 32]
        data[second:second + 11] = b'FIRST      ' if same else b'SECOND     '
    return data, first, second
