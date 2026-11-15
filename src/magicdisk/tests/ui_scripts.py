"""Screen-driven BIOS keyboard tours for production DOS executable tests."""
KEYS = {'enter': 0x000d, 'esc': 0x001b, 'f10': 0x4400, 'right': 0x4d00,
        'up': 0x4800, 'down': 0x5000, 'end': 0x4f00, 'f3': 0x3d00,
        'f4': 0x3e00, 'f8': 0x4200, 'home': 0x4700, 'pgup': 0x4900, 'pgdn': 0x5100, 'f5': 0x3f00, 'f6': 0x4000, 'f7': 0x4100, 'left': 0x4b00}


def step(cue, key, focus=False, absent=''):
    if any(char in cue or char in absent for char in ['|', '\r', '\n']):
        raise ValueError('KEYPLAY cues cannot contain field separators or newlines')
    value = KEYS[key] if isinstance(key, str) else key
    return f'{0x3f if focus else 0:x}|{cue}|{absent}|{value:04x}\n'


def typing(cue, text):
    return ''.join(step(cue, ord(char)) for char in text) + step(cue, 'enter')


def diagnose():
    return step('Allocation view ready.', 'f10') + step('Open image...', 'right', True)


def save_report(name):
    return (step('Check finished.', 'f7') + typing('SAVE REPORT TO A SEPARATE DISK', name) +
            step('Report saved.', 'esc'))


main_keys = (diagnose() + step('Filesystem check', 'right', True) +
             step('Allocation map', 'right', True) + step('Save report...', 'right', True) +
             step('Keyboard / guide', 'down', True) + step('About MagicDisk', 'enter', True) +
             step('ABOUT MAGICDISK', 'esc') + step('Allocation view ready.', 'f3', absent='ABOUT MAGICDISK') +
             step('Check finished.', 'f4') + step('Surface read finished.', 'f7') +
             typing('SAVE REPORT TO A SEPARATE DISK', 'SURFACE.TXT') +
             step('Report saved.', 'f5') +
             step('Allocation view ready.', 'esc'))
marker_keys = (diagnose() + step('Filesystem check', 'down', True) +
               step('Surface read', 'down', True) + step('Retest marked bad', 'down', True) +
               step('Mark bad sector', 'enter', True) +
               typing('MARK BAD: ABSOLUTE DISK SECTOR', '372') +
               step('REVIEW BAD-CLUSTER CHANGE', 'enter') + typing('CONFIRM MARKER CHANGE', 'MARK') +
               typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'UI.MDJ') +
               step('Repair verified.', 'f10') + step('Open image...', 'right', True) +
               step('Filesystem check', 'down', True) + step('Surface read', 'down', True) +
               step('Retest marked bad', 'enter', True) + step('Bad markers retained.', 'f6') +
               step('INSPECTION FINDINGS', 'esc') +
               step('Bad markers retained.', 'esc', absent='INSPECTION FINDINGS'))
device_keys = (step('Ready. F10 opens the menu', 'f10') + step('Open image...', 'down', True) +
               step('BIOS device...', 'enter', True))
floppy_keys = (device_keys + step('Floppy drive 1', 'enter', True) +
               step('Allocation view ready.', 'f3', absent='SELECT BIOS DEVICE') + save_report('FD.TXT'))
hard_keys = (device_keys + step('SELECT BIOS DEVICE', 'end') + step('Other device...', 'up', True) +
             step('Hard disk 1', 'enter', True) + step('FAT16', 'down', True) +
             step('FAT32', 'enter', True) +
             step('Allocation view ready.', 'f3', absent='SELECT VOLUME') +
             step('Check finished.', 'f7') + typing('SAVE REPORT TO A SEPARATE DISK', 'HD.TXT') +
             step('Save reports on a different physical disk.', 'f7') +
             typing('SAVE REPORT TO A SEPARATE DISK', 'A:\\HD.TXT') +
             step('Report saved.', 'esc'))
fsinfo_keys = (diagnose() + step('Filesystem check', 'up', True) +
               step('Clear invalid timestamps', 'up', True) +
               step('Clear invalid long names', 'up', True) +
               step('Repair invalid DOS names', 'up', True) +
               step('Rebuild FAT32 FSInfo', 'enter', True) + step('REVIEW FSINFO REPAIR', 'enter') +
               typing('TYPE REBUILD TO REPAIR FSINFO', 'REBUILD') +
               typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'FSUI.MDJ') +
               step('Repair verified.', 'f3') + save_report('FS.TXT'))
