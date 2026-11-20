#!/usr/bin/env python3
"""PLZ: the LZ77 format Polaris uses to pack stage 2 (decoder in boot/unpack.asm).

Bit stream (MSB first) interleaved with byte-aligned raw bytes read from the
same input stream:

    stream  := literals { after-literals }
    literals:= gamma(n) byte*n
    after-literals := 0 gamma(len)                 match, previous offset
                    | 1 offset gamma(len-1)        match, new offset (len >= 2)
    after-match    := 0 literals | 1 offset gamma(len-1)
    offset  := gamma(hi + 1) byte(lo)              offset - 1 = hi * 256 + lo
    end     := 1 gamma(256)                        (only where an offset may follow)

gamma(n), n >= 1: (bits(n) - 1) zero bits, then n in binary (MSB first).
The first token is always a literal run (no flag bit).

    python3 plz.py pack input output
"""
import sys

MAXOFF = 32768
MAXLEN = 65535


def gbits(n):
    return 2 * n.bit_length() - 1


class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.bitpos = None     # index of the current bit byte
        self.nbits = 8

    def bit(self, b):
        if self.nbits == 8:
            self.bitpos = len(self.out)
            self.out.append(0)
            self.nbits = 0
        if b:
            self.out[self.bitpos] |= 0x80 >> self.nbits
        self.nbits += 1

    def gamma(self, n):
        assert n >= 1
        k = n.bit_length()
        for _ in range(k - 1):
            self.bit(0)
        for i in range(k - 1, -1, -1):
            self.bit((n >> i) & 1)

    def byte(self, v):
        self.out.append(v & 255)


def off_cost(o):
    return gbits(((o - 1) >> 8) + 1) + 8


def pack(data):
    n = len(data)
    INF = float('inf')
    # best[i] = (cost, kind, arg, prev, lastoff); kind 'L' literal run (arg=len), 'M' new (off,len), 'R' rep (len)
    cost_l = [INF] * (n + 1)      # arrive at i, last token = literal run
    cost_m = [INF] * (n + 1)      # arrive at i, last token = match
    from_l = [None] * (n + 1)
    from_m = [None] * (n + 1)
    off_l = [0] * (n + 1)         # last offset on the best path ending with literals at i
    off_m = [0] * (n + 1)
    cost_m[0] = 0                 # virtual: stream starts expecting literals (no flag)
    from_m[0] = ('start',)
    chains = {}
    for i in range(n):
        # literal runs starting at i (from a match state, or the start)
        if cost_m[i] < INF:
            flag = 0 if i == 0 else 1
            for ln in range(1, min(n - i, 512) + 1):
                c = cost_m[i] + flag + gbits(ln) + 8 * ln
                if c < cost_l[i + ln]:
                    cost_l[i + ln] = c
                    from_l[i + ln] = ('L', ln, i)
                    off_l[i + ln] = off_m[i]
        # matches starting at i
        if i + 1 < n:
            key = data[i:i + 2]
            cands = chains.get(key, [])
            seen = set()
            for j in reversed(cands[-48:]):
                o = i - j
                if o > MAXOFF:
                    break
                if o in seen:
                    continue
                seen.add(o)
                ln = 0
                while i + ln < n and data[j + ln] == data[i + ln] and ln < MAXLEN:
                    ln += 1
                if ln < 2:
                    continue
                for src, base, last, tag in ((cost_l, cost_l[i], off_l[i], 'l'),
                                             (cost_m, cost_m[i], off_m[i], 'm')):
                    if base == INF or (tag == 'm' and i == 0):
                        continue
                    oc = off_cost(o)
                    lens = range(2, ln + 1) if ln <= 64 else list(range(2, 65)) + [ln]
                    for l in lens:
                        c = base + 1 + oc + gbits(l - 1)
                        if c < cost_m[i + l]:
                            cost_m[i + l] = c
                            from_m[i + l] = ('M', o, l, i, tag)
                            off_m[i + l] = o
            # repeat-offset match, only directly after literals
            o = off_l[i]
            if o and cost_l[i] < INF and i - o >= 0:
                ln = 0
                while i + ln < n and data[i - o + ln] == data[i + ln] and ln < MAXLEN:
                    ln += 1
                lens = range(1, ln + 1) if ln <= 64 else list(range(1, 65)) + [ln]
                for l in lens:
                    c = cost_l[i] + 1 + gbits(l)
                    if c < cost_m[i + l]:
                        cost_m[i + l] = c
                        from_m[i + l] = ('R', o, l, i, 'l')
                        off_m[i + l] = o
            chains.setdefault(key, []).append(i)
    # backtrack
    tokens = []
    i = n
    state = 'l' if cost_l[n] <= cost_m[n] else 'm'
    while i > 0:
        if state == 'l':
            k, ln, s = from_l[i]
            tokens.append(('L', s, ln))
            i, state = s, 'm'
        else:
            t = from_m[i]
            tokens.append((t[0], t[1], t[2]))
            i, state = t[3], t[4]
    tokens.reverse()
    assert tokens[0][0] == 'L'
    w = BitWriter()
    prev = None
    for t in tokens:
        if t[0] == 'L':
            if prev is not None:
                w.bit(0)
            w.gamma(t[2])
            for b in data[t[1]:t[1] + t[2]]:
                w.byte(b)
        elif t[0] == 'R':
            w.bit(0)
            w.gamma(t[2])
        else:
            w.bit(1)
            w.gamma(((t[1] - 1) >> 8) + 1)
            w.byte((t[1] - 1) & 255)
            w.gamma(t[2] - 1)
        prev = t[0]
    if prev == 'L':
        # after literals a 1 bit introduces a new offset
        pass
    w.bit(1)
    w.gamma(256)
    return bytes(w.out)


def unpack(src):
    out = bytearray()
    pos = 0
    bitbyte = 0
    nb = 0

    def bit():
        nonlocal pos, bitbyte, nb
        if nb == 0:
            bitbyte = src[pos]; pos += 1; nb = 8
        nb -= 1
        return (bitbyte >> nb) & 1

    def gamma():
        k = 0
        while not bit():
            k += 1
        v = 1
        for _ in range(k):
            v = v << 1 | bit()
        return v

    def byte():
        nonlocal pos
        pos += 1
        return src[pos - 1]

    last = 0
    ln = gamma()
    for _ in range(ln):
        out.append(byte())
    after_lit = True
    while True:
        if bit():
            hi = gamma()
            if hi == 256:
                return bytes(out)
            last = (hi - 1) * 256 + byte() + 1
            ln = gamma() + 1
        else:
            if after_lit:
                ln = gamma()
            else:
                ln = gamma()
                for _ in range(ln):
                    out.append(byte())
                after_lit = True
                continue
        for _ in range(ln):
            out.append(out[-last])
        after_lit = False


def main():
    if len(sys.argv) == 4 and sys.argv[1] == 'pack':
        data = open(sys.argv[2], 'rb').read()
        packed = pack(data)
        assert unpack(packed) == data, 'round trip failed'
        open(sys.argv[3], 'wb').write(packed)
        print(f'plz: {len(data)} -> {len(packed)} bytes')
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == '__main__':
    main()
