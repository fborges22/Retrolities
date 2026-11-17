#!/usr/bin/env python3
"""Drive the real workspace (ui.c) through F9, the Fix menu and P shortcuts."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from xlink_fixture import crosslinked, fsck, read_file

cli = Path(sys.argv[1]).resolve()
ui = Path(sys.argv[2]).resolve()


def play(root, image, lines, code=0):
    keys = root / 'keys.txt'
    keys.write_text('\n'.join(lines) + '\n')
    screen = root / 'screen.txt'
    result = subprocess.run([str(ui), '--tui', str(image)], capture_output=True, text=True,
                            env=dict(os.environ, MAGICDISK_KEYS=str(keys),
                                     MAGICDISK_SCREEN=str(screen)))
    shown = screen.read_text() if screen.exists() else ''
    assert result.returncode == code, (result.returncode, result.stderr, shown)
    return shown


def scan(image):
    result = subprocess.run([str(cli), str(image)], capture_output=True, text=True)
    return result.returncode, result.stdout


def journal(path):
    return ['wait:NEW JOURNAL FILE', f'type:{path}', 'enter']


with tempfile.TemporaryDirectory(prefix='magicdisk-fix-ui-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        work = root / f'w{bits}'
        work.mkdir()
        image = root / f'fat{bits}.img'
        payload, _, broken, _, _, _ = crosslinked(image, bits, work)
        opened = ['wait:Allocation view ready.']
        # The harness itself must reject missing text and runaway scripts.
        play(root, image, opened + ['wait:NO SUCH SCREEN TEXT'], code=3)
        play(root, image, opened + ['absent:Allocation view ready.'], code=3)
        play(root, image, opened, code=4)

        # Menu bar shows Fix; Left from Disk wraps to it; cancelling writes nothing.
        play(root, image, opened + [
            'wait:Help    Fix', 'f10', 'wait:Open image...', 'left',
            'wait:Automatic repair...      F9', 'wait:Copy cross-linked files...',
            'wait:Convert lost chains...', 'wait:Automatic + surface...', 'wait:CMOS check...',
            'enter',
            'wait:REVIEW AUTOMATIC REPAIR', 'wait:Pending Copy cross-linked clusters', 'esc',
            'wait:Automatic repair cancelled; no writes made.', 'esc'])
        assert image.read_bytes() == broken

        # The CMOS check is read-only; the host build explains it needs DOS.
        play(root, image, opened + ['f10', 'left', 'end', 'wait:CMOS check...', 'enter',
                                    'wait:CMOS access needs MAGICDSK.EXE under DOS', 'esc'])
        assert image.read_bytes() == broken

        # P on CHAIN_SHARED opens cross-link copies (not truncating tree repair).
        code, report = scan(image)
        codes = re.findall(r'^  ([A-Z_]+)\s+\d+\s', report, re.M)
        index = codes.index('CHAIN_SHARED')
        play(root, image, opened + ['f3', 'wait:Check finished.', 'f6',
                                    'wait:INSPECTION FINDINGS'] + ['down'] * index +
             ['wait:P: review volume repair', 'key:p', 'wait:REVIEW CROSS-LINK COPIES',
              'wait:Copy shared clusters for 2 cross-linked files.', 'esc',
              'wait:Repair cancelled; no writes made.', 'esc'])
        assert image.read_bytes() == broken

        # Fix menu: copy cross-links, then adopt lost chains, each journaled.
        play(root, image, opened + [
            'f10', 'left', 'down', 'down', 'wait:Copy cross-linked files...', 'enter',
            'wait:REVIEW CROSS-LINK COPIES', 'enter', 'wait:TYPE COPY', 'type:COPY', 'enter'] +
            journal(root / f'x{bits}.mdj') + ['wait:Repair verified.',
            'f10', 'left', 'up', 'wait:CMOS check...', 'up', 'wait:Convert lost chains...', 'enter',
            'wait:REVIEW LOST-CHAIN ADOPTION', 'wait:as 2 root FILEnnnn.CHK files', 'enter',
            'wait:TYPE ADOPT', 'type:ADOPT', 'enter'] + journal(root / f'l{bits}.mdj') +
            ['wait:Repair verified.', 'f3', 'wait:Check finished.', 'esc'])
        code, report = scan(image)
        assert code == 0 and ' 0 issues' in report, report
        fsck(image)
        assert read_file(image, 'CHARLIE.BIN') == payload['ALPHA.BIN']
        for name in [f'l{bits}.mdj', f'x{bits}.mdj']:
            subprocess.run([str(cli), str(image), '--restore-journal', str(root / name),
                            '--apply'], check=True, capture_output=True)
        assert image.read_bytes() == broken

        # F9 commits every step with per-step journals and reports a clean result.
        journals = root / f'j{bits}'
        journals.mkdir()
        play(root, image, opened + [
            'f9', 'wait:REVIEW AUTOMATIC REPAIR', 'wait:5 issues', 'enter',
            'wait:TYPE FIX TO REPAIR EVERYTHING', 'type:fix', 'enter',
            'wait:Automatic repair cancelled; no writes made.',
            'f9', 'wait:REVIEW AUTOMATIC REPAIR', 'enter', 'type:FIX', 'enter',
            'wait:JOURNAL DIRECTORY ON A SEPARATE DISK', f'type:{journals}', 'enter',
            'wait:AUTOMATIC REPAIR RESULTS', 'wait:Now: 0 issues, 0 lost',
            'wait:Fixed   Copy cross-linked clusters',
            'wait:Fixed   Convert lost chains to files', 'enter',
            'wait:Automatic repair verified: no remaining issues.', 'esc'])
        code, report = scan(image)
        assert code == 0, report
        fsck(image)
        names = sorted(p.name for p in journals.iterdir())
        assert names[:2] == ['MDFX0001.MDJ', 'MDFX0002.MDJ'], names
        for name in reversed(names):
            subprocess.run([str(cli), str(image), '--restore-journal', str(journals / name),
                            '--apply'], check=True, capture_output=True)
        assert image.read_bytes() == broken

        # A repaired volume: F9 reports nothing to do and writes nothing.
        cleaned = root / f'k{bits}'
        cleaned.mkdir()
        subprocess.run([str(cli), str(image), '--autofix', '--apply', '--journal', str(cleaned)],
                       check=True, capture_output=True)
        fixed = image.read_bytes()
        play(root, image, opened + ['f9', 'wait:No problems found; no writes made.', 'esc'])
        assert image.read_bytes() == fixed
    print('PASS: Fix menu, F9 autofix, P routing, typed confirmations and journals in the workspace')
