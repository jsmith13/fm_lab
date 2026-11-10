# SPDX-License-Identifier: GPL-3.0-only
"""SAMPLE RATE, heard: build/host/srate-24.wav is one note at 88.2K, 44.1K, 32K, 22K, 16K, 11K and 8K.
Under 44.1K the voices are held: a sample is the one before it again for all but `rate` of every
44100, so the share of repeats says the rate. At 44.1K and up nothing repeats. The note must keep
its level at every rate, and the knob must stop at 8K."""
import os, sys

D = sys.argv[1] if len(sys.argv) > 1 else os.path.join('build', 'host')
FS = 44100
STEPS = [('88.2K', 1.2, 44100), ('44.1K', 2.8, 44100), ('32K', 4.4, 32000), ('22K', 6.0, 22050),
         ('16K', 7.6, 16000), ('11K', 9.2, 11025), ('8K', 10.8, 8000)]


def left(name):
    d = open(os.path.join(D, name), 'rb').read()[44:]
    return [int.from_bytes(d[i:i + 3], 'little', signed=True) for i in range(0, len(d), 6)]


def shot(ms):                           # the .sh runner turns them into .png, the .ps1 does not
    p = os.path.join(D, 'srate-%d.ppm' % ms)
    return open(p if os.path.exists(p) else p[:-4] + '.png', 'rb').read()


x = left('srate-24.wav')
bad = []
ref = None
for name, t, hz in STEPS:
    a = int((t + 0.2) * FS)
    w = x[a:a + FS // 2]
    rep = sum(1 for i in range(1, len(w)) if w[i] == w[i - 1]) / (len(w) - 1)
    rms = (sum(float(v) * v for v in w) / len(w)) ** 0.5
    want = 1 - hz / FS
    ref = ref or rms
    print('%-6s repeats %.3f (want %.3f)  level %.2f of 88.2K' % (name, rep, want, rms / ref))
    if abs(rep - want) > 0.02:
        bad.append('%s is not held at its rate' % name)
    if not 0.7 < rms / ref < 1.3:
        bad.append('%s changes how loud the note is' % name)
if shot(1000) == shot(10700):
    bad.append('the knob does not change the screen')
if shot(10700) != shot(12400):
    bad.append('the knob goes past 8K')
for b in bad:
    print('FAIL:', b)
print('srate: %s' % ('FAIL' if bad else 'ok'))
sys.exit(1 if bad else 0)
