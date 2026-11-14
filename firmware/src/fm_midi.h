/* SPDX-License-Identifier: GPL-3.0-only */
static const char *const mi_extra_names[] = {
    "OP1 LEVEL","OP2 LEVEL","OP3 LEVEL","OP4 LEVEL","OP5 LEVEL","OP6 LEVEL",
    "OP1 COARSE","OP2 COARSE","OP3 COARSE","OP4 COARSE","OP5 COARSE","OP6 COARSE",
    "OP1 FINE","OP2 FINE","OP3 FINE","OP4 FINE","OP5 FINE","OP6 FINE",
    "FEEDBACK","LFO RATE","LFO PITCH","LFO AMP",
    "AMP ATTACK","AMP DECAY","AMP SUSTAIN","AMP RELEASE",
    "CHO RATE","CHO DEPTH","CHO WIDTH","PHA RATE","PHA DEPTH","PHA FEEDBACK",
    "DLY TIME","DLY FEEDBACK","DLY TONE","REV DECAY","REV DAMP","REV PRE","EG BIAS"
};
#define MI_TARGET_COUNT (13u+sizeof mi_extra_names/sizeof mi_extra_names[0])
static const char *const mi_group_names[] = {
    "PERFORM","TONE","OP 1","OP 2","OP 3","OP 4","OP 5","OP 6",
    "LFO","AMP ENV","CHORUS","PHASER","DELAY","REVERB"
};
/* First byte is the count; destination IDs retain their stored MIDI meaning. */
static const uint8_t mi_groups[][8] = {
    {6,0,1,2,3,4,5}, {5,6,7,8,31,51},
    {3,13,19,25}, {3,14,20,26}, {3,15,21,27},
    {3,16,22,28}, {3,17,23,29}, {3,18,24,30},
    {3,32,33,34}, {4,35,36,37,38},
    {4,10,39,40,41}, {4,12,42,43,44}, {4,11,45,46,47}, {4,9,48,49,50}
};
#include "midi_control.h"
static const uint8_t fm_midi_fx_targets[16] = {MI_CHORUS,39,40,41,MI_PHASER,42,43,44,
                                             MI_DELAY,45,46,47,MI_REVERB,48,49,50};
static uint8_t midi_voice[DX_VSZ], midi_voice_seen[DX_VSZ];
static int32_t midi_brightness, midi_vibrato, midi_bias;
