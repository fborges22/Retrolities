#!/usr/bin/env python3
"""Writes a Program Manager group file (Windows 3.1 .GRP, "PMCC") for tests:
   mkgrp.py OUT.GRP "Group name" "Item|command|working dir|icon file|index|hotkey" ...
The hotkey is Program Manager's word: virtual key | (1 Shift, 2 Ctrl, 4 Alt) << 8."""
import struct
import sys


def grp(name, items):
    n = len(items)
    head = 34 + 2 * n
    data = bytearray(head)
    data[0:4] = b'PMCC'

    def add(b):
        off = len(data)
        data.extend(b)
        return off

    def string(s):
        return add(s.encode('latin-1') + b'\0')

    struct.pack_into('<H', data, 8, 1)                       # nCmdShow
    struct.pack_into('<hhhh', data, 10, 10, 10, 300, 200)    # rcNormal
    struct.pack_into('<H', data, 22, string(name))
    struct.pack_into('<H', data, 32, n)
    for i, (title, cmd, wdir, icon, idx, hot) in enumerate(items):
        rec = add(bytes(24))
        struct.pack_into('<hhH', data, rec, 10 + 70 * i, 10, idx)
        struct.pack_into('<H', data, rec + 6, 0x000c)
        struct.pack_into('<HHH', data, rec + 18, string(title), string(cmd), string(icon))
        struct.pack_into('<H', data, 34 + 2 * i, rec)
    struct.pack_into('<H', data, 6, len(data))               # cbGroup: start of the tags
    add(struct.pack('<HHH', 0x8000, 0xFFFF, 10) + b'PMCC')
    for i, item in enumerate(items):
        if item[2]:
            d = item[2].encode('latin-1') + b'\0'
            add(struct.pack('<HHH', 0x8101, i, 6 + len(d)) + d)
        if item[5]:
            add(struct.pack('<HHHH', 0x8102, i, 8, item[5]))
    add(struct.pack('<HHH', 0xFFFF, 0xFFFF, 0))
    return bytes(data)


if __name__ == '__main__':
    entries = []
    for arg in sys.argv[3:]:
        title, cmd, wdir, icon, idx, hot = (arg.split('|') + ['', '', '', '', '0', '0'])[:6]
        entries.append((title, cmd, wdir, icon, int(idx or 0), int(hot or 0)))
    with open(sys.argv[1], 'wb') as f:
        f.write(grp(sys.argv[2], entries))
