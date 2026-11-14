#!/usr/bin/env python3
"""Actual controller-editor gestures, saved-state redraws and stereo MIDI audio."""
import math
import struct
import sys
import wave
from pathlib import Path

bad = 0
out = Path(sys.argv[1])


def check(name, good):
    global bad
    bad += not good
    print(f"{name}: {'ok' if good else 'FAIL'}")


def row(ms, y, height=16):
    header, data = Path(f"{out}-{ms}.ppm").read_bytes().split(b"\n", 1)
    assert header.split() == [b"P6", b"240", b"240", b"255"]
    return data[y * 240 * 3:(y + height) * 240 * 3]

def cell(ms, knob):
    data = row(ms, 176, 32)
    return b"".join(data[y * 720 + knob * 180:y * 720 + (knob + 1) * 180]
                    for y in range(32))

check("reserved sustain does not consume Learn", cell(2100, 0) == cell(2295, 0)
      and row(2100, 224) != row(2295, 224))
check("Learn changes source, not the selected destination", cell(2100, 0) != cell(2400, 0)
      and row(2100, 38, 114) == row(2400, 38, 114))
check("depth, polarity and bend have independent knob cells",
      all(cell(2800, knob) != cell(3500, knob) for knob in (1, 2, 3))
      and cell(2800, 0) == cell(3500, 0) and row(2800, 38, 114) == row(3500, 38, 114))
check("quiet save removes pending marker without changing assignments",
      row(3500, 18) != row(5600, 18) and row(3500, 38, 202) == row(5600, 38, 202))
check("SAVE clears source, not destination/depth/polarity/range",
      cell(3500, 0) != cell(12300, 0) and row(3500, 38, 114) == row(12300, 38, 114)
      and all(cell(3500, knob) == cell(12300, knob) for knob in (1, 2, 3)))
check("cleared assignment also persists after quiet save",
      row(12300, 18) != row(14500, 18) and row(12300, 38, 202) == row(14500, 38, 202))
check("PRESETS and ALGORITHM navigate groups/targets without changing the four knob values",
      row(14500, 38, 114) != row(14750, 38, 114)
      and row(14750, 38, 114) != row(14850, 38, 114)
      and row(14850, 38, 114) != row(14950, 38, 114)
      and row(14950, 38, 114) != row(15050, 38, 114)
      and all(cell(14500, knob) == cell(15050, knob) for knob in range(4)))
check("centered TONE selector shows all five distinct target rows",
      all(any(row(15050, y)) for y in (58, 76, 94, 112, 130)))
check("navigation hint removed and lowered bars sit just above the labels",
      not any(row(3500, 152, 24)) and any(row(3500, 202, 4))
      and not any(row(3500, 206, 2)) and any(row(3500, 208)))
check("holding GLO pages once and SELECT returns to the same assignment",
      row(15050, 18) != row(15800, 18)
      and row(15050, 18, 222) == row(15950, 18, 222))

with wave.open(str(out) + ".wav") as audio:
    assert audio.getnchannels() == 2 and audio.getsampwidth() == 2
    rate = audio.getframerate()
    raw = audio.readframes(audio.getnframes())
samples = struct.unpack("<%dh" % (len(raw) // 2), raw)


def segment(start, end, channel=0):
    return samples[2 * int(start * rate) + channel:2 * int(end * rate):2]


def rms(start, end, channel=0):
    x = segment(start, end, channel)
    return math.sqrt(sum(v * v for v in x) / len(x))


def frequency(start, end):
    x = segment(start, end)
    crossings = [i + (-x[i]) / (x[i + 1] - x[i]) for i in range(len(x) - 1)
                 if x[i] <= 0 < x[i + 1]]
    return (len(crossings) - 1) * rate / (crossings[-1] - crossings[0])


check("released key sustains while pedal is down", rms(.96, 1.15) > 100)
check("pedal up enters normal release", rms(1.42, 1.48) < 2)
check("zero expression smoothly silences a held voice", rms(6.8, 7.0) < 2)
check("expression restores the same held voice", rms(7.2, 7.4) > 100)
check("edited seven-semitone bend reaches its pitch", abs(frequency(7.6, 7.9) - 440 * 2 ** (7 / 12)) < 1)
check("bend center restores exact pitch", abs(frequency(8.1, 8.4) - 440) < 1)
check("pan reaches left-only without muting left", rms(8.6, 8.9) > 100 and rms(8.6, 8.9, 1) < 2)
check("All Sound Off immediately clears pedal-held audio and tails",
      rms(10.02, 10.3) < 2 and rms(10.02, 10.3, 1) < 2)
check("Reset All Controllers releases preserved pedal for subsequent notes", rms(11.42, 11.49) < 2)

log = out.with_suffix(".log").read_text()
if out.name == "controllers":
    check("editor group/target navigation never sends a program change", log.count("MIDI out C") == 1)
    check("live controllers do not autosave performance values", log.count("settings saved (316 B)") == 2)
sys.exit(1 if bad else 0)
