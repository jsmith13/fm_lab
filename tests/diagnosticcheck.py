#!/usr/bin/env python3
"""GLOBAL diagnostics navigation, read-only controls and non-destructive reset."""
import math
import struct
import sys
import wave
from pathlib import Path

out = Path(sys.argv[1])
bad = 0


def check(name, good):
    global bad
    bad += not good
    print(f"{name}: {'ok' if good else 'FAIL'}")


def row(ms, y, height=16):
    header, data = Path(f"{out}-{ms}.ppm").read_bytes().split(b"\n", 1)
    assert header.split() == [b"P6", b"240", b"240", b"255"]
    return data[y * 720:(y + height) * 720]


check("final GLOBAL bank renders diagnostics instead of editable parameter rows",
      row(550, 24, 160) != row(800, 24, 160)
      and all(any(row(800, y)) for y in (44, 62, 80, 98, 120, 136, 152, 168)))
check("read-only knobs and SEL hold do not change diagnostic values",
      row(800, 44, 140) == row(1300, 44, 140) == row(2000, 44, 140))
check("SEL tap and hold both report a timing reset",
      row(800, 188) != row(1000, 188) and row(1000, 188) == row(2000, 188))
check("GLO wraps to the first bank and SELECT returns to the same diagnostic page",
      row(550, 24, 160) == row(2300, 24, 160)
      and row(800, 24, 160) == row(2600, 24, 160))
check("turning a read-only knob under SEL suppresses the reset gesture",
      row(2600, 188) != row(4050, 188) and row(4050, 188) == row(800, 188))
check("host timing disclaimer stays stable during reset",
      row(800, 224) == row(1000, 224) == row(2000, 224))
check("HOME leaves diagnostics cleanly", row(4200, 44, 140) != row(800, 44, 140))

with wave.open(str(out) + ".wav") as audio:
    rate = audio.getframerate()
    assert audio.getnchannels() == 2 and audio.getsampwidth() == 2
    raw = audio.readframes(audio.getnframes())
samples = struct.unpack("<%dh" % (len(raw) // 2), raw)


def segment(start, end):
    return samples[2 * int(start * rate):2 * int(end * rate):2]


def rms(start, end):
    x = segment(start, end)
    return math.sqrt(sum(v * v for v in x) / len(x))


reference = rms(.4, .48)
check("diagnostics, both resets and read-only knobs preserve the held voice",
      reference > 100 and all(abs(rms(a, b) / reference - 1) < .03
                             for a, b in ((1.05, 1.09), (1.3, 1.4), (1.95, 2.05),
                                          (2.3, 2.4), (4.01, 4.1))))
x = segment(2.3, 2.4)
crossings = [i + (-x[i]) / (x[i + 1] - x[i]) for i in range(len(x) - 1)
             if x[i] <= 0 < x[i + 1]]
frequency = (len(crossings) - 1) * rate / (crossings[-1] - crossings[0]) if len(crossings) > 1 else 0
check("ignored diagnostic knob turns do not leak into later settings", abs(frequency - 440) < 1)
check("normal note-off still releases after leaving diagnostics", rms(4.6, 4.69) < 2)
sys.exit(1 if bad else 0)
