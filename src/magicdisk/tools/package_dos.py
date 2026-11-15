#!/usr/bin/env python3
"""Build a DOS-friendly ZIP deterministically from the supplied build inputs."""
import argparse
import hashlib
import os
from pathlib import Path
import tempfile
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--watcom', type=Path, default=Path('/opt/watcom'))
args = parser.parse_args()
source = Path(__file__).resolve().parents[1]


def dos_text(path):
    return path.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')


payload = {
    'MAGICDSK.EXE': args.executable.read_bytes(),
    'README.TXT': dos_text(source / 'README.TXT'),
    'LICENSE.TXT': dos_text(source.parents[1] / 'LICENSE'),
    'UNICODE.TXT': dos_text(source / 'UNICODE-LICENSE.txt'),
    'DOS32A.TXT': dos_text(args.watcom / 'binw' / 'license.d32'),
    'WATCOM.TXT': dos_text(args.watcom / 'license.txt'),
}
if not payload['MAGICDSK.EXE'].startswith(b'MZ'):
    raise SystemExit('Expected the DOS executable, not a native host build')
guide = payload['README.TXT'].decode('ascii')
if any(len(line) > 79 for line in guide.splitlines()):
    raise SystemExit('DOS guide lines must fit an 80-column display')
payload['SHA256.TXT'] = ''.join(
    hashlib.sha256(data).hexdigest() + '  ' + name + '\r\n'
    for name, data in sorted(payload.items())).encode('ascii')

args.output.parent.mkdir(parents=True, exist_ok=True)
with tempfile.NamedTemporaryFile(dir=args.output.parent, delete=False) as file:
    temporary = Path(file.name)
try:
    with zipfile.ZipFile(temporary, 'w', allowZip64=False) as archive:
        for name, data in sorted(payload.items()):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 0
            info.external_attr = 0x20
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data, compresslevel=9)
    os.replace(temporary, args.output)
finally:
    temporary.unlink(missing_ok=True)
print(f'Created {args.output}: {len(payload)} files, {args.output.stat().st_size} bytes')