lfn_keys = (step('Allocation view ready.', 'f3') + step('Check finished.', 'f6') +
            step('INSPECTION FINDINGS', 'right') + step('INSPECTION FINDINGS', 'right') +
            step('INSPECTION FINDINGS', 'left') + step('INSPECTION FINDINGS', 'esc') +
            step('Check finished.', 'f7', absent='INSPECTION FINDINGS') +
            typing('SAVE REPORT TO A SEPARATE DISK', 'LFN.TXT') + step('Report saved.', 'esc'))


boot_keys = (diagnose() + step('Filesystem check', 'up', True) +
             step('Clear invalid timestamps', 'up', True) +
             step('Clear invalid long names', 'up', True) +
             step('Repair invalid DOS names', 'up', True) +
             step('Rebuild FAT32 FSInfo', 'up', True) +
             step('Rebuild backup boot', 'enter', True) +
             step('REVIEW BACKUP BOOT REPAIR', 'enter') +
             typing('TYPE REBUILD TO REPAIR BACKUP BOOT', 'REBUILD') +
             typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'BOOTUI.MDJ') +
             step('Repair verified.', 'f3') + save_report('BOOTUI.TXT'))

mounted_keys = (step('Ready. F10 opens the menu', 'f10') + step('Open image...', 'down', True) +
                step('BIOS device...', 'down', True) + step('DOS volume...', 'enter', True) +
                typing('OPEN DOS VOLUME (DRIVE LETTER)', 'D') +
                marker_keys.replace('ABSOLUTE DISK SECTOR', 'VOLUME SECTOR')
                .replace(typing('MARK BAD: VOLUME SECTOR', '372'),
                         typing('MARK BAD: VOLUME SECTOR', '3048'))
                .replace(typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'UI.MDJ'),
                         typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'C:\\RAWUI.MDJ')))

mirror_keys = (diagnose() + step('Filesystem check', 'up', True) +
               step('Clear invalid timestamps', 'up', True) +
               step('Clear invalid long names', 'up', True) +
               step('Repair invalid DOS names', 'up', True) +
               step('Rebuild FAT32 FSInfo', 'up', True) +
               step('Rebuild backup boot', 'up', True) +
               step('Rebuild FAT copies', 'enter', True) +
               typing('SOURCE FAT NUMBER TO KEEP', '1') +
               step('REVIEW FAT-COPY REPAIR', 'enter') +
               typing('TYPE COPY TO USE THIS SOURCE FAT', 'COPY') +
               typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'C:\\FATUI.MDJ') +
               step('Repair verified.', 'f3') + save_report('FATUI.TXT'))
mounted_mirror_keys = (step('Ready. F10 opens the menu', 'f10') + step('Open image...', 'down', True) +
                       step('BIOS device...', 'down', True) + step('DOS volume...', 'enter', True) +
                       typing('OPEN DOS VOLUME (DRIVE LETTER)', 'D') + mirror_keys)

mounted_recovery_keys = (
    step('Ready. F10 opens the menu', 'f10') + step('Open image...', 'down', True) +
    step('BIOS device...', 'down', True) + step('DOS volume...', 'enter', True) +
    typing('OPEN DOS VOLUME (DRIVE LETTER)', 'D') + diagnose() +
    step('Filesystem check', 'up', True) +
    step('Clear invalid timestamps', 'up', True) +
    step('Clear invalid long names', 'up', True) + step('Rebuild FAT32 FSInfo', 'up', True) +
    step('Repair invalid DOS names', 'up', True) +
    step('Rebuild backup boot', 'up', True) + step('Rebuild FAT copies', 'up', True) +
    step('Rebuild FAT headers', 'up', True) +
    step('Recover lost chains...', 'enter', True) + step('RECOVER LOST CHAINS', 'enter') +
    typing('RECOVERY DESTINATION DIRECTORY', 'C:\\') +
    step('LOST-CHAIN RECOVERY RESULTS', 'enter') +
    step('Recovery finished; source allocation unchanged.', 'esc'))

