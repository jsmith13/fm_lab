# MIDI controls and Learn

Available in FM Lab **1.3.3**, released after the installed test build was reported working well.
Detailed device CPU/load qualification remains outstanding.
FM Lab and WT Lab use the same sixteen-slot global assignment editor.

## On the instrument

Hold **GLO** for 400 ms; a short press still opens GLOBAL.
**SELECT** chooses slot 1..16; pressing **GLO** again advances one slot and wraps after 16.
Changing slots cancels Learn without changing assignments.
The four bottom labels **SOURCE / DEPTH / POLAR / BEND** name knobs **1 / 2 / 3 / 4**:
source, depth (0..100%), polarity, and global pitch-wheel range (0..24 semitones, default +/-2).
**PRESETS** chooses a destination group; **ALGORITHM** chooses a target within that group.
The selected target is highlighted in the center of three or five visible choices.
Neighboring choices wrap at the ends, with no duplicated targets in the visible list.
Group changes select their first target; ALGORITHM wraps within the group.
Returning to an assignment shows the group containing its existing destination.
FM groups are PERFORM (including NONE), TONE, OP 1..6 (level/coarse/fine), LFO,
AMP ENV, CHORUS, PHASER, DELAY and REVERB. Each contains at least three choices.
Select a destination, tap **SEL**, then move a CC controller or apply channel pressure.
The source is learned; notes and pitch-wheel messages do not consume Learn.
**SEL** cancels Learn. **SAVE** clears only this slot's source, keeping destination/depth/polarity.
**HOME** exits; other page buttons also cancel Learn.

Mappings and bend range are global device settings, not patch fields. `*` means changes are
pending. They save automatically after two seconds without configuration edits, once notes
and effects are quiet; wait for `*` to disappear before powering off. A save failure is displayed.
Live controller movement never triggers a settings save.

The panel continues to show the underlying patch values. MIDI changes are temporary:
they do not overwrite the patch, A/B copy, or native bank. Turning a mapped physical parameter
fades its MIDI override back to the panel value; the next matching controller message reactivates
the assignment. Normal saving keeps native behavior: patch SAVE stores explicit patch edits,
while front-panel global edits use their existing settings save. Neither captures all live MIDI
values. There is no separate commit-performance-values gesture.

## Default assignments

| Slot | Source | Destination | Depth |
| --- | --- | --- | --- |
| 1 | CC1 | Vibrato | 12% |
| 2 | CC7 | Volume | 100% |
| 3 | CC11 | Expression | 100% |
| 4 | CC10 | Pan | 100% |
| 5 | CC74 | Cutoff | 100% |
| 6 | CC71 | Resonance | 100% |
| 7 | CC91 | Reverb mix | 100% |
| 8 | CC93 | Chorus mix | 100% |
| 9 | Channel pressure | Vibrato | 12% |
| 10 | Channel pressure | Brightness | 25% |
| 11..16 | OFF | NONE | 100% |

All default polarities are positive. A source can drive several destinations simultaneously.
Replace a default slot to change that controller's usual behavior; adding another slot does not
remove its default routing. CC0/32, CC64 and CC120..127 are reserved and cannot be assigned.
Reserved messages remain functional while Learn is armed, without consuming it.

CC1 and channel pressure add offsets to the saved value; zero adds nothing.
Vibrato, brightness and amplitude are also additive for other sources. Other ordinary CC routes
blend toward an absolute destination value according to depth/polarity. Multiple routes add
their offsets from the base, then clamp to the destination's native range.
Responses ramp over 20 ms; discrete/native quantization and synthesis pitch limits still apply.

Vibrato shares the existing patch LFO waveform/rate/key sync, and FM's LFO delay, adding independent
depth without replacing saved pitch modulation. Its 12% default is about 24 cents at full input.
FM brightness increases modulator gain; algorithms with only parallel carriers may not brighten.
WT brightness offsets both wavetable positions; the timbre change depends on the selected tables.
Cutoff affects sound only when a filter type other than OFF is selected.
Volume and expression multiply; their startup values are full, while wheel/pressure/bend start at zero.
CC10 value 64 is exact center; 0/127 are left/right. OUTPUT MONO folds both panned sides onto both outputs.

Common destinations include vibrato, amplitude, volume, expression, pan, cutoff, resonance,
brightness and all four effects' mixes. WT adds oscillator position/level/tuning/position-envelope,
both ADSRs, LFO rate/depth, filter tracking/envelope, velocity and effect parameters.
FM adds six operators' level/coarse/fine, feedback, native LFO rate/pitch/amplitude depth,
global ADSR, effect parameters and EG bias. To modulate native FM pitch depth with its saved
sensitivity instead of independent vibrato, assign LFO PITCH.
Operator mute switches remain authoritative. Filter OFF and FM global-envelope OFF stay off;
effect mix controllers can activate their effects without changing the saved mix.

## Fixed messages and channel policy

CC64 values 64..127 hold released notes; 0..63 release them through their normal envelopes.
Repeated pedal-held notes retrigger a logical voice. Released voices are stolen first, then
pedal-held voices, then physical held voices. Mono retains physical last-note fallback.
Program changes retain controller/pedal/mapping state without abandoning deferred releases.
Note-on velocity zero is note-off.

CC120 **All Sound Off** immediately silences voices and effect/filter/decimator/click history,
and stops generators. It preserves controller/pedal values; new notes still obey a held pedal.
CC123 **All Notes Off** releases note ownership and stops generators, honoring sustain and normal release.
CC121 **Reset All Controllers** releases sustain and resets wheel, pressure, bend and expression
without cutting physical held notes, changing program/mappings, or stopping transport.
It retains volume, pan, sound controllers 70..79 and effect controllers 91..95;
other ordinary routes return to the patch base. This is the supported reset policy, not full GM compliance.

Channel pressure is a separate D0..DF message, not a CC. Notes, CCs, pressure and bend obey
the receive-channel setting. ALL is one shared, single-timbral instrument, not sixteen independent
parts; incoming channels share note/controller state. Changing receive channel clears old notes,
pedal and expressive controllers so old-channel releases cannot strand them.
Polyphonic key pressure, RPN/NRPN bend-range programming and full GM controller behavior are not implemented.

## Storage and rollback

Native patch/bank formats are unchanged. Old settings load with default MIDI mappings.
Custom mappings/range require a larger settings record that older firmware cannot load.
Clearing slots alone does **not** restore defaults: destinations/depth/polarity remain.
To retain device preferences across a downgrade, restore every route exactly as the default
table above, including OFF/NONE/100%/positive for slots 11..16 and bend +/-2, then wait for quiet save.
That writes the legacy settings size again. WT's separate sample-rate rollback precautions still apply.
Keep the rollback firmware and saved banks. FM's `1.2.1` package is retained;
WT's user-designated last known-working baseline is `0.3.2-test`, with `0.2.7-test` retained as an older rollback.
