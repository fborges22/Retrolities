"""Drive QEMU through its monitor: boot an image, send keys, take screenshots."""
import pathlib
import subprocess
import tempfile
import time


KEYMAP = {' ': 'spc', '\n': 'ret', '.': 'dot', ',': 'comma', '-': 'minus', '+': 'shift-equal',
          '/': 'slash', '\\': 'backslash', ':': 'shift-semicolon', '*': 'shift-8', '_': 'shift-minus',
          '=': 'equal', ';': 'semicolon', '>': 'shift-dot', '<': 'shift-comma'}


class VM:
    def __init__(self, args, memory=32):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix='polaris-qemu-'))
        argv = ['qemu-system-i386', '-accel', 'tcg', '-m', str(memory), '-display', 'none',
                '-serial', 'none', '-monitor', 'stdio', '-no-reboot', '-rtc',
                'base=2026-11-19T21:47:00'] + args
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
        self.n = 0

    def cmd(self, c):
        self.p.stdin.write((c + '\n').encode())
        self.p.stdin.flush()

    def key(self, k, wait=0.25):
        self.cmd(f'sendkey {k} 60')
        time.sleep(wait)

    def type(self, text, wait=0.12):
        for ch in text:
            if ch.isupper():
                k = 'shift-' + ch.lower()
            else:
                k = KEYMAP.get(ch, ch)
            self.key(k, wait)

    def shot(self, path, crop=True):
        ppm = self.tmp / f'shot{self.n}.ppm'
        self.n += 1
        self.cmd(f'screendump {ppm}')
        for _ in range(50):
            time.sleep(0.1)
            if ppm.exists() and ppm.stat().st_size > 1000:
                time.sleep(0.2)
                break
        from PIL import Image
        img = Image.open(ppm).convert('RGB')
        img.save(path)
        return img

    def text(self):
        """the 80x25 text page, read from video memory through the monitor"""
        import os
        import re
        import select
        fd = self.p.stdout.fileno()
        while select.select([fd], [], [], 0.1)[0]:      # drain old output
            if not os.read(fd, 65536):
                break
        self.cmd('xp /2000hx 0xb8000')
        buf = b''
        deadline = time.time() + 10
        words = []
        while time.time() < deadline:
            if select.select([fd], [], [], 0.2)[0]:
                buf += os.read(fd, 65536)
                words = re.findall(rb'0x([0-9a-f]{4})\b', buf.split(b'xp /2000hx', 1)[-1])
                words = [w for w in words if len(w) == 4]
                if len(words) >= 2000 + 250:
                    break
        lines = [l for l in buf.decode(errors='replace').splitlines() if re.match(r'^[0-9a-f]{8}:', l)]
        cells = []
        for l in lines:
            cells += [int(w, 16) for w in l.split(':', 1)[1].split()]
        chars = ''.join(chr(c & 0xFF) if 32 <= (c & 0xFF) < 127 else ' ' for c in cells[:2000])
        return '\n'.join(chars[i:i + 80] for i in range(0, len(chars), 80))

    def wait_text(self, needle, timeout=30):
        end = time.time() + timeout
        t = ''
        while time.time() < end:
            t = self.text()
            if needle in t:
                return t
            time.sleep(1)
        raise AssertionError(f'{needle!r} not on screen:\n{t}')

    def quit(self):
        try:
            self.cmd('quit')
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()