mounted_reclaim_keys = (
    step('Ready. F10 opens the menu', 'f10') + step('Open image...', 'down', True) +
    step('BIOS device...', 'down', True) + step('DOS volume...', 'enter', True) +
    typing('OPEN DOS VOLUME (DRIVE LETTER)', 'D') + diagnose() +
    step('Filesystem check', 'up', True) +
    step('Clear invalid timestamps', 'up', True) +
    step('Clear invalid long names', 'up', True) + step('Rebuild FAT32 FSInfo', 'up', True) +
    step('Repair invalid DOS names', 'up', True) +
    step('Rebuild backup boot', 'up', True) + step('Rebuild FAT copies', 'up', True) +
    step('Rebuild FAT headers', 'up', True) +
    step('Recover lost chains...', 'up', True) + step('Reclaim lost allocation...', 'enter', True) +
    step('REVIEW LOST-ALLOCATION REPAIR', 'enter') +
    typing('TYPE RECLAIM TO FREE LOST ALLOCATION', 'RECLAIM') +
    typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'C:\\LOSTUI.MDJ') +
    step('Repair verified.', 'f3') + save_report('LOSTUI.TXT'))

chain_menu = (
    step('Filesystem check', 'up', True) +
    step('Clear invalid timestamps', 'up', True) +
    step('Clear invalid long names', 'up', True) + step('Rebuild FAT32 FSInfo', 'up', True) +
    step('Repair invalid DOS names', 'up', True) +
    step('Rebuild backup boot', 'up', True) + step('Rebuild FAT copies', 'up', True) +
    step('Rebuild FAT headers', 'up', True) +
    step('Recover lost chains...', 'up', True) + step('Reclaim lost allocation...', 'up', True) +
    step('Repair file chains...', 'enter', True))
mounted_chain_keys = (
    step('Ready. F10 opens the menu', 'f10') + step('Open image...', 'down', True) +
    step('BIOS device...', 'down', True) + step('DOS volume...', 'enter', True) +
    typing('OPEN DOS VOLUME (DRIVE LETTER)', 'D') + diagnose() + chain_menu +
    step('REVIEW FILE-CHAIN REPAIR', 'esc') + step('File repair cancelled;', 'f6') +
    step('FILE_REPAIR', 'esc') + step('File repair cancelled;', 'f7') +
    typing('SAVE REPORT TO A SEPARATE DISK', 'D:\\NO.TXT') +
    step('Save reports on a different volume', 'f7') +
    typing('SAVE REPORT TO A SEPARATE DISK', 'C:\\FPREV.TXT') +
    step('Report saved.', 'f10') + step('Open image...', 'right', True) + chain_menu +
    step('REVIEW FILE-CHAIN REPAIR', 'enter') +
    typing('TYPE REPAIR TO CORRECT FILE CHAINS', 'REPAIR') +
    typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'C:\\FILESUI.MDJ') +
    step('Repair verified.', 'f3') + save_report('FILESUI.TXT'))

directory_menu = chain_menu.replace(step('Repair file chains...', 'enter', True),
                                    step('Repair file chains...', 'up', True) +
                                    step('Repair directory chains...', 'enter', True))
mounted_directory_keys = (
    step('Ready. F10 opens the menu', 'f10') + step('Open image...', 'down', True) +
    step('BIOS device...', 'down', True) + step('DOS volume...', 'enter', True) +
    typing('OPEN DOS VOLUME (DRIVE LETTER)', 'D') + diagnose() + directory_menu +
    step('REVIEW DIRECTORY-CHAIN REPAIR', 'esc') + step('Directory repair cancelled;', 'f6') +
    step('DIR_REPAIR', 'esc') + step('Directory repair cancelled;', 'f7') +
    typing('SAVE REPORT TO A SEPARATE DISK', 'D:\\NO.TXT') +
    step('Save reports on a different volume', 'f7') +
    typing('SAVE REPORT TO A SEPARATE DISK', 'C:\\DPREV.TXT') +
    step('Report saved.', 'f10') + step('Open image...', 'right', True) + directory_menu +
    step('REVIEW DIRECTORY-CHAIN REPAIR', 'enter') +
    typing('TYPE REPAIR TO CORRECT DIRECTORY CHAINS', 'REPAIR') +
    typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'C:\\DIRSUI.MDJ') +
    step('Repair verified.', 'f3') + save_report('DIRSUI.TXT'))

