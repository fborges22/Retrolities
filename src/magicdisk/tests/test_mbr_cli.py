"""User-facing MBR preview, apply and undo on disposable images."""
from pathlib import Path
import subprocess
import sys
import tempfile
from directory_fixture import create

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-mbr-cli-') as temporary:
    root = Path(temporary)
    for bits in [12, 16, 32]:
        volume, disk, journal = [root / name for name in ['VOL.IMG', 'DISK.IMG', 'UNDO.MDJ']]
        create(volume, bits)
        original = bytes(63 * 512) + volume.read_bytes()
        disk.write_bytes(original)
        journal.unlink(missing_ok=True)
        options = ['--rebuild-mbr', '2', '--volume-start', '63', '--chs-heads', '16', '--chs-sectors', '63']

        def run(args, status=0):
            result = subprocess.run([str(exe), str(disk)] + args, capture_output=True, text=True)
            assert result.returncode == status, result.stdout + result.stderr
            return result.stdout

        report = run(options)
        assert 'slot 2' in report and 'MISMATCH; unchanged' in report and 'Preview only' in report
        assert disk.read_bytes() == original and not journal.exists()
        for extra in [['--partition', '1'], ['--surface'], ['--find-volumes'],
                      ['--apply'], ['--journal', str(journal)], ['--accept-torn'],
                      ['--repair-tree'], ['--fat-source', '1']]:
            run(options + extra, 2)
            assert disk.read_bytes() == original and not journal.exists()
        run(['--rebuild-mbr', '1', '--volume-start', '63'], 2)
        run(['--chs-heads', '16'], 2)
        assert 'reconstructed and verified' in run(options + ['--apply', '--journal', str(journal)])
        applied = disk.read_bytes()
        assert applied[512:] == original[512:]
        assert 'LBA         63' in run(['--list'])
        assert run(['--partition', '1'])
        saved = journal.read_bytes()
        assert len(saved) == 1600 and saved[:8] == b'MDMBR001'
        assert saved[64:576] == original[:512] and saved[576:1088] == applied[:512]
        for invalid in [saved[:-1], saved + b'X', saved[:100] + bytes([saved[100] ^ 1]) + saved[101:]]:
            journal.write_bytes(invalid)
            run(['--restore-mbr', str(journal), '--apply'], 2)
            assert disk.read_bytes() == applied
        journal.write_bytes(saved)
        assert 'restored and verified' in run(['--restore-mbr', str(journal), '--apply'])
        assert disk.read_bytes() == original and journal.read_bytes() == saved
        print(f'PASS FAT{bits}: CLI preview, option conflicts, journal apply, partition scan, damaged-journal refusal, exact undo')
