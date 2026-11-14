# FM Lab quick reference

Every control on one page. The [README](../README.md) says *why* each one works the way it does; this only
says *what* it does. Rule of thumb: **HOME** always gets you back, and the strip along the bottom of the screen
always names the four knobs.

## Knobs

**MIDI Learn:** hold **GLO** 400 ms. **SELECT** picks a slot; another **GLO** press advances it.
**PRESETS** picks a destination group; **ALGORITHM** picks its target.
Knobs 1..4 set **SOURCE / DEPTH / POLAR / BEND**.
**SEL** arms/cancels Learn; move a CC or channel-pressure controller.
**SAVE** clears only the source, retaining the other settings; **HOME** exits.
`*` means the global mapping is still waiting for a quiet settings save. See [MIDI.md](MIDI.md).

| Knob | Does |
| --- | --- |
| **PRESETS** | The voice: 20 factory voices, then user slots `U1-01`–`U8-32`. **SEL** held: a bank at a time. Changes under held notes. While saving: the slot. |
| **SELECT** | The next four things under the knobs, either way round — a page's next bank, HOME's next knob set, the next operator after a tap. While saving: which letter. On the step view: the step. |
| **ALGORITHM** | The algorithm, 1–32, from any page. While saving: the letter itself. |
| **Knobs 1–4** | Whatever the bottom strip says. A 0–99 value moves **5** a click. |
| **SEL** + a knob | **1** a click, for landing on a value (the strip goes lilac). On `RATIO`: every ratio the two bytes can make (`x1.01`, `x1.02`, …) instead of the list. |

## Buttons

| Button | Tap | Hold |
| --- | --- | --- |
| **HOME** | Back to HOME. On HOME: the next screen — algorithm → spectrum → scope | — |
| **EDIT** | On HOME: `LEVEL RATIO DETUNE WAVE` for one operator; again: next operator | The OPERATOR page; again: next bank |
| **ENV** | On HOME: `ATK DEC SUS REL` for one operator; again: next operator. On `GLOBAL` or `PITCH`: that envelope off / on | The ENVELOPE page; again: next bank |
| **LFO** | On HOME: this operator's `A M SENS`; again: next operator. On `GLOBAL`: the LFO's wave | The LFO page; again: next bank |
| **FX** | On HOME: the four effects' `MIX` | The EFFECTS page, one effect a bank; again: next effect |
| **GLO** | The GLOBAL page; again: next bank | MIDI assignments / Learn |
| **SEL** | Selected operator off / on | Solo it; hold again: all back on |
| **ARP** | Its knobs on HOME; again: arpeggiator on / off | — |
| **SEQ** | The looper's knobs on HOME | The step view |
| **REC** | Stopped: a bar count-in, then record one loop. Playing: overdub one loop. | Clear the phrase |
| **PLAY** | Play / stop the phrase | — |
| **SAVE** | Start saving: the slot on **PRESETS**, the name on **SELECT** + **ALGORITHM**. Again: write it. Anything else but **SEL** backs out. | — |
| **OCT-** / **OCT+** | Two octaves either way. Lit for a shift, pulses at the end. Hold **OCT-** while switching on: USB rescue. | — |

**SEL** acts on release, and only where there is a selected operator (a page, or a tap of EDIT/ENV/LFO). A press
used as the fine shift does nothing else.

## Black keys

The panel prints a name under some black keys. They only do that job where it makes sense; everywhere else
they are notes. A key that is doing its job glows.

| Key | Printed | Does | Where |
| --- | --- | --- | --- |
| F#3 G#3 A#3 C#4 D#4 F#4 | `OP1`–`OP6` | Pick that operator | OPERATOR and ENVELOPE pages, after a tap of EDIT/ENV/LFO, and on the `GLOBAL`/`PITCH` pictures (to go back) |
| G#4 | `PIT` | The pitch envelope: on HOME as `PITCH`, or the ENVELOPE page on it | Where the `OP` keys pick operators |
| A#4 | `GLOBAL` | The global amp envelope (`ENV GLOBAL`), or the whole LFO (`LFO GLOBAL`) | ENVELOPE page, after a tap of ENV or LFO |
| C#5 / D#5 | `MONO` / `POLY` | One voice, last note wins / eight voices | OPERATOR page |
| F#5 | | Tap: A/B compare, lit on B. Hold: copy this side onto the other. | Anywhere but plain HOME |