primary_keys = (diagnose() + step('Filesystem check', 'down', True) +
                step('Surface read', 'down', True) + step('Retest marked bad', 'down', True) +
                step('Mark bad sector', 'down') + step('Unmark bad sector', 'down') +
                step('Restore journal', 'down', True) + step('Recover primary boot', 'enter', True) +
                typing('BACKUP BOOT SECTOR (USUALLY 6)', '6') +
                step('REVIEW PRIMARY BOOT RECOVERY', 'enter') +
                typing('TYPE RECOVER TO RESTORE PRIMARY BOOT', 'RECOVER') +
                typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'PRIMARYU.MDJ') +
                step('Repair verified.', 'f3') + save_report('PRIMARYU.TXT'))

mounted_tree_keys = (mounted_directory_keys
    .replace(step('Repair directory chains...', 'enter', True),
             step('Repair directory chains...', 'up', True) +
             step('Repair filesystem tree...', 'enter', True))
    .replace('REVIEW DIRECTORY-CHAIN REPAIR', 'REVIEW FILESYSTEM TREE REPAIR')
    .replace('Directory repair cancelled;', 'Combined repair cancelled;')
    .replace('TYPE REPAIR TO CORRECT DIRECTORY CHAINS', 'TYPE REPAIR TO CORRECT FILESYSTEM TREE'))

mounted_source_keys = (mounted_tree_keys
    .replace(step('Repair filesystem tree...', 'enter', True),
             step('Repair filesystem tree...', 'up', True) +
             step('Repair from FAT copy...', 'enter', True) +
             typing('SOURCE FAT NUMBER TO REPAIR AND KEEP', '2')))

restore_menu = (step('Report saved.', 'f10') + step('Open image...', 'right', True) +
                step('Filesystem check', 'down', True) + step('Surface read', 'down', True) +
                step('Retest marked bad', 'down', True) + step('Mark bad sector', 'down', True) +
                step('Unmark bad sector', 'down', True) + step('Restore journal', 'enter', True) +
                typing('JOURNAL TO RESTORE', 'C:\\FATUI.MDJ') +
                typing('TYPE RESTORE TO RESTORE ORIGINAL METADATA', 'RESTORE'))
transaction_keys = (mounted_mirror_keys
    .replace(step('Repair verified.', 'f3'),
             step('Commit repair', 'esc') + step('Repair verified.', 'f3'))
    .replace(step('Report saved.', 'esc'),
             restore_menu + step('Check undo sectors', 'esc') +
             step('Journal restoration cancelled;', 'f3') +
             step('Check finished.', 'f7') + typing('SAVE REPORT TO A SEPARATE DISK', 'C.TXT') +
             restore_menu + step('Restore metadata', 'esc') +
             step('Original metadata restored and verified.', 'esc')))


mounted_entry_keys = (mounted_tree_keys
    .replace(step('Repair filesystem tree...', 'enter', True),
             step('Repair filesystem tree...', 'up', True) +
             step('Repair from FAT copy...', 'up', True) +
             step('Repair directory entries...', 'enter', True))
    .replace('REVIEW FILESYSTEM TREE REPAIR', 'REVIEW DIRECTORY-ENTRY REPAIR')
    .replace('Combined repair cancelled;', 'Entry repair cancelled;')
    .replace('DIR_REPAIR', 'ENTRY_REPAIR')
    .replace('TYPE REPAIR TO CORRECT FILESYSTEM TREE', 'TYPE REPAIR TO CORRECT DIRECTORY ENTRIES'))



