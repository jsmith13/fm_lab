#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run firmware/src/fmlab.c on this computer (tests/host.c): build/host/fmlab.wav and the screens as PNG.
#
#   tests/run_host.sh                       the built-in script
#   tests/run_host.sh tests/algorithms.txt  all 32 algorithms, drawn, as build/host/algorithms-*.png
#   tests/run_host.sh tests/operators.txt   the operator switches and the A/B compare
#   tests/run_host.sh tests/bank.txt        the bank, saving, and .syx in and out
#   tests/run_host.sh tests/panel.txt       the pages, the operator keys, and what they send
#   tests/run_host.sh tests/spectrum.txt    the analyser, against where the partials belong
#   tests/run_host.sh tests/home.txt        the four knobs HOME puts under your fingers
#   tests/run_host.sh tests/env.txt         ADSR, against the eight bytes it writes
#
# A script's output is named after it, so the runs do not overwrite each other.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
script="$1"
out=build/host/fmlab
if [ -n "$script" ]; then
    name=${script##*/}
    out=build/host/${name%.*}
fi
cc -O2 -Wall -Wno-unused-function -DDINGHY_HOST -DAPP_VER="\"${APP_VER:-DEV}\"" -Ifirmware/src -Ifirmware/gen -o build/host/fmlab firmware/src/fmlab.c
rm -f "$out"-*.ppm "$out"-*.png "$out"-bank.bin "$out"-out.syx   # the bank outlives a run, as flash does
case "$script" in
*/bank.txt|*/alias.txt) python3 tests/mksyx.py build/host ;; # the .syx files it sends itself
esac
build/host/fmlab "$out" $script > "$out".log   # the log is kept: panelcheck.py reads the MIDI out of it
cat "$out".log
case "$script" in                                           # while the screens are still PPMs
*/panel.txt) python3 tests/panelcheck.py "$out" ;;          # the pages and the keys that are not keys
*/spectrum.txt) python3 tests/speccheck.py "$out" ;;        # the analyser, against the arithmetic
*/levels.txt) python3 tests/levelcheck.py "$out" ;;         # the operator boxes, against the envelopes
*/edit.txt)   python3 tests/editcheck.py "$out" ;;          # RATIO, against the bytes underneath it
*/home.txt)   python3 tests/homecheck.py "$out" ;;          # HOME's knobs, against the bytes they move
*/env.txt)    python3 tests/envcheck.py "$out" ;;           # ADSR, against the segments underneath it
*/focus.txt)  python3 tests/focuscheck.py "$out" ;;         # a tap of EDIT or ENV, against a hold
*/fast.txt)   python3 tests/fastcheck.py "$out" ;;          # four clicks, against four with SEL held
*/corners.txt) python3 tests/cornercheck.py "$out" ;;       # the battery, and which HOME screen
  */global.txt)  python3 tests/globalcheck.py "$out" ;;        # one envelope over all six
  */lfo.txt)     python3 tests/lfocheck.py "$out" ;;           # a depth, and who is listening
  */fx.txt)      python3 tests/fxcheck.py "$out" ;;            # the filter and the effects' knobs
  */effects.txt) python3 tests/effectscheck.py "$out" ;;       # chorus, phaser, delay and reverb, heard
  */waves.txt)   python3 tests/wavescheck.py "$out" ;;         # the operator waves, heard, drawn and saved
  */scope.txt)   python3 tests/scopecheck.py "$out" ;;         # the waveform screen, by its trace
  */play.txt)    python3 tests/playcheck.py "$out" ;;          # the arpeggiator and the sequencer, heard
  */midi.txt)    python3 tests/midicheck.py "$out" ;;          # the channel, and program change
  */pitch.txt)   python3 tests/pitchcheck.py "$out" ;;         # the PIT key, and the pitch envelope, heard
esac
OUT="$out" python3 - <<'PY'
import os
from pathlib import Path
from PIL import Image
for p in sorted(Path("build/host").glob(Path(os.environ["OUT"]).name + "-*.ppm")):
    Image.open(p).resize((480, 480), Image.NEAREST).save(p.with_suffix(".png"))
    p.unlink()
    print("screen ->", p.with_suffix(".png"))
PY
case "$script" in
*/operators.txt) python3 tests/wavcheck.py "$out".wav ;;    # the switches have to agree with the sound
*/bank.txt) python3 tests/syxcheck.py "$out" ;;             # and what was stored with what was sent
*/depth.txt) python3 tests/depthcheck.py build/host ;;      # and the 24 bits the DAC is given
*/alias.txt) python3 tests/aliascheck.py build/host ;;      # and 88.2K, against what folds back
*/bits.txt)  python3 tests/bitscheck.py build/host ;;       # and BIT DEPTH, against its grid
*/mono.txt)  python3 tests/monocheck.py build/host ;;       # and OUTPUT, stereo and mono
*/srate.txt) python3 tests/sratecheck.py build/host ;;      # and SAMPLE RATE, held at each step
esac
