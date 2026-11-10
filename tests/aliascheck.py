# SPDX-License-Identifier: GPL-3.0-only
"""SAMPLE RATE 88.2K (oversampled), heard: build/host/alias-24.wav is one bright note (tests/mksyx.py's BRIGHT, C7) with
it ON, then again with it OFF. Every partial of that voice is a whole harmonic of the note, so what
lands between the harmonics is what folded back past 22 kHz. ON has to leave far less of it there,
and must not change how loud the note is (its fundamental: OFF's fold-back is loud enough to add to
the whole signal's level)."""
import cmath, math, os, struct, sys

D = sys.argv[1] if len(sys.argv) > 1 else os.path.join('build', 'host')
FS, N = 44100, 8192
F0 = 440 * 2 ** ((96 - 69) / 12)


def left(name):
    b = open(os.path.join(D, name), 'rb').read()
    d = b[44:]
    return [int.from_bytes(d[i:i + 3], 'little', signed=True) for i in range(0, len(d), 6)]


def fft(x):
    n = len(x)
    if n == 1:
        return x
    e, o = fft(x[0::2]), fft(x[1::2])
    t = [cmath.exp(-2j * math.pi * k / n) * o[k] for k in range(n // 2)]
    return [e[k] + t[k] for k in range(n // 2)] + [e[k] - t[k] for k in range(n // 2)]


def measure(x, t):
    a = int(t * FS)
    w = [x[a + i] * (0.42 - 0.5 * math.cos(2 * math.pi * i / (N - 1)) + 0.08 * math.cos(4 * math.pi * i / (N - 1)))
         for i in range(N)]
    P = [abs(c) ** 2 for c in fft(w)[:N // 2 + 1]]
    on = off = fund = 0.0
    for k, p in enumerate(P):
        f = k * FS / N
        if abs(f - round(f / F0) * F0) < 20:                # Blackman's main lobe is ~±16 Hz here
            on += p
            if abs(f - F0) < 20:
                fund += p
        else:
            off += p
    # the fundamental's level, not the whole signal's: OFF's fold-back is loud enough to add to that
    return 10 * math.log10(on / off), 10 * math.log10(fund) - 20 * math.log10(8388607 * N * 0.42 / 2)


x = left('alias-24.wav')
(son, lon), (soff, loff) = measure(x, 1.2), measure(x, 4.2)
print('88.2K: between the harmonics %5.1f dB under them, fundamental at %5.1f dBFS' % (son, lon))
print('44.1K: between the harmonics %5.1f dB under them, fundamental at %5.1f dBFS' % (soff, loff))
bad = []
if son < 30:
    bad.append('ON still leaves a lot between the harmonics')
if son - soff < 20:
    bad.append('ON is not clearly cleaner than OFF')
if abs(lon - loff) > 0.5:
    bad.append('ON changes how loud the note is')
for b in bad:
    print('FAIL:', b)
print('alias: %s' % ('FAIL' if bad else 'ok'))
sys.exit(1 if bad else 0)
