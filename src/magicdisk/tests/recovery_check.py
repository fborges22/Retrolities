"""Verify exported cluster payloads independently of the recovery implementation."""
import re


def verify(directory, payloads):
    manifest = (directory / 'RECOVERY.TXT').read_text()
    assert 'FINAL COMPLETE ' in manifest and 'unreadable=0' in manifest, manifest
    assert 'cycle-or-shared-tail' in manifest and 'leaves-lost-set' in manifest
    seen = set()
    lengths = {}
    for line in manifest.splitlines():
        match = re.fullmatch(r'CLUSTER (\d+) file=(\S+) offset=(\d+) disk_lba=(\d+) sectors=(\d+)', line)
        if not match:
            continue
        cluster, name, offset, lba, sectors = match.groups()
        cluster, offset, sectors = int(cluster), int(offset), int(sectors)
        assert cluster in payloads and cluster not in seen
        seen.add(cluster)
        file = directory / name
        content = file.read_bytes()
        assert content[offset:offset + sectors * 512] == payloads[cluster]
        lengths[name] = offset + sectors * 512
    assert seen == payloads.keys(), (seen, payloads.keys())
    for name, size in lengths.items():
        assert (directory / name).stat().st_size == size
    assert len(list(directory.glob('*.CHK'))) == 9
    return manifest

