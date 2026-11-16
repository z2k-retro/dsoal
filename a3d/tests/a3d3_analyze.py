#!/usr/bin/env python3
"""Checks the output of a3d3_test.exe (OpenAL Soft's wave writer) against
its timeline. Usage: a3d3_analyze.py out.wav run3.txt

The test plays a 50 ms noise burst once a second. For each 3 s phase this
prints, per burst: the level 300-900 ms after the burst relative to the burst
(the reverb tail), the left/right balance of the burst, and the level 400-450
ms after it on each side (where the manual echo should land)."""
import struct
import sys

import numpy as np


def read_wav(path):
    d = open(path, 'rb').read()
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        cid, n = d[pos:pos + 4], struct.unpack('<I', d[pos + 4:pos + 8])[0]
        body = d[pos + 8:pos + 8 + n]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
        elif cid == b'data':
            data = body
        pos += 8 + n + (n & 1)
    tag, ch, rate, _, _, bits = fmt
    if tag == 3 or (tag == 0xfffe and bits == 32):
        x = np.frombuffer(data[:len(data) // 4 * 4], '<f4').astype(float)
    else:
        x = np.frombuffer(data[:len(data) // 2 * 2], '<i2').astype(float) / 32768
    return x[:len(x) // ch * ch].reshape(-1, ch), rate


def db(p):
    return 10 * np.log10(p + 1e-20)


def main():
    x, rate = read_wav(sys.argv[1])
    phases = [(l.split()[1], float(l.split()[2])) for l in open(sys.argv[2]) if l.startswith('PHASE')]
    e = (x ** 2).sum(1)
    hop = rate // 1000
    env = np.convolve(e, np.ones(hop) / hop, 'same')
    first = int(np.argmax(env > env.max() * 0.05))  # first burst onset
    print('first burst at %.3f s in the file' % (first / rate))
    ms = lambda t: int(t * rate / 1000)
    for (name, t), (_, t_next) in zip(phases, phases[1:]):
        rows = []
        k0 = int(round(t)) + 1 if name != 'dry' else 0
        for k in range(k0, int(round(t_next))):
            on = first + k * rate
            # refine onset within +-30 ms
            w = env[on - ms(30):on + ms(30)]
            if len(w) == 0:
                continue
            on = on - ms(30) + int(np.argmax(w > w.max() * 0.2))
            burst = x[on:on + ms(50)]
            tail = x[on + ms(300):on + ms(900)]
            echo = x[on + ms(400):on + ms(450)]
            pb = (burst ** 2).mean(0)
            rows.append((db((tail ** 2).sum(1).mean()) - db(pb.sum()),
                         db(pb[0]) - db(pb[1]),
                         db((echo[:, 0] ** 2).mean()) - db(pb.sum()),
                         db((echo[:, 1] ** 2).mean()) - db(pb.sum())))
        r = np.array(rows)
        print('%-7s bursts %d  tail %6.1f dB  L-R %5.1f dB  echo window L %6.1f R %6.1f dB'
              % (name, len(r), r[:, 0].mean(), r[:, 1].mean(), r[:, 2].mean(), r[:, 3].mean()))


main()