def sector_keys(boot, data_sector, hold=0):
    first_bytes = ' '.join(f'{value:02X}' for value in boot[:8])
    last_row = '1F0  ' + ' '.join(f'{value:02X}' for value in boot[496:512])
    return (
        step('Allocation view ready.', 'f8') +
        step(f'Disk LBA {data_sector} /', 'home') +
        step(first_bytes, ord('v')) * hold +
        step(first_bytes, ord('r')) + step('mkfs.fat', 'right') + step(last_row, 'pgup') +
        step('Page 2/2', 'left') + step('Page 1/2', 'pgdn') +
        step('Disk LBA 1 /', 'pgup') + step('Disk LBA 0 /', 'end') +
        step('Disk LBA 2879 /', 'pgdn') + step('Disk LBA 2879 /', ord('g')) +
        typing('GO TO ABSOLUTE DISK LBA', '2880') +
        step('Invalid disk LBA;', ord('g')) + typing('GO TO ABSOLUTE DISK LBA', '0x0') +
        step(first_bytes, ord('r')) + step(first_bytes, 'esc') +
        step('Allocation view ready.', 'enter', absent='READ-ONLY SECTOR VIEWER') +
        step('BLOCK DETAILS', 'enter') + step(f'Disk LBA {data_sector} /', 'esc') +
        step('Allocation view ready.', 'f10', absent='READ-ONLY SECTOR VIEWER') +
        step('Open image...', 'right', True) + step('Filesystem check', 'right', True) +
        step('Allocation map', 'down', True) + step('Zoom into block', 'down', True) +
        step('Whole volume', 'down', True) + step('Findings', 'down', True) +
        step('Sector viewer...', 'enter', True) +
        step('READ-ONLY SECTOR VIEWER', 'esc') +
        step('Allocation view ready.', 'esc', absent='READ-ONLY SECTOR VIEWER'))


# Inspect the whole BIOS disk from a selected FAT32 partition, then continue its scan.
hard_keys = hard_keys.replace(
    step('Allocation view ready.', 'f3', absent='SELECT VOLUME'),
    step('Allocation view ready.', 'f8', absent='SELECT VOLUME') +
    step('READ-ONLY SECTOR VIEWER', 'home') + step('Disk LBA 0 /', 'right') +
    step('55 AA', 'esc') +
    step('Allocation view ready.', 'f3', absent='READ-ONLY SECTOR VIEWER'))


raw_sector_keys = (
    step('RAW.IMG', 'f8') + step('00 01 02 03 04 05 06 07', 'right') +
    step('100  00 01 02 03', 'left') + step('0123456789:;<=>?', 'end') +
    step('Disk LBA 0 / 0', 'pgdn') + step('Disk LBA 0 / 0', ord('g')) +
    typing('GO TO ABSOLUTE DISK LBA', '1') + step('Invalid disk LBA;', ord('g')) +
    typing('GO TO ABSOLUTE DISK LBA', '0') + step('Disk LBA 0 / 0', 'esc') +
    step('RAW.IMG', 'esc', absent='READ-ONLY SECTOR VIEWER'))


live_export_keys = (
    step('Allocation view ready.', 'f10') + step('Open image...', 'right', True) +
    step('Filesystem check', 'right', True) + step('Allocation map', 'right', True) +
    step('Save report...', 'down', True) + step('Export live files...', 'enter', True) +
    step('EXPORT LIVE FILES', 'enter') + typing('RECOVERY DESTINATION DIRECTORY', 'C:\\') +
    step('LIVE-FILE EXPORT RESULTS', 'enter') +
    step('Recovery finished with recorded gaps or damage.', 'esc'))


torn_restore_keys = (
    step('Allocation view ready.', 'f10') + step('Open image...', 'right', True) +
    step('Filesystem check', 'right', True) + step('Allocation map', 'right', True) +
    step('Save report...', 'down', True) + step('Export live files...', 'down', True) +
    step('Recover interrupted write...', 'enter', True) +
    step('RECOVER INTERRUPTED WRITE', 'enter') + typing('JOURNAL TO RESTORE', 'TORN.MDJ') +
    typing('TYPE RESTORE TO RESTORE ORIGINAL METADATA', 'RESTORE') +
    step('Original metadata restored and verified.', 'esc'))


occupied_dot_keys = (
    step('Allocation view ready.', 'f10') + step('Open image...', 'right', True) +
    ''.join(step(cue, 'down', True) for cue in [
        'Filesystem check', 'Surface read', 'Retest marked bad', 'Mark bad sector',
        'Unmark bad sector', 'Restore journal', 'Recover primary boot']) +
    step('Repair directory entries...', 'enter', True) +
    step('REVIEW DIRECTORY-ENTRY REPAIR', 'enter') +
    typing('TYPE REPAIR TO CORRECT DIRECTORY ENTRIES', 'REPAIR') +
    typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'SPACE.MDJ') +
    step('Repair verified.', 'esc'))