## HOME

Three screens, **HOME** steps through them. With nothing tapped, the knobs are:

| Screen | Knobs | **SELECT** |
| --- | --- | --- |
| Algorithm diagram | `LVL 1`–`LVL 4` | → `LVL 5 6` + `FEEDBK` → `RAT 1-4` → `RAT 5 6` + `FEEDBK` |
| Spectrum (`FILTER`) | `CUT RESO TYPE SPAN` — the filter | — |
| Scope | as the diagram | as the diagram |

A tap of **EDIT**, **ENV**, **LFO**, **FX**, **ARP** or **SEQ** borrows the knobs; the title says so
(`EDIT OP3`, `ENV GLOBAL`, `PITCH`, …). **HOME** gives them back.

## Pages

| Page | Banks (**SELECT** or the page's button walks them) |
| --- | --- |
| OPERATOR | `LEVEL RATIO DETUNE WAVE` · `MODE COARSE FINE V SENS` · `AMSENS RT SCL BRK PT L DEP` · `R DEP L CRV R CRV` |
| ENVELOPE | `ATTACK DECAY SUSTAIN RELEASE` · then the four segments, a rate and its level each. On `PITCH`: the pitch envelope's four. |
| LFO | `SPEED DELAY WAVE` · `P MOD A MOD PMSENS` · `K SYNC` |
| GLOBAL | `ALGO FEEDBK O SYNC TRANSP` · the pitch envelope's rates · its levels · `MIDICH PRG CH S RATE BITS` · `OUTPUT` (stereo / mono) · system diagnostics. The CPU meter is the bottom line; the firmware version is top right. |
| EFFECTS | `CHORUS` · `PHASER` · `DELAY` · `REVERB`, each `MIX` first; `MIX 0` is off |
| ARP (tap) | `TEMPO PATTERN OCTAVE LATCH` · `GATE RATE SWING VELO` |
| SEQ (tap) | `TEMPO BARS DIV CLICK` |
| Step view (hold SEQ) | `LENGTH TEMPO BARS DIV`. **SELECT** moves the cursor; a key adds or removes its note on that step. |

## System diagnostics

The final GLOBAL bank (**6/6**) shows app CPU/peak, active voices, LATE callbacks, sample rate
and build. **GLO** or **SELECT** walks to it; **SEL** requests peak/LATE reset on the next audio
callback, without muting/soloing or changing settings. The four parameter knobs are read-only.
At the recorded peak, **SYN** is voices/envelopes/downsampling, **FX** is effects, **CTL** is
controllers/notes/ARP/SEQ, and **POST** is filter/quality/analyser/output processing.
Percentages use the 256-frame 44.1 kHz deadline (~5.80 ms); they exclude other USB/platform/mastering
work. LATE counts app callbacks exceeding that deadline, not hardware underruns.
Host builds explicitly say **NOT DEVICE**; hardware headroom still needs testing on the instrument.

## What is kept where

| | Kept |
| --- | --- |
| **In the voice** (DX7 data; `*` while edited) | Everything on OPERATOR, ENVELOPE, LFO and GLOBAL's first three banks, the pitch envelope, operator waves. **SAVE** to a user slot; turn **PRESETS** off and back to throw an edit away. |
| **With the instrument** (saved by itself) | Octave, mono/poly, the global envelope and its switch, the pitch envelope's switch, filter, effects, spectrum span, MIDI channel, program change, sample rate and bit depth, stereo / mono output, arp and looper settings, the phrase. |
| **Not kept** | The operator switches and solo — changing voice brings all six back. |
