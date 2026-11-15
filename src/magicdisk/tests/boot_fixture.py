"""Copy boot media for QEMU without modifying the user-supplied source."""
import shutil
import subprocess


def copy_boot(source, destination):
    if source.suffix.lower() != '.7z':
        shutil.copy2(source, destination)
        return

    archiver = shutil.which('7z') or shutil.which('7zz')
    if not archiver:
        raise RuntimeError('Reading a .7z boot archive requires 7z or 7zz')
    listing = subprocess.check_output(
        [archiver, 'l', '-slt', '--', str(source)], text=True)
    members = [line[7:] for line in listing.splitlines()
               if line.startswith('Path = ') and line.lower().endswith('.img')]
    if len(members) != 1:
        raise ValueError('Boot archive must contain exactly one .img file')
    # Stream only the chosen image; archive paths never become filesystem paths.
    with destination.open('wb') as output:
        subprocess.run([archiver, 'x', '-so', '-spd', '--', str(source), members[0]],
                       stdout=output, check=True)


