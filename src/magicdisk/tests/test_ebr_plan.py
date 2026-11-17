"""Independent expected EBR bytes, chain preservation, refusal and source immutability."""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys
import tempfile
from directory_fixture import create

probe, exe = [Path(x).resolve() for x in sys.argv[1:3]]

def chs(lba):
    cylinder, rem = divmod(lba, 16 * 63)
    head, sector = divmod(rem, 63)
    return bytes([head, sector + 1 | ((cylinder >> 2) & 192), cylinder & 255])

def entry(kind, absolute, relative, count):
    return bytes([0]) + chs(absolute) + bytes([kind]) + chs(absolute + count - 1) + struct.pack('<II', relative, count)

with tempfile.TemporaryDirectory(prefix='magicdisk-ebr-plan-') as temporary:
    root = Path(temporary)
    for bits in [12, 16, 32]:
        volume, disk, output = [root / n for n in ['VOL.IMG', 'DISK.IMG', 'EBR.BIN']]
        create(volume, bits)
        data = volume.read_bytes()
        sectors = len(data) // 512
        records = [63, 126 + sectors + 64]
        starts = [x + 63 for x in records]
        total = starts[1] + sectors + 1
        raw = bytearray(total * 512)
        raw[446:462] = entry(15, 63, 63, total - 63)
        raw[510:512] = b'\x55\xaa'
        kind = {12: 1, 16: 6, 32: 11}[bits]
        for index, (record, start) in enumerate(zip(records, starts)):
            raw[start * 512:(start + sectors) * 512] = data
            raw[record * 512:record * 512 + 446] = bytes((i * 13 + index) % 256 for i in range(446))
            raw[record * 512 + 446:record * 512 + 462] = entry(kind, start, 63, sectors)
            raw[record * 512 + 510:record * 512 + 512] = b'\x55\xaa'
        raw[63 * 512 + 462:63 * 512 + 478] = entry(15, records[1], records[1] - 63, total - records[1])

        def run(target, candidate, source=0, status=0):
            output.unlink(missing_ok=True)
            before = hashlib.sha256(disk.read_bytes()).digest()
            result = subprocess.run([str(probe), str(disk), str(candidate), str(target), str(source), '16', '63', str(output)], capture_output=True, text=True)
            assert result.returncode == status, result.stdout + result.stderr
            assert hashlib.sha256(disk.read_bytes()).digest() == before
            if status:
                assert not output.exists()
                return result.stdout
            return output.read_bytes()

        for index in [0, 1]:
            record, start = records[index], starts[index]
            damaged = bytearray(raw)
            damaged[record * 512 + 446:record * 512 + 462] = bytes(16)
            disk.write_bytes(damaged)
            proposed = run(record, start)
            assert proposed == raw[record * 512:(record + 1) * 512]
            repaired = bytearray(damaged)
            repaired[record * 512:(record + 1) * 512] = proposed
            assert repaired == raw
            checked = root / 'CHECK.IMG'
            checked.write_bytes(repaired)
            for partition in [1, 2]:
                result = subprocess.run([str(exe), str(checked), '--partition', str(partition)], capture_output=True, text=True)
                assert result.returncode == 0, result.stdout
            # Exercise the actual transaction with a neighboring logical volume.
            journal = root / f'UNDO{bits}-{index}.MDJ'
            apply = subprocess.run([str(exe), str(disk), '--rebuild-ebr', str(record),
                                    '--volume-start', str(start), '--chs', '16', '63',
                                    '--apply', '--journal', str(journal)], capture_output=True, text=True)
            assert apply.returncode == 0, apply.stdout + apply.stderr
            assert disk.read_bytes() == raw
            undo = subprocess.run([str(exe), str(disk), '--restore-ebr', str(journal), '--apply'],
                                  capture_output=True, text=True)
            assert undo.returncode == 0, undo.stdout + undo.stderr
            assert disk.read_bytes() == damaged
            # Malformed flags/extra entries anywhere in the chain block a preview.
            other = records[1 - index]
            for offset, value in [(other * 512 + 446, 1), (other * 512 + 478, 1), (63 * 512 + 462, 128)]:
                corrupt = bytearray(damaged)
                corrupt[offset] = value
                disk.write_bytes(corrupt)
                run(record, start, status=2)
            disk.write_bytes(raw)
            assert 'empty' in run(record, start, status=2)
        # A loop after the selected record must still invalidate its preview.
        damaged = bytearray(raw)
        damaged[63 * 512 + 446:63 * 512 + 462] = bytes(16)
        offset = records[1] * 512
        damaged[offset + 462:offset + 478] = entry(15, records[1], records[1] - 63, total - records[1])
        disk.write_bytes(damaged)
        assert 'Loop' in run(63, starts[0], status=2)
        # A valid candidate cannot be inserted twice or beyond its container.
        damaged = bytearray(raw)
        damaged[63 * 512 + 446:63 * 512 + 462] = bytes(16)
        disk.write_bytes(damaged)
        assert 'Overlapping' in run(63, starts[1], status=2)
        damaged = bytearray(raw)
        damaged[offset + 446:offset + 462] = bytes(16)
        shorter_end = starts[1] + sectors - 1
        struct.pack_into('<I', damaged, 458, shorter_end - 63)
        struct.pack_into('<I', damaged, 63 * 512 + 474, shorter_end - records[1])
        disk.write_bytes(damaged)
        assert 'outside its container' in run(records[1], starts[1], status=2)
        # An erased final EBR signature can be reconstructed without changing links.
        damaged = bytearray(raw)
        offset = records[1] * 512
        damaged[offset + 446:offset + 462] = bytes(16)
        damaged[offset + 510:offset + 512] = bytes(2)
        disk.write_bytes(damaged)
        assert run(records[1], starts[1]) == raw[offset:offset + 512]
        # Disconnecting the second record makes it ineligible, even with a valid BPB.
        damaged[63 * 512 + 462:63 * 512 + 478] = bytes(16)
        disk.write_bytes(damaged)
        assert 'reachable' in run(records[1], starts[1], status=2)
        for signature_at in [512, len(damaged) - 512]:
            corrupt = bytearray(damaged)
            corrupt[signature_at:signature_at + 8] = b'EFI PART'
            disk.write_bytes(corrupt)
            assert 'GPT' in run(records[0], starts[0], status=2)
        if bits == 32:
            damaged = bytearray(raw)
            damaged[offset + 446:offset + 462] = bytes(16)
            damaged[starts[1] * 512 + 510:starts[1] * 512 + 512] = bytes(2)
            disk.write_bytes(damaged)
            assert 'corroborated' in run(records[1], starts[1], status=2)
            assert run(records[1], starts[1], source=6) == raw[offset:offset + 512]
        print(f'PASS FAT{bits}: first/last logical entry, preserved links/bootstrap, exact bytes, selectable volumes, ambiguous/unreachable/GPT refusals, source unchanged')
