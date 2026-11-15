"""Independent long-name, multi-cluster file for selected recovery tests."""
from pathlib import Path
from directory_fixture import command, Layout, UNICODE_FILE


def create_selected(path, bits):
    path = Path(path)
    size = {12: 1440 * 1024, 16: 32 * 1024 * 1024, 32: 64 * 1024 * 1024}[bits]
    with path.open('wb') as file:
        file.truncate(size)
    command('mkfs.fat', '-F', str(bits), str(path))
    spc = path.read_bytes()[13]
    payload = bytes((index * 37 + 11) & 255 for index in range(spc * 512 * 3 + 37))
    source = path.parent / UNICODE_FILE
    source.write_bytes(payload)
    command('mcopy', '-i', str(path), str(source), '::/')
    source = path.parent / 'OTHER.TXT'
    source.write_bytes(b'Unrelated file must not be exported.\r\n')
    command('mcopy', '-i', str(path), str(source), '::/')
    command('mmd', '-i', str(path), '::/DIRECTRY')
    data = path.read_bytes()
    layout = Layout(data, bits)
    _, entry = next((parts, entry) for parts, entry in layout.long_names(layout.root)
                    if layout.decoded_name(parts) == UNICODE_FILE)
    chain = [layout.cluster(entry)]
    for _ in range(3):
        chain.append(layout.fat(chain[-1]))
    assert layout.fat(chain[-1]) >= {12: 0xff8, 16: 0xfff8, 32: 0x0ffffff8}[bits]
    return data, layout, chain, payload, entry
