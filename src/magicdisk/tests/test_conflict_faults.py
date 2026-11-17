from pathlib import Path
import subprocess
import sys
import tempfile
from directory_fixture import create
from duplicate_fixture import long_name

with tempfile.TemporaryDirectory(prefix='magicdisk-conflict-faults-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / 'SOURCE.IMG'
        layout = create(path, bits)
        parent = layout.cluster(layout.child())
        child = layout.cluster(layout.child(parent))
        names = list(layout.long_names(child))
        data = bytearray(layout.data)
        for i in range(20):
            fp, first = names[i]
            parts, second = names[i + 30]
            data[second:second + 11] = data[first:first + 11]
            long_name(data, parts, second, layout.decoded_name(fp))
        path.write_bytes(data)
        subprocess.run([str(Path(sys.argv[1]).resolve()), str(path), str(root / 'FAULT.MDJ')], check=True)
        assert path.read_bytes() == data
        print(f'PASS FAT{bits}: conflict repair faults and rollback', flush=True)
