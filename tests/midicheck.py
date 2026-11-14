#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check a tests/midi.txt run: the MIDI channel and program change, on GLOBAL's fourth bank.

    python3 tests/midicheck.py build/host/midi

Heard: a note on any channel while the channel is ALL, then only on the one picked. Sent: the
keys' notes on that channel, and a program change when PRESETS turns -- only with PROG CHG on.
Seen: the bank, its value moving, and the voice changing for a program on the right channel only.
"""
import re
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


def main(stem):
    stem = Path(stem)
    wav = stem.with_name(stem.name + ".wav").read_bytes()
    pcm = wav[wav.index(b"data") + 8:]
    L = struct.unpack("<%dh" % (len(pcm) // 2), pcm)[0::2]

    def rms(t0, t1):
        v = L[int(FS * t0 / 1000):int(FS * t1 / 1000)]
        return (sum(float(s) * s for s in v) / max(1, len(v))) ** 0.5

    def shot(ms):
        return stem.with_name(stem.name + f"-{ms}.ppm").read_bytes()

    def stable_shot(ms):
        pixels = shot(ms).split(b"\n", 1)[1]
        # Ignore the live status indicator at the bottom right, not the voice/parameter display.
        return b"".join(pixels[y * 720:y * 720 + (576 if y >= 224 else 720)] for y in range(240))

    out = [(int(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4)))
           for m in re.finditer(r"^\s*(\d+) ms  MIDI out (\w\w) (\d+) (\d+)",
                                stem.with_name(stem.name + ".log").read_text(), re.M)]

    check("ALL: channel 3 is heard", rms(450, 800) > 200, f"rms {rms(450, 800):.0f}")
    check("GLOBAL has a fourth bank", shot(2200) != shot(1700))
    check("turning MIDI CH shows it", shot(2800) != shot(2200))
    check("channel 3: channel 1 is not heard", rms(3050, 3400) < 5, f"rms {rms(3050, 3400):.0f}")
    check("..and channel 3 is", rms(3850, 4200) > 200, f"rms {rms(3850, 4200):.0f}")
    keys = [o for o in out if 4500 <= o[0] <= 4900]
    check("a key sends on channel 3", [o[1] for o in keys] == ["92", "82"], str(keys))
    check("program 5 on channel 3 changes the voice", shot(5800) != shot(5200))
    check("program 3 on channel 1 does not", stable_shot(6400) == stable_shot(5800))
    pcs = [o for o in out if o[1][0] in "BC"]
    # voice 6 from program 5, one click to 7 unheard, one to 8 sent as program 7 -- had program 9
    # been taken while OFF, it would say 11. Then bank 3 program 4 is U3-05, one on is U3-06.
    check("PRESETS sends a program only with PROG CHG on",
          pcs[:2] == [(8000, "B2", 0, 0), (8000, "C2", 7, 0)], str(pcs[:2]))
    check("bank select 3, program 4 is U3-05", pcs[2:] == [(9000, "B2", 0, 3), (9000, "C2", 5, 0)],
          str(pcs[2:]))
    check("bank select 0, program 2 changes the voice", shot(9800) != shot(8800))

    print(f"\n{bad} checks failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "build/host/midi"))
