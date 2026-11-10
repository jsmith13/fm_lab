#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check a tests/mono.txt run: OUTPUT, STEREO and MONO.

    python3 tests/monocheck.py build/host

In stereo the chorus puts something different on each side. In mono both sides must be the same
sample for sample -- a mono plug hears only the left, so the left has to be all of it -- and the
note must be about as loud as it was, not doubled and not halved.
"""
import struct
import sys
from pathlib import Path

FS = 44100
bad = 0


def check(what, ok, note=""):
    global bad
    if not ok:
        bad += 1
    print(f"{what:<52} {'ok' if ok else 'WRONG':<5} {note}")


def main(d):
    d = Path(d)
    wav = (d / "mono.wav").read_bytes()
    pcm = wav[wav.index(b"data") + 8:]
    s = struct.unpack("<%dh" % (len(pcm) // 2), pcm)
    L, R = s[0::2], s[1::2]

    def win(t0, t1):
        a, b = int(FS * t0 / 1000), int(FS * t1 / 1000)
        return L[a:b], R[a:b]

    def rms(v):
        return (sum(float(x) * x for x in v) / max(1, len(v))) ** 0.5

    sl, sr = win(1200, 1900)
    ml, mr = win(4200, 4900)
    diff = rms([a - b for a, b in zip(sl, sr)])
    check("stereo: the chorus parts the two sides", diff > 100, f"rms of L-R {diff:.0f}")
    check("mono: left and right are the same", ml == mr)
    st, mo = (rms(sl) + rms(sr)) / 2, rms(ml)
    check("mono: about as loud as stereo", 0.6 * st < mo < 1.2 * st, f"{mo:.0f} against {st:.0f}")
    def shot(ms):                       # the .sh runner turns them into .png, the .ps1 does not
        p = d / f"mono-{ms}.ppm"
        return (p if p.exists() else p.with_suffix(".png")).read_bytes()

    check("the OUTPUT knob changes the screen", shot(3400) != shot(3800))

    print(f"\n{bad} checks failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "build/host"))
