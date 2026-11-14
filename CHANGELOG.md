# Changelog

FM Lab's releases, newest first. Each one is on the
[releases page](https://github.com/jsmith13/fm_lab/releases) with its `.fwsc`; the version a build is shows in
GLOBAL's top-right corner. Versions are `MAJOR.MINOR.PATCH`: a patch fixes what was there, a minor adds to it.

## 1.3.3 — 2026-11-14

The installed `1.3.3-test` was reported working well and approved for release on 2026-11-14.
The release uses the same on-screen version (`1.3.3`) and device identity (`FM-1_973`).
Detailed CPU/load qualification and validation on the newer flash chip remain outstanding.

### Added

- WT-style system diagnostics at the end of GLOBAL (6/6): app CPU/peak, peak-callback
  SYN/FX/CTL/POST breakdown, voices, LATE, sample rate and build. SEL requests audio-owned
  peak/LATE reset rather than mute/solo; parameter knobs are read-only. Host timing is marked
  NOT DEVICE; existing overload protection and patch/settings formats are unchanged.

- Shared CC assignment layout: envelope-style grouped target selector, PRESETS for group,
  ALGORITHM for target, SOURCE / DEPTH / POLAR / BEND bottom labels and GLO slot paging.
  Existing global mappings and stored destination IDs are unchanged.

- Sustain pedal, smooth wheel/expression, adjustable pitch bend (default +/-2 semitones),
  channel pressure, standard volume/pan/filter/effects CCs, and distinct panic/reset/note-release messages.
- Sixteen globally stored CC/pressure routes, depth/polarity and on-device Learn via held GLO.
  Physical parameter edits regain control; the next controller message reactivates the route.
  Live values never overwrite native patch data. Legacy settings and patch banks remain readable.
- Sustain-aware repeated notes/stealing/mono fallback, channel-change/overflow recovery, temporary
  FM brightness/EG bias, and effect activation without clearing large buffers in the audio interrupt.
- Settings write/readback verification and quiet-tail saving.

### Changed

- Lowered the assignment editor's four knob values/bars toward their bottom labels and removed
  redundant navigation hints.
- Physical parameter edits and live controllers share explicit ownership without changing
  native patch/bank formats. Custom MIDI settings require restoring all route fields and bend
  range to defaults before downgrading; see [MIDI storage and rollback](docs/MIDI.md#storage-and-rollback).

## 1.2.1 — 2026-11-09

### Fixed
- Support the newer UCUN UC25HQ80 flash chip (`0xB36014`) alongside the Puya P25Q80H (`0x856014`) for
  settings, user voices, USB rescue and firmware updates. The storage layout is unchanged.
- Wait up to 2 seconds for a sector erase and 50 milliseconds for a page write. A completed slow operation
  succeeds; a part still busy at the limit is reset, then the FM-1 restarts without resuming execution from
  busy flash. Updates feed the watchdog between sector erases. Unknown chips are still refused, including
  during startup cleanup.

## 1.2.0 — 2026-11-09

### Added
- **256 user slots**, as eight banks of 32: `U1-01` to `U8-32`. Bank `U1` is where it was, so the voices
  you have already saved are still there. Hold **SEL** while turning **PRESETS** to jump a bank at a time,
  in the voice list and while picking a slot to save to.
- Bank Select (CC 0) with program change: bank 1–8 and program *N* is slot *N*+1 of `U1`–`U8`; bank 0,
  or none, is the voice list as before. **PRESETS** sends the bank before the program.
- A bulk dump in goes into the bank you are in, and a bank request sends that bank back; the screen
  says which (`BANK 2 RECEIVED`).
- **OUTPUT** (GLOBAL, fifth bank): `STEREO`, or `MONO` for a mono 3.5 mm plug, with left and right summed
  onto both sides so nothing the effects put on the right is lost.

### Changed
- **OVERSAMPLE is now SAMPLE RATE** (GLOBAL, fourth bank): `88.2K` (oversampled, as `ON` was, and where it
  starts), `44.1K` (as `OFF` was), then `32K`, `22K`, `16K`, `11K` and `8K`, which hold the voices at that
  rate, aliasing and all, for the sound of older hardware. A saved `OFF` comes back as `44.1K`.

## 1.1.0 — 2026-10-18

### Added
- **OVERSAMPLE** (GLOBAL, last bank, `ON` to start). The operators run at 88.2 kHz and a half-band filter
  brings the sound back to 44.1 kHz, so the harmonics FM makes above 22 kHz no longer fold back into the
  audible range as tones that don't belong to the note: about 40 dB less of them on a bright patch played
  high. It roughly doubles the engine's CPU: ~72 % (82 % peak) with eight voices, the filter and every
  effect on. `OFF` gives that back.
- **BIT DEPTH** (GLOBAL, last bank): the voices cut to 16, 12 or 8 bits before the filter and the effects,
  for the sound of older hardware. It starts at `24 BIT`, which is FM Lab as before.

### Changed
- **24-bit output.** The sound used to be cut to 16 bits right after the voices were mixed, and only then
  sent to the 24-bit DAC. The mix leaves room for an 8-voice chord, so a single soft note used only ~11 of
  those 16 bits, and its decay ran out of steps and turned gritty (the DC blocker's rounding added a
  distortion that followed the note). The mix, the filter, the effects and MASTER now carry 24 bits to the
  DAC. USB audio stays 16-bit and is dithered. The delay's echoes are still stored at 16 bits, which saves
  128 KiB of RAM.

## 1.0.1 — 2026-10-18

### Fixed
- **The 3.5 mm MIDI IN jack works.** It was switched off in the platform FM Lab is built on, so a keyboard
  or pad controller on the jack played nothing; only USB-MIDI was heard. The jack now feeds the same input
  as USB — the GLOBAL `MIDI CHANNEL` filter and `PROG CHANGE` apply to both. It takes notes and program
  changes; SysEx and MIDI out stay on USB. Through a TRS-to-DIN adapter: if nothing plays, try the other
  type (A / B). The reader is Felucca's, as dxsloop carries it.

## 1.0.0 — 2026-10-18

The first release: a six-operator FM synthesizer for the M-VAVE FM-1, built to learn FM on.

- The DX7 engine (Dexed's, by way of dxsloop): 8 voices, 32 algorithms, feedback, per-operator envelopes,
  keyboard scaling, the LFO and the pitch envelope
- The algorithm drawn live, a spectrum analyser and a scope, and envelopes drawn to scale
- Every parameter under a knob with the sound playing; operator mute, solo and an A/B compare
- Operator waves, a global amp envelope, a filter, and chorus, phaser, delay and reverb
- A small arpeggiator and a one-phrase looper with a step view
- 20 factory voices and 32 user slots; DX7 SysEx in and out; MIDI channel and program change
- The firmware version on the GLOBAL page
