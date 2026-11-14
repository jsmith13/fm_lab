/* SPDX-License-Identifier: GPL-3.0-only */
/* FM Lab: a six-operator FM synth for the M-VAVE FM-1, to learn FM sound design on.
 *
 * The engine is Dexed's msfa core as sloopDX ported it to integer C (dx7_core.c): a real DX7 voice,
 * 32 algorithms, six operators with their envelopes, the pitch envelope and the LFO. This file is the
 * instrument around it. platform.c underneath is Dinghy's, untouched.
 *
 *   keys         play, 8 voices; the notes go out over USB-MIDI too
 *   OCT- OCT+    the octave (kept in flash)
 *   PRESETS      the voice
 *   MASTER       the volume (platform.c)
 *   USB-MIDI     notes in play, on any channel or the one GLOBAL's MIDI bank picks
 *
 * Editing, which is the point of the thing. Every knob takes effect under the notes you are already
 * holding -- dx_update is Dexed's Dx7Note::update, so the timbre changes without restarting the
 * envelopes or the phases. Turn an operator's level while a chord rings and you hear exactly what
 * that operator was doing.
 *
 *   HOME         the algorithm, drawn, with each operator lit by how hard it is working
 *   EDIT         the selected operator: level, ratio, detune, sensitivities, keyboard scaling
 *   ENV          the selected operator's envelope, drawn to scale, a segment at a time
 *   LFO          speed, delay, pitch and amplitude depth, wave
 *   GLO          algorithm, feedback, key sync, transpose, the pitch envelope
 *   SELECT       the next bank on the page, either way (the knobs are KNOB1.., left to right)
 *   OP1..OP6     the black keys: which operator the EDIT and ENV pages show, wherever one is shown
 *   ALGORITHM    the algorithm, on any page
 *   SAVE         write this sound into one of your 32 slots: press, pick the slot, press again
 *   FX           chorus, phaser, delay and reverb, on the finished sound
 *
 * Keeping things. PRESETS runs over the 20 built-in voices and then 32 slots of your own in flash.
 * An unused slot holds INIT VOICE -- a single operator at full level on algorithm 1, which is to
 * say a sine wave, and the honest place to start building something from.
 *
 * Patches go in and out in the DX7's own .syx format, so thirty years of other people's voices are
 * a MIDI cable away and reading them is most of how anyone learns this. A 32-voice bulk dump
 * replaces the whole bank; a single-voice dump lands in the editor, under whatever you are playing,
 * and you decide whether to keep it. The host can ask for either back (tools/syx.py).
 *
 * Listening, which is the other half of it. You cannot hear what an operator does by reading its
 * numbers; you hear it by taking it away and putting it back.
 *
 *   SEL          tap: switch the selected operator off, and on again
 *   SEL (hold)   solo it: every other operator off, and again for all six back on
 *   F#5          tap: the other side of the A/B compare; hold: copy this side onto the other one
 *
 * And something playing while you do it, so both hands are free for the knobs:
 *
 *   ARP          TEMPO, PATTERN, OCTAVES and LATCH on HOME's knobs; again, the arpeggiator on and off
 *   SEQ          TEMPO, BARS, DIV (what a note snaps to) and CLICK on HOME's knobs
 *   REC          a bar of count-in, then one time round the loop, recorded; while it plays, over it;
 *                held, the phrase is cleared
 *   PLAY         the phrase, round and round, and again to stop
 *
 * The switches are not part of the voice -- a DX7's were front-panel switches, not voice data -- so
 * they are applied on the way to the ISR and `edit` never carries them. Soloing a modulator is
 * silent, because a modulator is never heard, only felt: the output bus in the diagram turns red to
 * say so. That is the whole carrier-and-modulator distinction in one button.
 *
 * The two drawings are read out of the engine rather than kept alongside it: the diagram comes from
 * DX_ALG, the table dx_compute_ext obeys, and the envelope from the same arithmetic dx_env_advance
 * and dx_env_sample use. Neither can drift out of agreement with what you are hearing.
 *
 * Rules of the road (the chip has no FPU and no libgcc): integers only, no float, no 64-bit division.
 * tests/host.c runs this file on a computer: the screen as a PNG, the sound as a WAV.
 *
 * Who owns what, between the main loop and the audio ISR:
 *   the ISR owns dxv[], vst[] and the render. It never touches flash or the screen.
 *   the main loop owns the panel, the screen and the patch buffers. It speaks to the ISR only through
 *   the event ring (ev_push) and the published patch index (pcur), so no large struct is ever written
 *   while the ISR is reading it.
 */
#include "platform.c"

#define DX_LG_N 6                               /* 64-sample blocks: as Dexed renders, 4 per audio ISR */
#include "dx7_voice.h"                          /* first: the core reads the wave bytes it lays out */
#include "fm_midi.h"
#include "dx7_core.c"
#include "dx7_voices.h"
#include "spectrum.h"
#include <stddef.h>

#define NV 8u                                   /* voices (RAM: 428 B each) */

/* ---- the patch. `edit` is the main loop's own copy and the one the knobs change; pbuf is the pair
 * the ISR reads from. A change goes: edit -> the spare pbuf -> publish -> flip -> tell the ISR. The
 * ISR never sees a half-written patch, and nothing large is ever written under it. */
static uint8_t edit[DX_VSZ];
static uint8_t pbuf[2][DX_VSZ];
static volatile uint8_t pcur;                   /* the ISR reads pbuf[pcur] */
static uint8_t modified;                        /* the voice differs from the factory one */

/* The operator switches: bit op (0 = OP6 .. 5 = OP1) means that operator is switched off. They are
 * a way of listening, not part of the sound -- a DX7's were front-panel switches, not voice data --
 * so they live outside `edit`, are not saved, and survive an A/B switch. */
static uint8_t opmute;

/* A/B: two patches, one of them in `edit` and the other stashed here, with the switch swapping
 * them. Land on a sound, copy it across, change one thing, and flip back and forth: hearing the
 * same parameter two ways in succession is how you find out what it does. */
static uint8_t other[DX_VSZ];
static uint8_t ab;                              /* which side `edit` is: 0 = A, 1 = B */
static uint8_t other_mod;
static uint16_t other_voice;

/* ---- the bank: 256 slots of your own in flash, as eight banks of 32, each kept in the DX7's packed
 * 128-byte form, which is exactly what a .syx carries. A bank in RAM, a bank in flash and a bulk dump
 * on the wire are then all the same 4096 bytes; bankmsg wraps one bank in its SysEx on the way out.
 *
 * An unwritten slot holds INIT VOICE rather than being marked empty, which is both less state and
 * truer to the instrument: a slot always holds a voice, and until you put something in it that
 * voice is a sine wave.
 *
 * The voice knob runs over the 20 built-in voices and then your 256: 0..19 factory, 20..275 slots,
 * shown as U1-01 .. U8-32. */
#define BANK_V 32u                              /* voices in a bank: a DX7 bulk dump */
#define NBANK 8u
#define NUSER (NBANK * BANK_V)
#define NVOICE (DX_NVOICES + NUSER)

static uint8_t bankv[NUSER * 128u];             /* the eight banks, one after the other: 32 KiB */
static uint8_t bankmsg[6u + BANK_V * 128u + 2u];  /* F0 43 00 09 20 00 <4096> sum F7: one bank, going out */

/* what the voice line says instead of the voice, while saving or just after something arrived */
static uint8_t saving, save_slot;
static const char *flash_msg;
static uint32_t flash_at, msg_gen;

static void say(const char *m)                  /* a word on the voice line for a second and a half */
{
    flash_msg = m;
    flash_at = fm1_ms;
    msg_gen++;
}

static void bank_init(void)
{
    uint8_t u[DX_VSZ];
    uint32_t i, b;
    bankmsg[0] = 0xF0; bankmsg[1] = 0x43; bankmsg[2] = 0x00;
    bankmsg[3] = 0x09; bankmsg[4] = 0x20; bankmsg[5] = 0x00;   /* 32-voice bulk dump, 4096 bytes */
    bankmsg[sizeof bankmsg - 1u] = 0xF7;
    dx_init_voice(u);
    for (b = 0; b < NBANK; b++) {
        uint8_t *p = bankv + b * BANK_V * 128u;
        if (bank_read(b, p, BANK_V * 128u) == 0)
            continue;
        for (i = 0; i < BANK_V; i++)            /* no bank yet, or a cut write: 32 sine waves */
            dx_pack(u, p + i * 128u);
    }
}

static int bank_put(uint32_t b)                 /* one bank, RAM to flash. 0: written */
{
    return bank_write(b, bankv + b * BANK_V * 128u, BANK_V * 128u);
}

static int bank_store(uint32_t slot, const uint8_t *u)   /* 0: written */
{
    dx_pack(u, bankv + slot * 128u);
    return bank_put(slot / BANK_V);
}

/* a slot as the screen says it: U, its bank, a dash, its place in the bank -- U1-01 .. U8-32 */
static uint32_t put_slot(char *s, uint32_t i, uint32_t slot)
{
    s[i++] = 'U';
    s[i++] = (char)('1' + slot / BANK_V);
    s[i++] = '-';
    s[i++] = (char)('0' + (slot % BANK_V + 1u) / 10u);
    s[i++] = (char)('0' + (slot % BANK_V + 1u) % 10u);
    return i;
}

/* the ten characters a voice calls itself, printable and without the spaces a DX7 pads with */
static void voice_name(uint32_t vi, char *out)
{
    uint32_t i, n = 0;
    if (vi < DX_NVOICES) {
        const char *s = DX_VOICE_NAME[vi];
        while (n < 10u && s[n]) { out[n] = s[n]; n++; }
    } else {
        const uint8_t *b = bankv + (vi - DX_NVOICES) * 128u + 118u;
        for (i = 0; i < 10u; i++)
            out[i] = b[i] >= 32u && b[i] < 127u ? (char)b[i] : ' ';
        n = 10u;
    }
    while (n && out[n - 1u] == ' ')
        n--;
    out[n] = 0;
}

/* ---- naming a sound
 *
 * A sound you saved under someone else's name is a sound you will not find again, so SAVE asks for
 * a name as well as a slot. The two happen at once rather than one after the other: PRESETS picks
 * the slot, SELECT picks the letter you are on, ALGORITHM picks what that letter is, and SAVE
 * writes. Nothing is a new page and nothing is an extra press.
 *
 * The DX7 allows any printable character in a name; this knob offers a smaller alphabet, because
 * forty-one detents end to end is a name you can type and ninety-five is not. Space comes first,
 * so one detent back from A is a blank -- which is how a name shorter than ten gets its tail.
 *
 * The name being typed lives here and not in `edit`, because any button other than SAVE backs out
 * of the save, and backing out must leave the voice as it was. It is copied into the patch at the
 * moment the slot is written, so the voice line then agrees with the slot. */
static const char NAME_CH[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.#+";
#define NAME_NCH (sizeof NAME_CH - 1u)

static char save_name[11];
static uint8_t save_cur;

static uint32_t name_ix(char c)                 /* where a character sits on the knob; else a space */
{
    uint32_t i;
    for (i = 0; i < NAME_NCH; i++)
        if (NAME_CH[i] == c)
            return i;
    return 0;
}

static void name_begin(void)                    /* the name to edit is the one the patch already has */
{
    uint32_t i;
    for (i = 0; i < 10u; i++) {
        uint8_t c = edit[145 + i];
        save_name[i] = NAME_CH[name_ix(c >= 32u && c < 127u ? (char)c : ' ')];
    }
    save_name[10] = 0;
    save_cur = 0;
}

static int slot_free(uint32_t slot)             /* nothing of yours in it yet: still the INIT VOICE */
{
    static const char INIT[10] = {'I', 'N', 'I', 'T', ' ', 'V', 'O', 'I', 'C', 'E'};
    const uint8_t *b = bankv + slot * 128u + 118u;
    uint32_t i;
    for (i = 0; i < 10u; i++)
        if (b[i] != (uint8_t)INIT[i])
            return 0;
    return 1;
}

static uint32_t name_sum(void)                  /* enough of the name to notice a letter changing */
{
    uint32_t i, s = 0;
    for (i = 0; i < 10u; i++)
        s += (uint32_t)save_name[i] * (i + 1u);
    return s & 127u;
}

/* ---- main loop -> audio ISR: a single-producer ring of note events */
enum { EV_ON = 1, EV_OFF, EV_PATCH, EV_MONO, EV_PANIC, EV_SUSTAIN, EV_RELEASE_ALL, EV_SILENCE };
typedef struct { uint8_t kind, note, vel; } ev_t;
#define EVQ 32u
static ev_t evq[EVQ];
static volatile uint32_t ev_w, ev_r, ev_overflow;
static void say(const char *text);
static uint32_t midi_sustain, midi_silence;

static void ev_push(uint32_t kind, uint32_t note, uint32_t vel)
{
    uint32_t w = ev_w;
    if (ev_overflow || w - ev_r >= EVQ) {
        ev_overflow = 1; say("EVENT OVERFLOW"); return;
    }
    /* a patch event only says "read pbuf[pcur] again", so a second one behind the first is nothing:
     * a knob turned quickly would otherwise queue a dx_update of every voice per detent */
    if (kind == EV_PATCH && w != ev_r && evq[(w - 1u) % EVQ].kind == EV_PATCH)
        return;
    evq[w % EVQ].kind = (uint8_t)kind;
    evq[w % EVQ].note = (uint8_t)note;
    evq[w % EVQ].vel = (uint8_t)vel;
    RING_PUBLISH();                             /* the event is written before the ISR may see it */
    ev_w = w + 1u;
}

static void patch_publish(void)                 /* main loop: hand `edit` to the ISR */
{
    uint8_t *d = pbuf[pcur ^ 1u];
    uint32_t op;
    memcpy(d, edit, DX_VSZ);
    /* The switches are applied on the way out, so `edit` -- what is drawn, compared and reverted --
     * never carries them. An operator goes off by having its output level taken to zero, which is
     * what the DX7's switch did. Its keyboard level scaling goes with it: a + curve adds to the
     * output level before the level is used, so leaving it in would let a "switched off" operator
     * creep back at the top or bottom of the keyboard. */
    for (op = 0; op < 6u; op++)
        if (opmute >> op & 1u) {
            d[op * 21u + 16u] = 0;              /* output level */
            d[op * 21u + 9u] = 0;               /* L depth */
            d[op * 21u + 10u] = 0;              /* R depth */
        }
    RING_PUBLISH();                             /* the buffer is filled before the ISR is told to read it */
    pcur ^= 1u;
    ev_push(EV_PATCH, 0, 0);
}

/* ---- the voices (the ISR's) */
static dxv_t dxv[NV];
static struct {
    uint8_t note, vel, held, sustained;
    uint32_t age;
} vst[NV];
static uint32_t ages;
static volatile uint32_t v_lit;                 /* bit v: voice v sounds (the main loop reads, for the LEDs) */
static volatile uint8_t v_last;                 /* the newest note: the one the displays follow */
static volatile uint32_t midi_quiet_blocks;

/* ---- the global envelope (FM Lab's; a DX7 has nothing like it)
 *
 * Six operator envelopes are the honest way an FM voice is shaped, and they are also six places to
 * get one fade-in wrong. This is a single amplifier envelope over the whole voice, after the
 * operators and before the mix. It does not touch the timbre at all, only how much of it you hear,
 * and that difference is the thing worth learning from it: turn an operator's ATTACK up and the
 * note arrives dull and brightens as it goes, turn this one up and the finished sound -- bright
 * from the first instant -- fades in with its brightness intact.
 *
 * It is a setting of the instrument and not of the sound, for the same reason mono is: the 156
 * bytes are a DX7 voice and have no room for it, and a bank saved here should stay a true DX7
 * dump. So there is one of it, and it is under every sound you load.
 *
 * ADSR, and linear. Linear because it is drawn linear and its numbers are real milliseconds, and
 * someone learning this should be able to predict the picture from the knobs and the sound from
 * the picture. The operator envelopes next door are neither, and the contrast is the lesson.
 *
 * The times run 0 to 2 seconds over the 0..99 a knob gives, doubling about every nine clicks.
 * Two seconds is as slow as a shape you are meant to hear as a shape can usefully get; past that
 * you are no longer sculpting a note, you are fading one.
 *
 * The top of a falling stage is therefore not a very long time but OFF, and that stage then never
 * moves: DECAY at 99 holds at full and ignores the sustain, RELEASE at 99 leaves the note where it
 * was and the operators get to decide the tail, as they did before anyone touched this. That is
 * where RELEASE starts, and it is what makes full attack, full sustain and no release an envelope
 * that does nothing -- and one that does nothing is skipped outright: the multiply never happens
 * and the meter on GLOBAL reads exactly what it read before.
 *
 * Switch RELEASE on and it takes the tail over entirely. See genv_rel: the engine is not told the
 * key is up until the fade is finished, so the operators hold in sustain underneath it. Without
 * that the knob could only ever shorten a note, because the carriers would reach the floor on
 * their own and there would be nothing left to fade. The price is that the patch's own release is
 * not heard while this one is on, which is the trade the OFF position exists to let you refuse. */
enum { G_ATK = 0, G_DEC, G_SUS, G_REL, G_N };
#define G_ONE (1 << 24)                         /* the gain, Q24, 1.0 = all of the voice */

/* The time each knob value means, and the Q24 gain step that walks it in one render block. The
 * block is DX_N samples, and DX_N here is 64 and not dx7_core.c's 32 -- fmlab.c redefines DX_LG_N
 * to 6 just above. Generate these with the wrong block and every time is out by a factor of two
 * while every picture and every number on the screen still reads correctly, which is a long way to
 * go to find a bug. Anything under one block, 1.45 ms, is a step of G_ONE: instant. */
static const uint16_t GMS[100] = {
         0,     1,     1,     1,     1,     1,     1,     2,     2,     2,
         2,     2,     2,     3,     3,     3,     3,     3,     4,     4,
         4,     5,     5,     6,     6,     6,     7,     8,     8,     9,
         9,    10,    11,    12,    13,    14,    15,    16,    18,    19,
        21,    22,    24,    26,    28,    30,    33,    35,    38,    41,
        45,    48,    52,    56,    61,    66,    71,    77,    83,    90,
        97,   105,   113,   123,   132,   143,   155,   167,   181,   195,
       211,   228,   246,   266,   288,   311,   336,   363,   392,   424,
       458,   495,   535,   578,   625,   675,   730,   789,   852,   921,
       995,  1075,  1162,  1256,  1357,  1467,  1585,  1713,  1851,  2000,
};

static const int32_t GSTEP[100] = {
       16777216,   16777216,   16777216,   16777216,   16777216,   16777216,
       16777216,   12173944,   12173944,   12173944,   12173944,   12173944,
       12173944,    8115962,    8115962,    8115962,    8115962,    8115962,
        6086972,    6086972,    6086972,    4869577,    4869577,    4057981,
        4057981,    4057981,    3478270,    3043486,    3043486,    2705321,
        2705321,    2434789,    2213444,    2028991,    1872914,    1739135,
        1623192,    1521743,    1352660,    1281468,    1159423,    1106722,
        1014495,     936457,     869567,     811596,     737815,     695654,
         640734,     593851,     541064,     507248,     468229,     434784,
         399146,     368907,     342928,     316206,     293348,     270532,
         251009,     231885,     215468,     197950,     184454,     170265,
         157083,     145796,     134519,     124861,     115393,     106789,
          98975,      91533,      84541,      78289,      72464,      67074,
          62112,      57424,      53161,      49188,      45510,      42124,
          38957,      36071,      33353,      30859,      28577,      26436,
          24470,      22649,      20953,      19385,      17942,      16597,
          15361,      14214,      13154,      12174,
};

/* The main loop writes these and the ISR reads them. They are single bytes, so a read cannot catch
 * half of a write, and the worst a race can do is apply last block's attack time to this block. */
static volatile uint8_t gpar[G_N] = {0, 50, 99, 99};
static uint8_t g_live[G_N] = {0, 50, 99, 99};

/* A falling stage at the top of its knob does not fall at all. Attack has no such need: 0 is
 * already instant, and an attack that never arrived would simply be silence. */
static int32_t g_fall(uint32_t v)
{
    return v >= 99u ? 0 : GSTEP[v];
}

/* RELEASE is off at both ends. The top is off because that is what a falling stage does there;
 * the bottom is off because the thing it would otherwise mean -- cut the voice dead the instant
 * the key lifts -- is a click and not a release, and because OFF is where you want to get back to
 * in a hurry from wherever you are. DECAY keeps its 0: dropping straight to the sustain level is a
 * shape people actually want, and it is the only way to make a note start quiet. */
static int32_t g_rel(void)
{
    uint32_t v = g_live[G_REL];
    return (v == 0u || v >= 99u) ? 0 : GSTEP[v];
}

static uint32_t genv_idle(void)                 /* straight to full, stays there, never falls */
{
    return g_live[G_ATK] == 0u && g_live[G_SUS] >= 99u && g_rel() == 0;
}

enum { GS_OFF = 0, GS_ATK, GS_DEC, GS_REL };    /* sustain is decay that has arrived */
/* `hold` means the key is up but the engine has not been told. See genv_rel. */
static struct { uint8_t st, hold; int32_t g; } genv[NV];

static int32_t g_sus(void)
{
    return g_live[G_SUS] >= 99u ? G_ONE : (int32_t)g_live[G_SUS] * (G_ONE / 99);
}

/* One block of it. Decay and sustain are the same state: it walks towards the sustain level at the
 * decay rate and stops when it gets there, so turning SUSTAIN up under a held note slides rather
 * than jumps, and turning it up mid-decay is not a direction the envelope cannot go. */
static void genv_block(uint32_t v)
{
    int32_t g = genv[v].g, t, d;
    switch (genv[v].st) {
    case GS_ATK:
        g += GSTEP[g_live[G_ATK]];
        if (g >= G_ONE) { g = G_ONE; genv[v].st = GS_DEC; }
        break;
    case GS_DEC:
        t = g_sus();
        d = g_fall(g_live[G_DEC]);
        if (g > t)      { g -= d; if (g < t) g = t; }
        else if (g < t) { g += d; if (g > t) g = t; }
        break;
    case GS_REL:
        d = g_rel();
        if (!d) {                               /* switched to OFF mid-fade: hand the note straight
                                                 * back, or it would sustain for ever */
            if (genv[v].hold) { genv[v].hold = 0; dx_keyup(&dxv[v]); }
            break;
        }
        g -= d;
        if (g <= 0) { g = 0; genv[v].st = GS_OFF; genv[v].hold = 0; }
        break;
    default:
        break;
    }
    genv[v].g = g;
}

static void genv_key(uint32_t v, int again)     /* a note starts; `again` means it is not a slur */
{
    if (again)
        genv[v].g = g_live[G_ATK] ? 0 : G_ONE;
    genv[v].st = GS_ATK;
    genv[v].hold = 0;
}

/* A key goes up. Left to itself this envelope could only ever shorten a note: the operators would
 * reach the floor on their own release, dx_compute would be putting out silence, and fading silence
 * is not a release anyone can hear. So when there is a global RELEASE to run, the engine is not
 * told the key is up at all -- the operators stay in sustain underneath, and the fade on top is the
 * whole of what you hear. The engine finds out when the voice is freed a moment later.
 *
 * The cost is honest and worth saying out loud: with RELEASE on, the patch's own release is not
 * heard, because it never happens. RELEASE OFF, which is where it starts, gives it back. */
static uint32_t genv_rel(uint32_t v)            /* 1 if the caller should release the operators */
{
    genv[v].st = GS_REL;
    genv[v].hold = (uint8_t)(g_rel() > 0 && genv[v].g > 0);
    return !genv[v].hold;
}

/* The voice's transpose, u[144], 24 = none. dx7_core.c does not read it (sloopDX did not either), so
 * it is applied here, at the one place a note reaches the engine. vst[].note stays the note that was
 * played, so the keyboard and note-off still match. */
static int tnote(uint32_t note)
{
    int n = (int)note + (int)pbuf[pcur][144] - 24;
    return n < 0 ? 0 : n > 127 ? 127 : n;
}

static void v_on(uint32_t note, uint32_t vel)
{
    uint32_t v, pick = 0, best = 0xFFFFFFFFu, score;
    for (v = 0; v < NV; v++) {
        if ((vst[v].held || vst[v].sustained) && vst[v].note == note) score = 0u;
        else if (!dxv[v].on)                    score = 1u;                     /* never used */
        else if (!dx_playing(&dxv[v]))          score = 2u;                     /* finished */
        else if (!vst[v].held && !vst[v].sustained) score = 0x40000000u + vst[v].age;
        else if (vst[v].sustained)              score = 0x60000000u + vst[v].age;
        else                                    score = 0x80000000u + vst[v].age;  /* held: oldest */
        if (score < best) { best = score; pick = v; }
    }
    vst[pick].note = (uint8_t)note;
    vst[pick].vel = (uint8_t)vel;
    vst[pick].held = 1; vst[pick].sustained = 0;
    vst[pick].age = ++ages;
    dx_init(&dxv[pick], midi_voice, tnote(note), (int)vel);
    genv_key(pick, 1);
    v_last = (uint8_t)pick;                     /* the envelope and the diagram follow the newest note */
}

static void v_release(uint32_t v)
{
    vst[v].held = 0;
    if (midi_sustain) vst[v].sustained = 1;
    else {
        vst[v].sustained = 0;
        if (genv_rel(v)) dx_keyup(&dxv[v]);
    }
}

static void v_pedal(uint32_t down)
{
    uint32_t v;
    midi_sustain = down != 0u;
    if (!midi_sustain)
        for (v = 0; v < NV; v++)
            if (vst[v].sustained && !vst[v].held) v_release(v);
}

static void v_off(uint32_t note)
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (dxv[v].on && vst[v].held && vst[v].note == note) {
            v_release(v);
        }
}

/* ---- mono mode (FM Lab's; the DX7 has nothing to say about it, which is why the switch lives in
 * settings and not in the voice -- a bank saved here is still a true DX7 dump).
 *
 * One voice, last-note priority, with fall-back: the newest key down is the one you hear, and
 * letting it go drops you back to the key still under your finger rather than to silence. The
 * envelopes start again only when nothing was already held, so a slur keeps the attack it began
 * with and a separated note gets its own. On an FM patch that is a bigger difference than it sounds
 * -- the attack is where most of the bright modulation lives, so legato playing is a different
 * timbre, not just a different articulation. There is no portamento: the pitch steps. */
#define MSTK 16                                 /* more fingers than this and the oldest is forgotten */
static volatile uint8_t monomode;               /* the ISR's; the main loop asks for it with EV_MONO */
static uint8_t mnote[MSTK], mvel[MSTK], mheld;  /* the keys down, oldest first; the top is what sounds */

static uint32_t m_drop(uint32_t note)           /* take a key off the stack; 1 if it was there */
{
    uint32_t i;
    for (i = 0; i < mheld; i++)
        if (mnote[i] == note)
            break;
    if (i >= mheld)
        return 0;
    for (; i + 1u < mheld; i++) { mnote[i] = mnote[i + 1u]; mvel[i] = mvel[i + 1u]; }
    mheld--;
    return 1;
}

static void m_follow(uint32_t note, uint32_t vel, int again)
{
    vst[0].note = (uint8_t)note;
    vst[0].vel = (uint8_t)vel;
    vst[0].age = ++ages;
    if (again)
        dx_init(&dxv[0], midi_voice, tnote(note), (int)vel);
    else
        dx_retune(&dxv[0], midi_voice, tnote(note), (int)vel);
    vst[0].sustained = 0;
    genv_key(0, again);                         /* a slur keeps the envelope it began with */
    v_last = 0;
}

static void m_on(uint32_t note, uint32_t vel)
{
    uint32_t i, slur = mheld && dx_playing(&dxv[0]);
    m_drop(note);                               /* the same key twice: it is only ever on once */
    if (mheld == MSTK) {
        for (i = 0; i + 1u < MSTK; i++) { mnote[i] = mnote[i + 1u]; mvel[i] = mvel[i + 1u]; }
        mheld--;
    }
    mnote[mheld] = (uint8_t)note;
    mvel[mheld] = (uint8_t)vel;
    mheld++;
    vst[0].held = 1;
    m_follow(note, vel, !slur);
}

static void m_off(uint32_t note)
{
    if (!m_drop(note))
        return;
    if (!mheld) {
        v_release(0);
        return;
    }
    m_follow(mnote[mheld - 1u], mvel[mheld - 1u], 0);
}

static void ev_drain(void)                      /* audio ISR, before the render */
{
    if (ev_overflow) {
        uint32_t v;
        ev_r = ev_w; mheld = midi_sustain = 0; midi_silence = 1;
        for (v = 0; v < NV; v++) {
            dxv[v].on = vst[v].held = vst[v].sustained = 0;
            genv[v].st = GS_OFF; genv[v].hold = 0;
        }
        RING_PUBLISH(); ev_overflow = 0;
    }
    while (ev_r != ev_w) {
        const ev_t *e = &evq[ev_r % EVQ];
        uint32_t v;
        switch (e->kind) {
        case EV_ON:  if (monomode) m_on(e->note, e->vel); else v_on(e->note, e->vel); break;
        case EV_OFF: if (monomode) m_off(e->note); else v_off(e->note); break;
        case EV_PATCH:                          /* the voice changed under the sounding notes */
            for (v = 0; v < NV; v++)
                if (dxv[v].on)
                    dx_update(&dxv[v], midi_voice, tnote(vst[v].note), (int)vst[v].vel);
            break;
        case EV_MONO:                           /* the switch itself: start the new mode from silence */
            monomode = (uint8_t)e->note;
            for (v = 0; v < NV; v++) {
                dxv[v].on = 0; vst[v].held = vst[v].sustained = 0; genv[v].st = GS_OFF; genv[v].hold = 0;
            }
            mheld = 0;
            break;
        case EV_PANIC:
        case EV_SILENCE:
            for (v = 0; v < NV; v++) {
                dxv[v].on = 0; vst[v].held = vst[v].sustained = 0; genv[v].st = GS_OFF; genv[v].hold = 0;
            }
            mheld = 0;
            if (e->kind == EV_SILENCE) midi_silence = 1;
            break;
        case EV_SUSTAIN: v_pedal(e->note); break;
        case EV_RELEASE_ALL:
            mheld = 0;
            for (v = 0; v < NV; v++) if (vst[v].held) v_release(v);
            break;
        default: break;
        }
        ev_r++;
    }
}

/* ---- the clock, the arpeggiator and the sequencer: the ISR's half
 *
 * They are here for one reason, and it is not composition: a sound is designed by listening to it,
 * and listening is easier when the notes keep coming without a hand on the keys. So both are as
 * small as they can be and still do that. One clock of sixteenths at TEMPO drives both. The
 * arpeggiator walks the keys held (or, latched, the last chord) up, down, both, or at random, over
 * one to four octaves. The sequencer loops one recorded phrase of 1, 2 or 4 bars, with a click on
 * the quarters if you want one -- and always for the bar of count-in before a recording.
 *
 * The clock lives in the ISR rather than the main loop, because the main loop stops for as long as
 * a screen takes to draw and the ISR never does: here a note is late by at most one 64-sample block,
 * 1.5 ms, and the beat never drifts, because the phase is carried rather than restarted. The main
 * loop hands over everything it decides -- the chord, the phrase, the settings -- in one struct,
 * double-buffered the way the patch is. */
#define SQ_MAX 64u                              /* notes in a phrase */
#define ARP_MAX 16u                             /* keys in a chord */
enum { AP_UP = 0, AP_DOWN, AP_UPDOWN, AP_RANDOM, AP_PLAYED };
typedef struct { uint8_t step, note, len; } sqn_t;   /* a note of the phrase, in sixteenths */
typedef struct {
    uint32_t step_q8;                           /* a sixteenth: Q8 samples */
    uint32_t a_len[2], a_gate[2];               /* an arp step on the beat and off it -- SWING is the
                                                 * difference -- and how much of each sounds */
    uint8_t run, gen, cin, click;               /* gen: which PLAY; cin: steps of count-in before the loop */
    uint8_t arp, pat, oct, an, vel, sq_n, sq_len;
    uint8_t chord[ARP_MAX];                     /* ascending */
    uint8_t ord[ARP_MAX];                       /* ..and as the keys went down */
    sqn_t sq[SQ_MAX];
} play_t;
static play_t plbuf[2];
static volatile uint32_t plcur;
/* where the clock is, for the main loop to record against: the step since PLAY (count-in and all),
 * how far into it in Q8 samples, and which PLAY -- or 0xFFFF, stopped */
static volatile uint32_t pl_at, pl_atph, pl_atgen = 0xFFFFu;

static uint32_t pl_ph, pl_step, arp_ph, arp_rng = 1u;
static uint32_t clk_ph, clk_inc;                /* the click: a square, dying away */
static int32_t clk_a;                           /* its level, Q8 */
#define CLK_LEN 900                             /* 20 ms */
#define CLK_A0 (5000 * 256)
static uint8_t pl_run, pl_gen, pl_an, arp_odd, arp_ni, arp_oi, arp_up = 1, arp_new, arp_note = 0xFFu;
static uint8_t sq_note[SQ_MAX], sq_left[SQ_MAX];  /* what each note of the phrase is sounding, and for
                                                   * how many more steps: the note is kept, so a phrase
                                                   * replaced under it can still let it go */

static void pl_on(uint32_t n, uint32_t vel) { if (monomode) m_on(n, vel); else v_on(n, vel); }
static void pl_off(uint32_t n) { if (monomode) m_off(n); else v_off(n); }

static void arp_upstep(uint32_t n, uint32_t o)
{
    if (++arp_ni >= n) { arp_ni = 0; if (++arp_oi >= o) arp_oi = 0; }
}

static void arp_downstep(uint32_t n, uint32_t o)
{
    if (arp_ni) { arp_ni--; return; }
    arp_ni = (uint8_t)(n - 1u);
    arp_oi = (uint8_t)(arp_oi ? arp_oi - 1u : o - 1u);
}

static uint32_t arp_rand(uint32_t n)            /* 0..n-1, without a division */
{
    arp_rng = arp_rng * 1664525u + 1013904223u;
    return ((arp_rng >> 16) * n) >> 16;
}

static void arp_next(const play_t *p)
{
    uint32_t n = p->an, o = p->oct + 1u;
    if (arp_ni >= n) arp_ni = (uint8_t)(n - 1u);    /* a key let go under it */
    if (arp_oi >= o) arp_oi = (uint8_t)(o - 1u);
    if (p->pat == AP_RANDOM) {
        arp_ni = (uint8_t)arp_rand(n);
        arp_oi = (uint8_t)arp_rand(o);
    } else if (arp_new) {                       /* a fresh chord: from its own end */
        arp_up = p->pat != AP_DOWN;
        arp_ni = (uint8_t)(arp_up ? 0u : n - 1u);
        arp_oi = (uint8_t)(arp_up ? 0u : o - 1u);
    } else if (p->pat == AP_UP || p->pat == AP_PLAYED) {
        arp_upstep(n, o);
    } else if (p->pat == AP_DOWN) {
        arp_downstep(n, o);
    } else if (n * o > 1u) {                    /* up and down, without playing either end twice */
        if (arp_up && arp_ni + 1u >= n && arp_oi + 1u >= o) arp_up = 0;
        else if (!arp_up && !arp_ni && !arp_oi) arp_up = 1;
        if (arp_up) arp_upstep(n, o); else arp_downstep(n, o);
    }
    arp_new = 0;
}

static void clk_go(uint32_t bar)                /* a tick; higher on the first beat of a bar */
{
    clk_ph = 0;
    clk_inc = bar ? 194783098u : 97391549u;     /* 2 kHz, 1 kHz: 2^32 f / FS */
    clk_a = CLK_A0;
}

static void pl_tick(const play_t *p)            /* once a 64-sample block, before it is rendered */
{
    uint32_t i, an = p->arp ? p->an : 0u;
    if (p->run != pl_run || (p->run && p->gen != pl_gen)) {   /* started, stopped or started over:
                                                               * from the top, now */
        for (i = 0; i < SQ_MAX; i++)
            if (sq_left[i]) { pl_off(sq_note[i]); sq_left[i] = 0; }
        pl_run = p->run;
        pl_gen = p->gen;
        if (pl_run) {
            pl_step = 0xFFFFFFFFu;
            pl_ph = p->step_q8;
            arp_ph = p->a_len[1];               /* the arpeggiator on the same downbeat */
            arp_odd = 1;
        }
    }
    if ((an != 0u) != (pl_an != 0u)) {         /* the arpeggiator started or stopped */
        if (arp_note != 0xFFu) { pl_off(arp_note); arp_note = 0xFFu; }
        if (an) {
            arp_new = 1;
            if (!pl_run) {                      /* on its own it starts with the key, not the beat */
                arp_ph = p->a_len[1];
                arp_odd = 1;
            }
        }
    }
    pl_an = (uint8_t)an;
    if (pl_ph >= p->step_q8) {
        pl_ph -= p->step_q8;
        if (pl_ph >= p->step_q8)
            pl_ph = 0;                          /* the tempo went up past where we were */
        pl_step++;
        if (pl_run && pl_step < p->cin) {       /* the count-in: only the click */
            if (!(pl_step & 3u))
                clk_go(!(pl_step & 15u));
        } else if (pl_run && p->sq_len) {
            uint32_t pos = (pl_step - p->cin) & (p->sq_len - 1u);   /* 16, 32 or 64 */
            if (p->click && !(pos & 3u))
                clk_go(!(pos & 15u));
            for (i = 0; i < SQ_MAX; i++)
                if (sq_left[i] && --sq_left[i] == 0u)
                    pl_off(sq_note[i]);
            for (i = 0; i < p->sq_n; i++)
                if (p->sq[i].step == pos) {
                    if (sq_left[i]) pl_off(sq_note[i]);
                    sq_note[i] = p->sq[i].note;
                    sq_left[i] = p->sq[i].len;
                    pl_on(sq_note[i], 100u);
                }
        }
    }
    pl_ph += (uint32_t)DX_N << 8;
    /* the arpeggiator's own steps, at RATE and swung: they run on whether or not there is a chord,
     * so one started under a playing phrase lands on its grid */
    if (arp_note != 0xFFu && arp_ph >= p->a_gate[arp_odd]) { pl_off(arp_note); arp_note = 0xFFu; }
    if (arp_ph >= p->a_len[arp_odd]) {
        arp_ph -= p->a_len[arp_odd];
        arp_odd ^= 1u;
        if (arp_ph >= p->a_len[arp_odd])
            arp_ph = 0;                         /* the tempo went up past where we were */
        if (an) {
            uint32_t n;
            if (arp_note != 0xFFu) pl_off(arp_note);
            arp_next(p);
            n = (uint32_t)(p->pat == AP_PLAYED ? p->ord : p->chord)[arp_ni] + 12u * arp_oi;
            arp_note = (uint8_t)(n > 127u ? 127u : n);
            pl_on(arp_note, p->vel);
        }
    }
    arp_ph += (uint32_t)DX_N << 8;
    pl_at = pl_step;
    pl_atph = pl_ph;
    pl_atgen = pl_run ? pl_gen : 0xFFFFu;
}

/* ---- the load. The ISR must render HALF_FRAMES before the DMA plays them: 5.80 ms at 44.1 kHz.
 * Anything under that is headroom, and this is the number that decides how many voices the FM-1 can
 * actually hold. On the computer fm1_ticks() is that computer's clock, so the figure means nothing
 * there -- read it on the device. */
#define BUDGET_US (HALF_FRAMES * 1000000u / FS)
static volatile uint32_t cpu_q8, cpu_peak;      /* Q8 of the budget; the worst block so far, in percent */
static uint8_t cpu_over;                        /* bit k: the block k blocks ago took over 85 % */
typedef struct { uint32_t percent, synth, fx, control, post; } cpu_profile_t;
static cpu_profile_t cpu_profiles[2];
static volatile uint32_t cpu_profile_index, cpu_late, cpu_reset_generation;
static uint32_t cpu_reset_seen;

static void cpu_request_reset(void)
{
    RING_PUBLISH(); cpu_reset_generation++;
}

static void cpu_reset_pending(void)
{
    uint32_t generation = cpu_reset_generation;
    if (generation == cpu_reset_seen) return;
    cpu_peak = cpu_late = 0;
    memset(cpu_profiles,0,sizeof cpu_profiles);
    cpu_reset_seen = generation;
}

static void v_shed(void)                        /* overloaded: lose a voice rather than glitch every voice */
{
    uint32_t v, pick = NV, worst = 0, score;
    for (v = 0; v < NV; v++) {
        if (!dxv[v].on)
            continue;
        score = (vst[v].held ? 0u : 0x80000000u) + (ages - vst[v].age);   /* releasing first, then oldest */
        if (score >= worst) { worst = score; pick = v; }
    }
    if (pick < NV)
        dxv[pick].on = 0;
}

static void cpu_report(uint32_t ticks, const cpu_profile_t *parts)
{
    uint32_t us = ticks / FM1_TICKS_PER_US, percent = us * 100u / BUDGET_US;
    if (percent > cpu_peak) {
        uint32_t next = cpu_profile_index ^ 1u;
        cpu_profile_t *p = &cpu_profiles[next];
        p->percent = percent;
        p->synth = parts->synth / FM1_TICKS_PER_US * 100u / BUDGET_US;
        p->fx = parts->fx / FM1_TICKS_PER_US * 100u / BUDGET_US;
        p->control = parts->control / FM1_TICKS_PER_US * 100u / BUDGET_US;
        p->post = parts->post / FM1_TICKS_PER_US * 100u / BUDGET_US;
        RING_PUBLISH(); cpu_profile_index = next;
        cpu_peak = percent;
    }
    if (us > BUDGET_US) cpu_late++;
    cpu_q8 = (cpu_q8 * 15u + us * 256u / BUDGET_US) / 16u;
    cpu_over = (uint8_t)(cpu_over << 1 | (us * 100u > BUDGET_US * 85u));
    if ((cpu_over & 3u) == 3u) v_shed();
}

/* ---- the filter's coefficients, as the ISR reads them. Worked out in the main loop, because they
 * need a division and the ISR has no time for one; handed over the way the patch is, by writing
 * the spare half and flipping the index, so the ISR never reads a half-written set. */
typedef struct { int32_t a1, a2, a3, k; uint32_t type; } flt_t;
static flt_t fbuf[2];
static volatile uint8_t fcur;                   /* the ISR reads fbuf[fcur] */

/* ---- the effects
 *
 * Four, after the filter and after the analyser's tap, so the spectrum and the wave go on showing
 * what the operators made and these are the room it is played in. They are always in series --
 * PHASER, CHORUS, DELAY, REVERB, the order a pedalboard would put them in -- and each is off at
 * MIX 0, which is off and free: the ISR skips it. Up to the chorus the sound is mono; the chorus
 * and the reverb come back in stereo, so they are wider on headphones than on the speaker.
 *
 * PHASER: six allpasses, each a quarter turn of phase at a frequency the LFO sweeps, added back to
 * the dry sound. Where the two are out of step they cancel, so the sweep is three notches moving
 * through the spectrum together. FEEDBACK puts some of the output back in and deepens them.
 * CHORUS: a copy, a few milliseconds late, the lateness slowly swinging: a copy whose pitch is a
 * touch sharp and then a touch flat, which is what two players in unison are. WIDTH puts the left
 * and right copies out of step. DELAY: the echo, its own length later, with FEEDBACK echoing the
 * echo and TONE darkening each repeat a little more than the last. REVERB: a room, as four delay
 * lines between 34 and 64 ms that feed every one of them into all four (a feedback delay network,
 * after the one in dxsloop by Leo Kuroshita, GPL-3.0), behind two milliseconds of diffusers that
 * smear the attack. DECAY is how long it takes to fall 60 dB; DAMP takes the top off each pass,
 * as soft walls do; PRE is the time before the first reflection, which is how big the room sounds.
 *
 * The coefficients come from the main loop through a double buffer, as the filter's do. */
static const uint16_t FX_RATE[100] = {   /* centihertz: 0.05 Hz .. 10 Hz, evenly in pitch */
    5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 9, 9,
    10, 10, 11, 11, 12, 12, 13, 14, 15, 15, 16, 17,
    18, 19, 20, 21, 22, 24, 25, 26, 28, 29, 31, 33,
    34, 36, 38, 40, 43, 45, 47, 50, 53, 56, 59, 62,
    65, 69, 73, 77, 81, 85, 90, 95, 100, 106, 111, 118,
    124, 131, 138, 146, 154, 162, 171, 180, 190, 201, 212, 223,
    236, 249, 262, 277, 292, 308, 325, 343, 362, 382, 403, 425,
    448, 473, 499, 526, 555, 586, 618, 652, 688, 725, 765, 807,
    852, 898, 948, 1000,
};
static const int16_t PH_A[225] = {      /* the allpass coefficient, Q15: 100 Hz .. 12.8 kHz, 32 an octave */
    -32303, -32293, -32283, -32273, -32262, -32251, -32240, -32228, -32216, -32205,
    -32192, -32180, -32167, -32154, -32141, -32127, -32113, -32099, -32085, -32070,
    -32055, -32039, -32024, -32008, -31991, -31974, -31957, -31940, -31922, -31904,
    -31885, -31866, -31846, -31826, -31806, -31785, -31764, -31743, -31721, -31698,
    -31675, -31652, -31628, -31603, -31578, -31552, -31526, -31500, -31472, -31445,
    -31416, -31387, -31358, -31328, -31297, -31265, -31233, -31200, -31167, -31133,
    -31098, -31062, -31026, -30989, -30951, -30912, -30873, -30832, -30791, -30749,
    -30707, -30663, -30618, -30573, -30526, -30479, -30431, -30381, -30331, -30279,
    -30227, -30174, -30119, -30063, -30007, -29949, -29890, -29829, -29768, -29705,
    -29641, -29576, -29509, -29442, -29372, -29302, -29230, -29157, -29082, -29006,
    -28928, -28849, -28768, -28685, -28601, -28516, -28429, -28340, -28249, -28157,
    -28063, -27967, -27869, -27770, -27668, -27565, -27460, -27353, -27244, -27132,
    -27019, -26904, -26786, -26667, -26545, -26421, -26295, -26166, -26035, -25902,
    -25767, -25629, -25489, -25346, -25200, -25052, -24902, -24749, -24593, -24435,
    -24274, -24110, -23943, -23774, -23601, -23426, -23248, -23067, -22883, -22696,
    -22505, -22312, -22116, -21916, -21713, -21507, -21298, -21085, -20869, -20649,
    -20426, -20200, -19970, -19737, -19499, -19259, -19014, -18766, -18514, -18258,
    -17999, -17735, -17468, -17196, -16921, -16641, -16357, -16070, -15777, -15481,
    -15180, -14875, -14565, -14251, -13932, -13608, -13279, -12946, -12608, -12265,
    -11917, -11563, -11204, -10840, -10471, -10095, -9714, -9327, -8935, -8536,
    -8130, -7718, -7300, -6875, -6443, -6003, -5556, -5101, -4639, -4168,
    -3688, -3200, -2702, -2194, -1677, -1149, -611, -60, 502, 1077,
    1665, 2267, 2884, 3516, 4166,
};
static const uint16_t DLY_T[100] = {     /* samples: 20 ms .. 1.4 s, evenly in ratio */
    882, 921, 961, 1003, 1047, 1093, 1141, 1191, 1243, 1298,
    1355, 1414, 1476, 1541, 1608, 1679, 1753, 1829, 1910, 1993,
    2081, 2172, 2267, 2367, 2470, 2579, 2692, 2810, 2933, 3062,
    3196, 3336, 3482, 3635, 3794, 3961, 4134, 4316, 4505, 4702,
    4909, 5124, 5349, 5583, 5828, 6083, 6350, 6629, 6919, 7223,
    7539, 7870, 8215, 8575, 8951, 9344, 9753, 10181, 10628, 11094,
    11580, 12088, 12618, 13171, 13749, 14351, 14981, 15638, 16323, 17039,
    17786, 18566, 19380, 20230, 21117, 22043, 23010, 24018, 25072, 26171,
    27319, 28516, 29767, 31072, 32434, 33857, 35341, 36891, 38508, 40197,
    41960, 43799, 45720, 47725, 49817, 52002, 54282, 56662, 59147, 61740,
};
static const uint16_t DLY_TONE[100] = {  /* one pole in the loop, Q15: 500 Hz .. open */
    2253, 2335, 2421, 2509, 2600, 2695, 2793, 2894, 2999, 3107,
    3219, 3335, 3454, 3578, 3706, 3838, 3975, 4116, 4261, 4412,
    4567, 4727, 4892, 5063, 5239, 5420, 5608, 5800, 5999, 6204,
    6415, 6632, 6855, 7085, 7321, 7565, 7814, 8071, 8335, 8606,
    8883, 9169, 9461, 9760, 10067, 10381, 10703, 11031, 11368, 11711,
    12061, 12419, 12784, 13156, 13534, 13919, 14311, 14709, 15113, 15522,
    15937, 16358, 16783, 17213, 17646, 18084, 18525, 18968, 19414, 19861,
    20310, 20759, 21208, 21656, 22103, 22548, 22990, 23429, 23864, 24293,
    24717, 25135, 25546, 25949, 26343, 26729, 27104, 27469, 27823, 28166,
    28496, 28814, 29119, 29411, 29689, 29953, 30204, 30440, 30662, 32767,
};
static const uint8_t RV_RT[100] = {      /* the decay to -60 dB, in tenths of a second */
    3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5,
    5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10,
    10, 10, 11, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 17, 17,
    18, 19, 19, 20, 21, 22, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    33, 34, 35, 36, 38, 39, 41, 42, 44, 46, 47, 49, 51, 53, 55, 57,
    59, 61, 64, 66, 69, 71, 74, 77, 80, 83, 86, 89, 92, 96, 100, 103,
    107, 111, 116, 120,
};
static const uint16_t RV_G[400] = {      /* each line's gain for that decay, Q15, four to a row */
    14981, 12221, 9452, 7465, 15416, 12669, 9892, 7880,
    15847, 13118, 10335, 8302, 16273, 13564, 10780, 8729,
    16695, 14009, 11228, 9162, 17112, 14451, 11676, 9599,
    17524, 14890, 12125, 10040, 17929, 15326, 12574, 10484,
    18329, 15758, 13023, 10930, 18723, 16186, 13470, 11378,
    19110, 16609, 13915, 11827, 19491, 17027, 14358, 12276,
    19865, 17439, 14797, 12725, 20232, 17846, 15234, 13173,
    20592, 18247, 15667, 13619, 20944, 18642, 16095, 14064,
    21290, 19031, 16519, 14505, 21628, 19413, 16938, 14944,
    21960, 19788, 17352, 15380, 22283, 20156, 17760, 15811,
    22600, 20518, 18163, 16238, 22909, 20872, 18559, 16660,
    23211, 21219, 18949, 17078, 23506, 21559, 19332, 17490,
    23793, 21892, 19709, 17896, 24073, 22217, 20079, 18296,
    24346, 22535, 20442, 18690, 24612, 22846, 20798, 19078,
    24871, 23149, 21146, 19459, 25123, 23445, 21488, 19834,
    25368, 23734, 21822, 20201, 25607, 24016, 22149, 20562,
    25839, 24290, 22468, 20915, 26064, 24558, 22781, 21262,
    26283, 24818, 23086, 21601, 26496, 25072, 23383, 21932,
    26703, 25318, 23674, 22257, 26904, 25558, 23957, 22574,
    27098, 25792, 24233, 22884, 27287, 26018, 24502, 23186,
    27470, 26239, 24763, 23481, 27648, 26453, 25018, 23769,
    27820, 26661, 25267, 24050, 27987, 26863, 25508, 24324,
    28149, 27059, 25743, 24590, 28306, 27249, 25971, 24850,
    28458, 27433, 26193, 25102, 28605, 27612, 26408, 25348,
    28748, 27785, 26617, 25587, 28886, 27953, 26820, 25820,
    29019, 28116, 27018, 26046, 29148, 28274, 27209, 26265,
    29273, 28427, 27394, 26479, 29394, 28575, 27574, 26686,
    29511, 28719, 27749, 26887, 29625, 28858, 27918, 27082,
    29734, 28992, 28082, 27272, 29840, 29122, 28241, 27455,
    29942, 29248, 28395, 27634, 30041, 29370, 28544, 27806,
    30137, 29488, 28689, 27974, 30229, 29601, 28828, 28136,
    30318, 29712, 28964, 28293, 30404, 29818, 29095, 28446,
    30488, 29921, 29222, 28593, 30568, 30021, 29344, 28736,
    30646, 30117, 29463, 28874, 30721, 30210, 29578, 29008,
    30794, 30300, 29689, 29138, 30864, 30387, 29796, 29263,
    30931, 30471, 29900, 29384, 30997, 30552, 30000, 29502,
    31060, 30630, 30097, 29615, 31121, 30706, 30191, 29725,
    31179, 30779, 30281, 29831, 31236, 30849, 30369, 29934,
    31291, 30918, 30453, 30033, 31343, 30983, 30535, 30129,
    31394, 31047, 30614, 30221, 31444, 31108, 30690, 30311,
    31491, 31167, 30764, 30397, 31537, 31224, 30835, 30481,
    31581, 31280, 30903, 30562, 31624, 31333, 30970, 30640,
    31665, 31384, 31034, 30715, 31704, 31434, 31095, 30788,
    31743, 31481, 31155, 30858, 31780, 31528, 31212, 30926,
    31815, 31572, 31268, 30991, 31849, 31615, 31322, 31055,
    31883, 31656, 31373, 31116, 31914, 31696, 31423, 31174,
    31945, 31735, 31471, 31231, 31975, 31772, 31518, 31286,
    32004, 31808, 31563, 31339, 32031, 31842, 31606, 31390,
    32058, 31876, 31648, 31440, 32083, 31908, 31688, 31487,
    32108, 31939, 31727, 31533, 32132, 31969, 31764, 31577,
};
static const uint16_t RV_DAMP[100] = {   /* one pole in each line, Q15: open .. 1.2 kHz */
    32767, 30713, 30547, 30373, 30191, 30001, 29803, 29596, 29382, 29160,
    28930, 28692, 28447, 28195, 27935, 27669, 27396, 27117, 26832, 26541,
    26244, 25942, 25636, 25324, 25009, 24689, 24366, 24039, 23710, 23378,
    23043, 22706, 22368, 22028, 21687, 21345, 21003, 20661, 20318, 19976,
    19634, 19293, 18954, 18615, 18279, 17943, 17610, 17279, 16951, 16625,
    16301, 15981, 15663, 15349, 15038, 14730, 14426, 14125, 13828, 13535,
    13246, 12960, 12679, 12402, 12128, 11859, 11595, 11334, 11077, 10825,
    10577, 10334, 10094, 9859, 9628, 9402, 9179, 8961, 8747, 8537,
    8331, 8130, 7932, 7738, 7549, 7363, 7181, 7003, 6829, 6659,
    6492, 6329, 6169, 6013, 5861, 5712, 5566, 5424, 5285, 5149,
};

enum { FX_CHO = 0, FX_PHA, FX_DLY, FX_REV, FX_N };
typedef struct {
    uint32_t on;                                /* bit k: effect k is running */
    int32_t ch_mix, ch_dep, ch_wid; uint32_t ch_inc;
    int32_t ph_mix, ph_span, ph_fb; uint32_t ph_inc;
    int32_t dl_mix, dl_len, dl_fb, dl_tone;
    int32_t rv_mix, rv_pre, rv_damp, rv_g[4];
} fx_t;
static fx_t xbuf[2];
static volatile uint8_t xcur;

#define CHO_LEN 1024u                           /* 23 ms: the longest the chorus reaches is 11 */
#define CHO_BASE (220 << 8)                     /* 5 ms, Q8: where its swing starts */
#define DLY_LEN 65536u                          /* 1.49 s: TIME goes to 1.4 */
#define PRE_LEN 8192u                           /* 186 ms: PRE goes to 99 */
#define PH_LO (32 << 8)                         /* the sweep's bottom: 200 Hz, on PH_A's grid */
#define RV_NL (1499u + 1889u + 2381u + 2833u)
#define RV_NA (211u + 159u + 563u + 411u)
static const uint16_t RV_LEN[4] = {1499, 1889, 2381, 2833};   /* coprime, so the echoes never line up */
static const uint16_t RV_AP[4] = {211, 159, 563, 411};
static int32_t cho_buf[CHO_LEN] __attribute__((section(".pool")));      /* Q23, as the dry sound */
static int16_t dly_buf[DLY_LEN] __attribute__((section(".pool")));      /* Q15: at 32 bits, 128 KiB more */
static int32_t pre_buf[PRE_LEN] __attribute__((section(".pool")));      /* Q23 */
static int32_t rv_line[RV_NL] __attribute__((section(".pool")));  /* Q8: a tail fades below a bit */
static int32_t rv_ap[RV_NA] __attribute__((section(".pool")));
static struct {
    uint32_t cw, ch_ph, ph_ph, dw, pw;
    uint32_t cv, dv, pv, lv[4], av[4];
    int32_t ph_x[6], ph_y[6], ph_last, ph_a;
    int32_t d_cur, d_lp;                        /* the delay's length now, Q8, gliding to TIME's */
    uint16_t li[4], ai[4];
    int32_t lp[4];
} xs;

static int32_t sat16(int32_t x) { return x > 32767 ? 32767 : x < -32767 ? -32767 : x; }
static int32_t sat24(int32_t x) { return x > 8388607 ? 8388607 : x < -8388607 ? -8388607 : x; }
static int32_t mulq15(int32_t x, int32_t q15) { return (int32_t)(((int64_t)x * q15) >> 15); }
static uint32_t tri16(uint32_t ph)              /* a triangle, 0 .. 65535 .. 0, once a turn */
{
    uint32_t t = ph >> 15;
    return t < 65536u ? t : 131071u - t;
}

static int32_t fx_chorus_tap(uint32_t delay)
{
    uint32_t age = delay >> 8, p = xs.cw-age;
    int32_t a = age < xs.cv ? cho_buf[p & (CHO_LEN-1u)] : 0;
    int32_t b = age+1u < xs.cv ? cho_buf[(p-1u) & (CHO_LEN-1u)] : 0;
    return a+(int32_t)(((int64_t)(b-a)*(delay & 255u)) >> 8);
}

/* n = DX_N samples: l in, and l and r out, all Q23 */
static void fx_run(const fx_t *x, int32_t *l, int32_t *r)
{
    uint32_t i, k;
    if (!(x->on & 1u << FX_PHA)) {
        for (k = 0; k < 6u; k++)
            xs.ph_x[k] = xs.ph_y[k] = 0;
        xs.ph_last = 0;
        xs.ph_a = PH_A[PH_LO >> 8];
    } else {                                    /* the sweep moves once a block, and slides across it */
        int32_t pos, a1, a, da, s, k2;
        xs.ph_ph += x->ph_inc * DX_N;
        pos = PH_LO + (int32_t)((tri16(xs.ph_ph) * (uint32_t)x->ph_span) >> 16);
        if (pos > 224 * 256 - 1)
            pos = 224 * 256 - 1;
        a1 = PH_A[pos >> 8] + (((PH_A[(pos >> 8) + 1] - PH_A[pos >> 8]) * (pos & 255)) >> 8);
        da = (a1 - xs.ph_a) >> DX_LG_N;
        a = xs.ph_a;
        for (i = 0; i < DX_N; i++) {
            a += da;
            s = l[i] + (int32_t)(((int64_t)xs.ph_last * x->ph_fb) >> 15);
            for (k2 = 0; k2 < 6; k2++) {        /* y = a (x - y') + x': a quarter turn at the break */
                int32_t y = (int32_t)(((int64_t)a * (s - xs.ph_y[k2])) >> 15) + xs.ph_x[k2];
                xs.ph_x[k2] = s;
                xs.ph_y[k2] = y;
                s = y;
            }
            s = s > 8388607 ? 8388607 : s < -8388607 ? -8388607 : s;
            xs.ph_last = s;
            l[i] += mulq15(s - l[i], x->ph_mix);
        }
        xs.ph_a = a1;
    }
    if (!(x->on & 1u << FX_CHO)) {
        xs.cv = 0;
        for (i = 0; i < DX_N; i++)
            r[i] = l[i];
    } else {
        for (i = 0; i < DX_N; i++) {
            int32_t m = l[i], d;
            cho_buf[xs.cw & (CHO_LEN - 1u)] = m;
            if (xs.cv < CHO_LEN) xs.cv++;
            xs.ch_ph += x->ch_inc;
            d = CHO_BASE + (int32_t)(((tri16(xs.ch_ph) >> 4) * (uint32_t)x->ch_dep) >> 12);
            l[i] = m+mulq15(fx_chorus_tap((uint32_t)d)-m,x->ch_mix);
            d = CHO_BASE + (int32_t)(((tri16(xs.ch_ph + (uint32_t)x->ch_wid) >> 4) * (uint32_t)x->ch_dep) >> 12);
            r[i] = m+mulq15(fx_chorus_tap((uint32_t)d)-m,x->ch_mix);
            xs.cw++;
        }
    }
    if (!(x->on & 1u << FX_DLY)) {
        xs.dv = 0;
        xs.d_cur = x->dl_len << 8;
        xs.d_lp = 0;
    } else {
        if (!xs.dv) xs.d_cur = x->dl_len << 8;
        for (i = 0; i < DX_N; i++) {
            int32_t df = (x->dl_len << 8) - xs.d_cur, f, a, b, e;
            uint32_t age, p;
            xs.d_cur += (df >> 11) + (df > 0) - (df < 0);   /* a new TIME glides, as tape would */
            age = (uint32_t)xs.d_cur >> 8; p = xs.dw-age; f = xs.d_cur & 255;
            a = age <= xs.dv ? dly_buf[p & (DLY_LEN-1u)] : 0;
            b = age+1u <= xs.dv ? dly_buf[(p-1u) & (DLY_LEN-1u)] : 0;
            e = a + (((b - a) * f) >> 8);
            xs.d_lp += ((e - xs.d_lp) * x->dl_tone) >> 15;
            dly_buf[xs.dw & (DLY_LEN - 1u)] =
                (int16_t)sat16(((l[i] + r[i]) >> 9) + ((xs.d_lp * x->dl_fb) >> 15));
            xs.dw++;
            if (xs.dv < DLY_LEN) xs.dv++;
            e = (xs.d_lp * x->dl_mix) >> 7;     /* Q15 by Q15, out as Q23 */
            l[i] += e;
            r[i] += e;
        }
    }
    if (!(x->on & 1u << FX_REV)) {
        xs.pv = 0;
        for (k = 0; k < 4u; k++)
            { xs.lp[k] = 0; xs.lv[k] = xs.av[k] = 0; }
    } else {
        for (i = 0; i < DX_N; i++) {
            int32_t v, o[4], h[4], ol, or_;
            uint32_t base = 0;
            pre_buf[xs.pw & (PRE_LEN - 1u)] = sat24((l[i] >> 2) + (r[i] >> 2));
            if (xs.pv < PRE_LEN) xs.pv++;
            v = (uint32_t)x->rv_pre < xs.pv ? pre_buf[(xs.pw-(uint32_t)x->rv_pre) & (PRE_LEN-1u)] : 0;
            xs.pw++;
            for (k = 0; k < 4u; k++) {          /* the diffusers: allpasses, g = 5/8 */
                int32_t d = xs.av[k] >= RV_AP[k] ? rv_ap[base+xs.ai[k]] : 0, t = v+((d*5) >> 3);
                v = d - ((t * 5) >> 3);
                rv_ap[base + xs.ai[k]] = t;
                if (xs.av[k] < RV_AP[k]) xs.av[k]++;
                if (++xs.ai[k] >= RV_AP[k]) xs.ai[k] = 0;
                base += RV_AP[k];
            }
            base = 0;
            for (k = 0; k < 4u; k++) {          /* each line, out, and its walls */
                o[k] = xs.lv[k] >= RV_LEN[k] ? rv_line[base+xs.li[k]] : 0;
                xs.lp[k] += (int32_t)(((int64_t)(o[k] - xs.lp[k]) * x->rv_damp) >> 15);
                base += RV_LEN[k];
            }
            h[0] = (xs.lp[0] + xs.lp[1] + xs.lp[2] + xs.lp[3]) >> 1;   /* every line into all four */
            h[1] = (xs.lp[0] - xs.lp[1] + xs.lp[2] - xs.lp[3]) >> 1;
            h[2] = (xs.lp[0] + xs.lp[1] - xs.lp[2] - xs.lp[3]) >> 1;
            h[3] = (xs.lp[0] - xs.lp[1] - xs.lp[2] + xs.lp[3]) >> 1;
            base = 0;
            for (k = 0; k < 4u; k++) {
                int32_t w = v + (int32_t)(((int64_t)h[k] * x->rv_g[k]) >> 15);
                rv_line[base + xs.li[k]] = w > 33554431 ? 33554431 : w < -33554431 ? -33554431 : w;
                if (xs.lv[k] < RV_LEN[k]) xs.lv[k]++;
                if (++xs.li[k] >= RV_LEN[k]) xs.li[k] = 0;
                base += RV_LEN[k];
            }
            ol = (o[0] + o[2]) >> 3;            /* at MIX 99, a held note about as loud wet as dry */
            or_ = (o[1] + o[3]) >> 3;
            ol = ol > 16777215 ? 16777215 : ol < -16777215 ? -16777215 : ol;
            or_ = or_ > 16777215 ? 16777215 : or_ < -16777215 ? -16777215 : or_;
            l[i] += mulq15(ol, x->rv_mix);
            r[i] += mulq15(or_, x->rv_mix);
        }
    }
    for (i = 0; i < DX_N; i++) {
        l[i] = sat24(l[i]);
        r[i] = sat24(r[i]);
    }
}

/* ---- the sound. dx_compute adds DX_N mono samples; one carrier at full level is buf >> 12 = 8192,
 * so OUT_SHIFT 13 would let eight of them reach full scale together -- but eight real voices have more
 * than one carrier each, so 14 is where a full chord stops clipping. The DC blocker is on the sum: FM
 * with feedback is not symmetric, and the offsets of the voices add.
 * Everything from the sum on is Q23 (+-8388607: 24 bits, Q15 << 8). The headroom a chord needs leaves
 * one soft note far under full scale, and at 16 bits its tail ran out of steps and turned to grit;
 * the 8 bits under Q15 carry it to the DAC, which takes 24. */
#define OUT_SHIFT 14

/* ---- 2x oversampling. FM puts partials far above 22 kHz, and at 44.1 kHz they fold back down as
 * tones that are not in the harmony: the hard, glassy edge of a bright patch. SAMPLE RATE 88.2K renders the
 * operators at 88.2 kHz (dx_os) and this takes the sum back down: a half-band FIR, 63 taps (Kaiser,
 * beta 8), flat to 0.04 dB at 19 kHz, and anything that would fold below 18 kHz is 81 dB down. Every
 * other tap but the middle one is zero, and the rest are symmetric: 16 multiplies a sample. */
static const int32_t HB_Q[16] = {      /* Q24: the taps 1, 3, 5 .. 31 either side of the middle (0.5) */
    5319600, -1718700, 968449, -628967, 430129, -298713, 206621, -140529,
    93007, -59298, 36002, -20503, 10706, -4925, 1829, -403};
#define HB_H 62u                                /* the history: the taps less one */
static int32_t hb_x[HB_H + 2u * DX_N];
static volatile uint8_t os_want;                /* SAMPLE RATE 88.2K, read by the audio ISR once a render */
/* SAMPLE RATE under 44.1 kHz: the voices held, each value kept until the slower clock ticks again,
 * as a DAC running at that rate would. Nothing stops what is over half the rate first, so it folds
 * back down, the grainy top of the old samplers and the sound cards. */
#define SR_N 7u
#define SR_OS (SR_N - 1u)                       /* the top step: 88.2 kHz, oversampled */
static const uint16_t SR_HZ[SR_N] = {8000, 11025, 16000, 22050, 32000, 44100, 44100};
static volatile uint16_t sr_hz = 44100;         /* ..as the ISR reads it: under FS is held */
static const uint8_t CRUSH_SH[4] = {16, 12, 8, 0};   /* BIT DEPTH 8 12 16 24: the bits under it, dropped */
static volatile uint8_t crush_sh;               /* ..as the ISR reads it */
static volatile uint8_t out_mono;               /* OUTPUT MONO: both sides the sum, for a mono plug */

static void hb_down(int32_t *x)                 /* x[2 * DX_N] at 88.2 kHz, the sum -> x[DX_N], Q23 */
{
    uint32_t i, j;
    for (i = 0; i < 2u * DX_N; i++)
        hb_x[HB_H + i] = x[i] >> (OUT_SHIFT - 8);
    for (i = 0; i < DX_N; i++) {
        const int32_t *c = &hb_x[32u + 2u * i];
        int64_t a = (int64_t)c[0] << 23;
        for (j = 0; j < 16u; j++)
            a += (int64_t)HB_Q[j] * (c[-(int32_t)(2u * j + 1u)] + c[2u * j + 1u]);
        x[i] = (int32_t)((a + (1 << 23)) >> 24);
    }
    for (i = 0; i < HB_H; i++)
        hb_x[i] = hb_x[2u * DX_N + i];
}

static void fm_midi_prepare(flt_t *filter, fx_t *effects);

static void app_audio(int32_t *out, uint32_t n)
{
    static int32_t mono[2 * DX_N], one[2 * DX_N];
    static int32_t dc_x, dc_y;
    static int32_t f_ic1, f_ic2;                /* the filter's two integrators */
    static int32_t sr_held;                     /* SAMPLE RATE: the value being held.. */
    static uint32_t sr_acc;                     /* ..and how far the slow clock is to its next tick */
    uint32_t hz = sr_hz;
    flt_t ft = fbuf[fcur];
    fx_t live_effects;
    const fx_t *xf = &live_effects;
    const play_t *pq = &plbuf[plcur];
    uint32_t b, v, i, lit = 0, peak = 0, t0 = fm1_ticks(), tick, start;
    cpu_profile_t parts = {0};
    uint32_t ge;
    uint32_t os = os_want ? 1u : 0u, m = DX_N << os, lg = DX_LG_N + os;
    int32_t g0, controller_pitch;

    cpu_reset_pending();
    mi_audio_sync(); fm_midi_prepare(&ft,&live_effects);
    ev_drain();
    if (midi_silence) {
        dc_x = dc_y = f_ic1 = f_ic2 = sr_held = 0; sr_acc = 0;
        memset(hb_x,0,sizeof hb_x); memset(&xs,0,sizeof xs);
        xs.ph_a = PH_A[PH_LO >> 8];
        memset(sq_left,0,sizeof sq_left); arp_note = 0xFFu; clk_a = 0;
        midi_silence = 0;
    }
    if (!ft.type)                               /* off is off: nothing kept to ring when it comes back */
        f_ic1 = f_ic2 = 0;
    if (os && !dx_os)                           /* switched on: no stale history to play out */
        for (i = 0; i < HB_H; i++)
            hb_x[i] = 0;
    dx_os = (uint8_t)os;
    tick = fm1_ticks(); parts.control = tick-t0;

    for (b = 0; b < n; b += DX_N) {
        mi_tick(DX_N); fm_midi_prepare(&ft,&live_effects);
        ge = !genv_idle(); controller_pitch = mi_pitch()*65536/12;
        pl_tick(pq);                            /* the arpeggiator and the phrase, on the block */
        start = fm1_ticks(); parts.control += start-tick;
        for (i = 0; i < m; i++)
            mono[i] = 0;
        for (v = 0; v < NV; v++) {
            if (!dxv[v].on)
                continue;
            dxv[v].bright = midi_brightness; dxv[v].controller_vibrato = midi_vibrato;
            dxv[v].controller_bias = midi_bias; dxv[v].controller_mutes = opmute;
            /* the carriers are done, or the global envelope has closed over them: either way the
             * voice is free again. The envelope is always run, even when it is doing nothing, so
             * that turning it up under a held note finds it where the note left it. */
            g0 = genv[v].g >> 12;               /* Q12, where the block starts */
            genv_block(v);
            if (!dx_playing(&dxv[v]) || genv[v].st == GS_OFF) {
                dxv[v].on = 0;
                continue;
            }
            lit |= 1u << v;
            if (!ge) {
                dx_compute(&dxv[v], mono, controller_pitch);
                continue;
            }
            {   /* one voice on its own, then faded in as it is added. The gain is interpolated
                 * across the block, because a block is 0.7 ms and a step every 0.7 ms is audible;
                 * it is carried shifted up by lg so the step divides without a division. */
                int32_t g = g0 << lg, dg = (genv[v].g >> 12) - g0;
                for (i = 0; i < m; i++)
                    one[i] = 0;
                dx_compute(&dxv[v], one, controller_pitch);
                for (i = 0; i < m; i++) {
                    mono[i] += ((one[i] >> 8) * (g >> lg)) >> 4;
                    g += dg;
                }
            }
        }
        if (os)
            hb_down(mono);
        else
            for (i = 0; i < DX_N; i++)
                mono[i] >>= OUT_SHIFT - 8;      /* Q23: the 8 bits under Q15 are kept */
        tick = fm1_ticks(); parts.synth += tick-start; start = tick;
        for (i = 0; i < DX_N; i++) {
            int32_t s = mono[i];
            /* a ~34 Hz high pass. The feedback truncates towards zero: >> would floor it, and a
             * negative state then feeds itself exactly back and the offset sits there for ever. */
            int64_t fb = (int64_t)dc_y * 32610;
            int32_t y = s - dc_x + (int32_t)((fb + (fb < 0 ? 32767 : 0)) >> 15);
            dc_x = s;
            y = sat24(y);
            dc_y = y;
            if (hz < FS) {                      /* SAMPLE RATE: hold until the slower clock ticks */
                sr_acc += hz;
                if (sr_acc >= FS) {
                    sr_acc -= FS;
                    sr_held = y;
                }
                y = sr_held;
            }
            if (crush_sh) {                     /* BIT DEPTH: rounded to the coarser step, against full
                                                 * scale, as an old DAC would. The voices only: the
                                                 * filter and the effects after it stay smooth. */
                uint32_t sh = crush_sh;
                y = sat24((int32_t)(((int64_t)y + ((int64_t)1 << (sh - 1u))) >> sh) << sh);
            }
            if (ft.type) {                      /* the filter, on the finished sound, so the spectrum
                                                 * shows what it did. A state-variable filter in the
                                                 * Zavalishin form: stable however it is turned, and
                                                 * one pass gives all three of its outputs. */
                int32_t x = y, v3 = x - f_ic2, v1, v2;
                v1 = (int32_t)(((int64_t)ft.a1 * f_ic1 + (int64_t)ft.a2 * v3) >> 30);
                v2 = f_ic2 + (int32_t)(((int64_t)ft.a2 * f_ic1 + (int64_t)ft.a3 * v3) >> 30);
                f_ic1 = 2 * v1 - f_ic1;
                f_ic2 = 2 * v2 - f_ic2;
                x = ft.type == 1u ? v2 :
                    ft.type == 2u ? (int32_t)(((int64_t)ft.k * v1) >> 30) :
                    x - (int32_t)(((int64_t)ft.k * v1) >> 30) - v2;
                y = sat24(x);
            }
            if (sp_want)
                sp_feed(y >> 8);                /* the analyser's tap, before the effects: Q15 */
            mono[i] = y;
        }
        tick = fm1_ticks(); parts.post += tick-start; start = tick;
        fx_run(xf,mono,one);
        tick = fm1_ticks(); parts.fx += tick-start; start = tick;
        for (i = 0; clk_a > 0 && i < DX_N; i++) {   /* the click, after everything: not reverberated,
                                                     * and not in the analyser or the scope */
            int32_t c = clk_ph >> 31 ? clk_a : -clk_a;     /* (Q8 of Q15 is Q23) */
            mono[i] += c;
            one[i] += c;
            clk_ph += clk_inc;
            clk_a -= CLK_A0 / CLK_LEN;
        }
        for (i = 0; i < DX_N; i++) mi_stereo(&mono[i],&one[i]);
        if (out_mono)                           /* a mono plug hears the tip, the left: give it both
                                                 * sides, halved, so a centred sound keeps its level */
            for (i = 0; i < DX_N; i++)
                mono[i] = one[i] = (mono[i] + one[i]) >> 1;
        for (i = 0; i < DX_N; i++) {
            {
                uint32_t l = (uint32_t)(mono[i] < 0 ? -mono[i] : mono[i]);
                uint32_t r = (uint32_t)(one[i] < 0 ? -one[i] : one[i]);
                if (l > peak) peak = l;
                if (r > peak) peak = r;
            }
            out[2u * (b + i)] += mono[i];
            out[2u * (b + i) + 1u] += one[i];
        }
        tick = fm1_ticks(); parts.post += tick-start;
    }
    v_lit = lit;
    if (!lit && peak < 256u && ev_r == ev_w) {
        if (midi_quiet_blocks < 20u) midi_quiet_blocks++;
    } else midi_quiet_blocks = 0;

    start = fm1_ticks(); parts.post += start-tick;
    cpu_report(start-t0,&parts);
}

/* ---- the parameters under the knobs.
 * A page is a flat list; the knobs show four at a time and SELECT steps to the next four. `op` entries
 * are an offset inside the selected operator's 21 bytes, `g` entries an index into the 156.
 * Operators are stored OP6 first, so operator n lives at (6 - n) * 21. */
static const uint16_t SP_SPAN[5] = {1000, 2000, 5000, 10000, 20000};    /* the spectrum's top end */
#define SP_NSPAN (sizeof SP_SPAN / sizeof SP_SPAN[0])

enum { P_G = 0, P_OP = 1, P_UI = 2 };
enum { K_NUM = 0, K_RAT, K_DET, K_MODE, K_ONOFF, K_CURVE, K_WAVE, K_ALG, K_BRK, K_TRSP, K_SPAN,
       K_TIME,                                  /* a DX7 rate, said as the time it takes: 99 - rate */
       K_DEC,                                   /* the same, and it writes the next rate with it */
       K_SUS,                                   /* a level, and it writes the one before it with it */
       K_MS,                                    /* 0..99, said as the time it really is */
       K_MSO,                                   /* the same, and 99 of it is OFF rather than 2s */
       K_MSR,                                   /* and this one is OFF at both ends of the knob */
       K_HZ,                                    /* the filter's cutoff, in hertz */
       K_FTYPE,                                 /* and which of its three it is, or off */
       K_OPWAVE,                                /* an operator's wave: SINE, or seven that are not */
       K_FXHZ,                                  /* an effect's LFO, in hertz */
       K_DLYT,                                  /* the delay's TIME, in ms */
       K_RT,                                    /* the reverb's DECAY, in seconds */
       K_PRE,
       K_BPM,                                   /* TEMPO: 40 + 2 a step */
       K_APAT,                                  /* the arpeggiator's pattern */
       K_AOCT,                                  /* ..and how many octaves it climbs */
       K_BARS,                                  /* the phrase: 1, 2 or 4 bars */
       K_DIV,                                   /* ..and what a note recorded into it snaps to */
       K_PCT,                                   /* 1..100 % */
       K_ARATE,                                 /* the arpeggiator's step, as a note value */
       K_SWING,                                 /* 50..75 %: how far the off-beat step is pushed */
       K_VEL,                                   /* 0..99, said as the MIDI velocity it is */
       K_PLVL,                                  /* a pitch envelope level, said in semitones */
       K_MCH,                                   /* a MIDI channel, or ALL of them */
       K_BITS,                                  /* BIT DEPTH: 8 12 16 24 */
       K_SRATE,                                 /* SAMPLE RATE: 8K .. 44.1K, and 88.2K oversampled */
       K_STEREO };                              /* OUTPUT: STEREO or MONO */

/* The handful of switches that belong to the instrument and not to the sound. They sit in their
 * own bytes rather than in the 156, because the 156 is a DX7 voice and has no room for our
 * opinions; they are saved with the octave and the rest. */
enum { UI_SPAN = 0, UI_GATK, UI_GDEC, UI_GSUS, UI_GREL, UI_FCUT, UI_FRES, UI_FTYPE,
       UI_FX,                                   /* the effects: MIX first, then three, each */
       UI_GOFF = UI_FX + 4 * FX_N,              /* the global envelope switched out, its knobs kept;
                                                 * it starts out, so a patch is heard as it was made */
       UI_TEMPO,                                /* the clock, which both of them run on */
       UI_APAT, UI_AOCT, UI_LATCH,              /* the arpeggiator's */
       UI_BARS, UI_DIV, UI_CLICK,               /* the sequencer's */
       UI_AGATE, UI_ARATE, UI_SWING, UI_AVEL,   /* the arpeggiator's second four */
       UI_POFF,                                 /* the pitch envelope switched out, its bytes kept;
                                                 * it starts in, since it is the patch's own */
       UI_MCH, UI_PCH,                          /* MIDI: the channel (0 = all), program change on */
       UI_SRATE,                                /* the sample rate: SR_HZ's steps, so 88.2K is a full bar */
       UI_BITS,                                 /* the voices' bit depth: 8 12 16 24, so 24 is a full bar */
       UI_OUT,                                  /* the output: 0 stereo, 1 mono */
       UI_N };
#define PL2_N 4u                                /* UI_AGATE onwards */
#define PL_N 7u                                 /* UI_TEMPO onwards */
#define FX_DEFAULTS 0, 45, 40, 70,  0, 35, 70, 40,  0, 65, 35, 60,  0, 55, 30, 10
#define PLAY_DEFAULTS 40, 0, 0, 0, 0, 0, 0       /* 120 BPM, UP, one octave, unlatched; 1 bar, 1/16, no click */
#define PLAY2_DEFAULTS 59, 3, 0, 78             /* 60 %, 1/16, straight, velocity 100 */
static uint8_t ui[UI_N] = {2, 0, 50, 99, 99, 60, 0, 0, FX_DEFAULTS, 1, PLAY_DEFAULTS, PLAY2_DEFAULTS};
static uint8_t midi_ui[2][UI_N];
static volatile uint32_t midi_ui_index;
enum { FT_OFF = 0, FT_LP, FT_BP, FT_HP };

/* The ISR's copy of the global envelope. Called once a pass rather than at each of the places a
 * knob can reach those four bytes, because there are several and forgetting one would be a bug
 * you could only hear. Four byte stores: it is cheaper than remembering.
 *
 * Switched off, the ISR is handed the envelope that does nothing -- instant attack, full sustain,
 * no release -- which genv_idle recognises and skips, so off costs what it did before there was
 * one. The four knobs keep their places, so on again is the shape you left. */
static int fm_midi_dispatch(uint32_t st, uint32_t d1, uint32_t d2);
static void glo_sync(void)
{
    static const uint8_t NONE[G_N] = {0, 50, 99, 99};
    static uint32_t channel;
    uint32_t i;
    if (channel != ui[UI_MCH]) {
        channel = ui[UI_MCH];
        fm_midi_dispatch(0xB0u+(channel ? channel-1u : 0u),120,0);
        mi_reset_input(); mi_publish(); ev_push(EV_SUSTAIN,0,0);
    }
    for (i = 0; i < G_N; i++)
        gpar[i] = ui[UI_GOFF] ? NONE[i] : ui[UI_GATK + i];
    dx_peg_off = ui[UI_POFF];                   /* and the pitch envelope's switch, likewise */
    memcpy(midi_ui[midi_ui_index ^ 1u],ui,sizeof ui);
    RING_PUBLISH(); midi_ui_index ^= 1u;
}

/* ---- the filter
 *
 * One filter, on the whole instrument's output, after everything the voice does. It is not part
 * of FM -- a DX7 has none -- and that is the reason for having it: it is the subtractive way of
 * shaping a sound, sitting right next to the FM way, so you can hear what each one is good at.
 * Take the top off a bright patch with LOW PASS and then try to get the same thing by turning a
 * modulator down: the two sound different, and why they do is most of what FM is.
 *
 * It is the instrument's, like the global envelope, so it is saved with the instrument and not
 * with the voice -- a bank saved here stays a true DX7 bank. Off, it is not running at all.
 *
 * CUTOFF is a hundred steps from 20 Hz to 20 kHz, evenly spaced in pitch: a step is a little over
 * a semitone, so five of them -- one click -- is about half an octave. RESONANCE runs Q from 0.7,
 * which is no peak at all, to 12, which is a peak 21 dB tall, on the same curve, so the low half
 * of the knob is the subtle half. The tables are tan(pi f / fs) and 1 / Q, both Q28, worked out in
 * Python because the chip has no floating point to work them out with. */
static const uint32_t FLT_G[100] = {
    382456, 410095, 439732, 471510, 505585, 542122, 581300, 623309,
    668354, 716654, 768445, 823978, 883526, 947376, 1015841, 1089254,
    1167972, 1252379, 1342887, 1439935, 1543997, 1655580, 1775228, 1903523,
    2041090, 2188601, 2346773, 2516377, 2698240, 2893249, 3102354, 3326575,
    3567005, 3824816, 4101266, 4397705, 4715578, 5056437, 5421948, 5813896,
    6234196, 6684904, 7168225, 7686526, 8242347, 8838415, 9477655, 10163212,
    10898458, 11687021, 12532795, 13439967, 14413039, 15456852, 16576618, 17777946,
    19066880, 20449936, 21934144, 23527095, 25236996, 27072729, 29043918, 31161011,
    33435365, 35879351, 38506477, 41331525, 44370715, 47641908, 51164833, 54961371,
    59055893, 63475678, 68251418, 73417849, 79014534, 85086857, 91687269, 98876905,
    106727663, 115324935, 124771237, 135191075, 146737612, 159601914, 174026051, 190322048,
    208899987, 230310872, 255314164, 284988356, 320920423, 365548715, 422827096, 499626773,
    609049587, 779589572, 1087210227, 1825037845,
};
static const uint32_t FLT_K[100] = {
    379625063, 368921277, 358519291, 348410596, 338586923, 329040235, 319762722, 310746795,
    301985078, 293470404, 285195806, 277154517, 269339957, 261745734, 254365636, 247193624,
    240223832, 233450558, 226868261, 220471556, 214255211, 208214140, 202343401, 196638191,
    191093844, 185705823, 180469722, 175381255, 170436262, 165630696, 160960626, 156422232,
    152011801, 147725725, 143560498, 139512712, 135579056, 131756313, 128041354, 124431141,
    120922721, 117513222, 114199857, 110979915, 107850761, 104809835, 101854651, 98982790,
    96191903, 93479707, 90843983, 88282575, 85793388, 83374385, 81023588, 78739073,
    76518971, 74361467, 72264795, 70227240, 68247135, 66322861, 64452843, 62635551,
    60869499, 59153242, 57485377, 55864537, 54289399, 52758672, 51271106, 49825482,
    48420619, 47055367, 45728609, 44439259, 43186264, 41968598, 40785265, 39635297,
    38517753, 37431719, 36376306, 35350651, 34353916, 33385284, 32443963, 31529183,
    30640197, 29776276, 28936713, 28120823, 27327937, 26557408, 25808603, 25080912,
    24373739, 23686505, 23018648, 22369621,
};
static const uint16_t FLT_HZ[100] = {
    20, 21, 23, 25, 26, 28, 30, 33, 35, 37, 40, 43,
    46, 50, 53, 57, 61, 65, 70, 75, 81, 87, 93, 100,
    107, 114, 123, 132, 141, 151, 162, 174, 187, 200, 214, 230,
    247, 264, 283, 304, 326, 350, 375, 402, 431, 462, 495, 531,
    570, 611, 655, 702, 753, 807, 866, 928, 995, 1067, 1144, 1227,
    1316, 1411, 1513, 1622, 1739, 1865, 2000, 2145, 2300, 2466, 2644, 2835,
    3040, 3260, 3495, 3748, 4018, 4309, 4620, 4954, 5312, 5696, 6108, 6549,
    7022, 7530, 8074, 8658, 9283, 9954, 10673, 11445, 12272, 13159, 14110, 15129,
    16223, 17395, 18652, 20000,
};
static const int32_t FLT_LT[100] = {
    -38728, -38316, -37903, -37491, -37079, -36666, -36254, -35842, -35429, -35017,
    -34605, -34192, -33780, -33368, -32955, -32543, -32131, -31718, -31306, -30894,
    -30481, -30069, -29657, -29244, -28832, -28420, -28007, -27595, -27183, -26770,
    -26358, -25946, -25533, -25121, -24709, -24296, -23884, -23471, -23059, -22646,
    -22234, -21822, -21409, -20997, -20584, -20171, -19759, -19346, -18933, -18520,
    -18108, -17695, -17282, -16868, -16455, -16042, -15628, -15214, -14800, -14386,
    -13971, -13556, -13141, -12725, -12309, -11892, -11475, -11056, -10637, -10217,
    -9795, -9372, -8947, -8521, -8092, -7661, -7227, -6789, -6348, -5902,
    -5450, -4992, -4527, -4053, -3569, -3072, -2561, -2032, -1482, -905,
    -296, 354, 1055, 1825, 2685, 3671, 4841, 6300, 8266, 11327,
};
static const uint32_t FLT_EXP2[17] = {
    65536, 68438, 71468, 74632, 77936, 81386, 84990, 88752, 92682,
    96785, 101070, 105545, 110218, 115098, 120194, 125515, 131072,
};

/* 2^54 / d, by hand: the chip cannot divide 64 bits by anything. Called only when a knob moves. */
static uint32_t udiv64_32(uint64_t n, uint32_t d)
{
    uint64_t r = 0;
    uint32_t q = 0, i;
    for (i = 0; i < 64u; i++) {
        r = (r << 1) | (n >> 63);
        n <<= 1;
        q <<= 1;
        if (r >= d) {
            r -= d;
            q |= 1u;
        }
    }
    return q;
}

/* The ISR's coefficients, worked out again whenever one of the three bytes has moved:
 * a1 = 1 / (1 + g (g + k)), a2 = g a1, a3 = g a2, all Q30. */
static void flt_config(flt_t *f, uint32_t c, uint32_t r, uint32_t t)
{
    uint32_t ci = c >> 8, ri = r >> 8, g, k, d, a1, a2;
    g = FLT_G[ci]; k = FLT_K[ri];
    if (ci < 99u) g += (uint32_t)(((int64_t)(FLT_G[ci+1u]-g)*(c & 255u)) >> 8);
    if (ri < 99u) k = (uint32_t)((int32_t)k+
        (int32_t)(((int64_t)((int32_t)FLT_K[ri+1u]-(int32_t)k)*(r & 255u)) >> 8));
    d = (1u << 24)+(uint32_t)(((uint64_t)g*(g+k)) >> 32);
    a1 = udiv64_32(1ull << 54,d);
    a2 = (uint32_t)(((uint64_t)g*a1) >> 28);
    f->a1 = (int32_t)a1; f->a2 = (int32_t)a2;
    f->a3 = (int32_t)(((uint64_t)g*a2) >> 28);
    f->k = (int32_t)(k << 2); f->type = t;
}

static void flt_sync(void)
{
    static uint32_t had = ~0u;
    uint32_t c = ui[UI_FCUT] < 100u ? ui[UI_FCUT] : 99u, r = ui[UI_FRES] < 100u ? ui[UI_FRES] : 99u;
    uint32_t t = ui[UI_FTYPE] & 3u, now = c | r << 8 | t << 16;
    if (now == had)
        return;
    had = now;
    flt_config(&fbuf[fcur ^ 1u],c*256u,r*256u,t);
    fcur ^= 1u;
}

/* The effects' sixteen bytes, as the ISR wants them. An effect coming on has its lines cleared
 * first, so it never plays back what it held the last time it was on: the ISR does not touch an
 * effect that is off, and runs only while this does not, so the clear cannot race it. */
static void fx_config(fx_t *x, const uint8_t u[16])
{
    uint32_t k, now = 0;
    for (k = 0; k < FX_N; k++)
        if (u[4u * k])
            now |= 1u << k;
    x->on = now;
    x->ch_mix = u[0] * 16384 / 99;              /* the two that add a copy: 99 is half and half,
                                                 * which is as deep as a notch or a beat goes */
    x->ch_inc = FX_RATE[u[1]] * 97391u / 100u;  /* 2^32 / 44100 a hertz */
    x->ch_dep = u[2] * (265 * 256) / 99;        /* up to 6 ms of swing */
    x->ch_wid = (int32_t)(u[3] * 21691754u);    /* up to half a turn between left and right */
    x->ph_mix = u[4] * 16384 / 99;
    x->ph_inc = FX_RATE[u[5]] * 97391u / 100u;
    x->ph_span = u[6] * (192 * 256) / 99;       /* up to six octaves of sweep */
    x->ph_fb = u[7] * 24576 / 99;
    x->dl_mix = u[8] * 32767 / 99;              /* the two that add a tail: 99 is as loud as the sound */
    x->dl_len = DLY_T[u[9]];
    x->dl_fb = u[10] * 31130 / 99;
    x->dl_tone = DLY_TONE[u[11]];
    x->rv_mix = u[12] * 32767 / 99;
    for (k = 0; k < 4u; k++)
        x->rv_g[k] = RV_G[u[13] * 4u + k];
    x->rv_damp = RV_DAMP[u[14]];
    x->rv_pre = u[15] * 441 / 10;
}

static void fx_sync(void)
{
    static uint8_t had[16];
    if (!memcmp(had,&ui[UI_FX],sizeof had)) return;
    memcpy(had,&ui[UI_FX],sizeof had);
    fx_config(&xbuf[xcur ^ 1u],had); xcur ^= 1u;
}

static void fm_midi_prepare(flt_t *filter, fx_t *effects)
{
    static flt_t live_filter;
    static uint32_t filter_key = ~0u;
    uint8_t fx_values[16];
    const uint8_t *u = midi_ui[midi_ui_index];
    static const uint8_t none[G_N] = {0,50,99,99};
    uint32_t i, c, r, key;
    memcpy(midi_voice,pbuf[pcur],sizeof midi_voice);
    for (i = 0; i < 6u; i++) {
        uint32_t op = 5u-i, at = op*21u;
        midi_voice[at+16u] = (uint8_t)mi_value(13u+i,midi_voice[at+16u],0,99);
        midi_voice[at+18u] = (uint8_t)mi_value(19u+i,midi_voice[at+18u],0,31);
        midi_voice[at+19u] = (uint8_t)mi_value(25u+i,midi_voice[at+19u],0,99);
        if (opmute >> op & 1u) midi_voice[at+16u] = midi_voice[at+9u] = midi_voice[at+10u] = 0;
    }
    midi_voice[135] = (uint8_t)mi_value(31,midi_voice[135],0,7);
    midi_voice[137] = (uint8_t)mi_value(32,midi_voice[137],0,99);
    midi_voice[139] = (uint8_t)mi_value(33,midi_voice[139],0,99);
    midi_voice[140] = (uint8_t)mi_value(34,midi_voice[140],0,99);
    if (memcmp(midi_voice_seen,midi_voice,sizeof midi_voice)) {
        for (i = 0; i < NV; i++)
            if (dxv[i].on) dx_update(&dxv[i],midi_voice,tnote(vst[i].note),vst[i].vel);
        memcpy(midi_voice_seen,midi_voice,sizeof midi_voice);
    }
    midi_brightness = mi_value(MI_BRIGHTNESS,0,-65536,65536)*1024;
    midi_vibrato = mi_value(MI_VIBRATO,0,-256,256)*65536/12;
    midi_bias = mi_value(51,0,-65536,65536)*512;
    for (i = 0; i < G_N; i++)
        g_live[i] = u[UI_GOFF] ? none[i] : (uint8_t)mi_value(35u+i,u[UI_GATK+i],0,99);
    c = (uint32_t)mi_value(MI_CUTOFF,u[UI_FCUT]*256,0,99*256);
    r = (uint32_t)mi_value(MI_RESONANCE,u[UI_FRES]*256,0,99*256);
    key = c | r << 15 | (u[UI_FTYPE] & 3u) << 30;
    if (key != filter_key) { flt_config(&live_filter,c,r,u[UI_FTYPE]); filter_key = key; }
    *filter = live_filter;
    for (i = 0; i < 16u; i++) fx_values[i] = (uint8_t)mi_value(fm_midi_fx_targets[i],u[UI_FX+i],0,99);
    fx_config(effects,fx_values);
    effects->ch_mix = mi_value(MI_CHORUS,u[UI_FX]*16384/99,0,16384);
    effects->ph_mix = mi_value(MI_PHASER,u[UI_FX+4u]*16384/99,0,16384);
    effects->dl_mix = mi_value(MI_DELAY,u[UI_FX+8u]*32767/99,0,32767);
    effects->rv_mix = mi_value(MI_REVERB,u[UI_FX+12u]*32767/99,0,32767);
    effects->on = (effects->ch_mix ? 1u : 0u) | (effects->ph_mix ? 2u : 0u) |
                  (effects->dl_mix ? 4u : 0u) | (effects->rv_mix ? 8u : 0u);
}

/* What the filter does to a frequency, for drawing it. The filter is the analogue one put through
 * the bilinear transform, so what it does at f is exactly what the analogue one does at the warped
 * frequency w = tan(pi f / fs) / g -- no approximation. pos is where f is on the cutoff knob's own
 * grid, Q8, so the drawing and the knob agree by construction; below 20 Hz it carries on along the
 * line, which is what tan does down there. */
static int32_t flt_lt(int32_t pos)              /* log2 tan(pi f / fs), Q12 */
{
    int32_t i, fr;
    if (pos < 0)
        return FLT_LT[0] + (int32_t)(((int64_t)(FLT_LT[1] - FLT_LT[0]) * pos) >> 8);
    if (pos >= 99 * 256)
        return FLT_LT[99];
    i = pos >> 8;
    fr = pos & 255;
    return FLT_LT[i] + (((FLT_LT[i + 1] - FLT_LT[i]) * fr) >> 8);
}

static int32_t lg64(uint64_t x)                 /* log2, Q8 */
{
    uint32_t sh = 0;
    while (x >> 31) {                           /* sp_log2_q8 cannot take the top bit */
        x >>= 1;
        sh++;
    }
    return (int32_t)(sp_log2_q8((uint32_t)x) + (sh << 8));
}

/* The gain at pos, in sixteenths of a dB. W is w squared, Q16; with it the three responses are
 * 1, k^2 W and W^2, each over (1 - W)^2 + k^2 W. Seven octaves either side is 84 dB down even at
 * the gentlest slope, which is well off the bottom of every plot. */
static int32_t flt_db16(int32_t pos, uint32_t c, uint32_t r, uint32_t t)
{
    int32_t u = 2 * (flt_lt(pos) - FLT_LT[c < 100u ? c : 99u]), n;
    uint32_t fr, m, W, kq, kk;
    uint64_t den, num;
    int64_t d1;
    if (u > 14 * 4096) u = 14 * 4096;
    if (u < -14 * 4096) u = -14 * 4096;
    n = u >> 12;
    fr = (uint32_t)u & 4095u;
    m = FLT_EXP2[fr >> 8] + (((FLT_EXP2[(fr >> 8) + 1u] - FLT_EXP2[fr >> 8]) * (fr & 255u)) >> 8);
    W = n >= 0 ? m << n : m >> -n;
    kq = FLT_K[r < 100u ? r : 99u] >> 12;      /* Q16 */
    kk = (kq * kq) >> 16;
    d1 = (int64_t)65536 - (int64_t)W;
    den = (uint64_t)(d1 * d1) + (uint64_t)kk * W;
    num = t == FT_LP ? 1ull << 32 : t == FT_BP ? (uint64_t)kk * W : (uint64_t)W * W;
    if (!num)
        num = 1u;
    return ((lg64(num) - lg64(den)) * 12330) >> 16;   /* 10 log10(2) * 16 / 256 */
}

typedef struct {
    const char *name;
    const char *knob;                           /* six characters for the legend, or 0 for name */
    uint8_t where, idx, max, kind;
} param_t;
#define OPB_WAVE 21u                            /* the one operator byte past its 21: the wave */

static const param_t PG_OP[] = {
    {"LEVEL",  0, P_OP, 16, 99, K_NUM},         /* how loud -- or, on a modulator, how much it modulates */
    {"RATIO",  0, P_OP, 18, 36, K_RAT},         /* coarse and fine, said as the one number they mean */
    {"DETUNE", 0, P_OP, 20, 14, K_DET},
    {"WAVE",   0, P_OP, OPB_WAVE, DX_NWAVE - 1u, K_OPWAVE},   /* not in the operator's 21: see p_at */
    {"MODE",   0, P_OP, 17, 1,  K_MODE},
    {"COARSE", 0, P_OP, 18, 31, K_NUM},         /* the two bytes RATIO says in one number */
    {"FINE",   0, P_OP, 19, 99, K_NUM},
    {"VEL SENS", "V SENS", P_OP, 15, 7, K_NUM},
    {"A M SENS", "AMSENS", P_OP, 14, 3, K_NUM},   /* how hard this operator listens to the LFO */
    {"RATE SCL", "RT SCL", P_OP, 13, 7, K_NUM},
    {"BREAK PT", "BRK PT", P_OP, 8,  99, K_BRK},
    {"L DEPTH", "L DEP", P_OP, 9,  99, K_NUM},
    {"R DEPTH", "R DEP", P_OP, 10, 99, K_NUM},
    {"L CURVE", "L CRV", P_OP, 11, 3,  K_CURVE},
    {"R CURVE", "R CRV", P_OP, 12, 3,  K_CURVE},
};
/* A M SENS is the one row in this table reached by name from somewhere else -- a tap of LFO puts
 * it under the first knob -- so where it sits is not free to change. */
#define PG_OP_AMS 8u
/* ---- the envelope, said twice
 *
 * The DX7 gives an operator four segments, each a rate and the level it is heading for. It is the
 * honest shape and it is on this page, but it is not how anyone thinks about an envelope, and two
 * of its eight numbers almost never earn their keep.
 *
 * So the page leads with the four every other synth has. ATTACK is rate 1. DECAY is rate 2, and it
 * writes rate 3 with it, so a decay drawn in two stages becomes one that moves at one speed.
 * SUSTAIN is level 3 -- where the envelope actually sits while you hold the key -- and it writes
 * level 2 with it, which is what flattens stages 2 and 3 into the single decay DECAY then steers.
 * RELEASE is rate 4. Levels 1 and 4 are left alone: a patch whose attack peaks below full, or
 * whose release lands above silence, goes on doing that.
 *
 * The rates are turned round. On a DX7 a bigger rate is a faster one, which is backwards from
 * every knob labelled ATTACK ever made, so what these three show is 99 - rate: turn it up and the
 * segment takes longer. The byte underneath is untouched, and the four raw segments are still on
 * the banks after these, saying the same thing the other way. */
static const param_t PG_EG[] = {
    {"ATTACK",  "ATK", P_OP, 0, 99, K_TIME},
    {"DECAY",   "DEC", P_OP, 1, 99, K_DEC},     /* rate 2, and rate 3 with it */
    {"SUSTAIN", "SUS", P_OP, 6, 99, K_SUS},     /* level 3, and level 2 with it */
    {"RELEASE", "REL", P_OP, 3, 99, K_TIME},
    {"RATE 1",  0, P_OP, 0, 99, K_NUM},
    {"LEVEL 1", "LVL 1", P_OP, 4, 99, K_NUM},
    {"RATE 2",  0, P_OP, 1, 99, K_NUM},
    {"LEVEL 2", "LVL 2", P_OP, 5, 99, K_NUM},
    {"RATE 3",  0, P_OP, 2, 99, K_NUM},
    {"LEVEL 3", "LVL 3", P_OP, 6, 99, K_NUM},         /* where it sits while the key is held */
    {"RATE 4",  0, P_OP, 3, 99, K_NUM},
    {"LEVEL 4", "LVL 4", P_OP, 7, 99, K_NUM},         /* where it goes when you let go */
};
/* The global envelope's four, which are the same four names in the same order as the operator
 * envelope's -- deliberately, because the point of it is the comparison. They are not in the 156:
 * they live in ui[], with the spectrum span and the rest of the instrument's own opinions. */
static const param_t PG_GEG[] = {
    {"ATTACK",  "ATK", P_UI, UI_GATK, 99, K_MS},
    {"DECAY",   "DEC", P_UI, UI_GDEC, 99, K_MSO},
    {"SUSTAIN", "SUS", P_UI, UI_GSUS, 99, K_NUM},
    {"RELEASE", "REL", P_UI, UI_GREL, 99, K_MSR},
};
/* ---- the LFO, and the two numbers that decide whether anyone is listening
 *
 * A depth on its own does nothing here. The LFO has one PITCH MOD depth and one AMP MOD depth for
 * the whole voice, and then each destination has a sensitivity saying how much of that depth it
 * actually takes: one for pitch, which is the voice's, and one per operator for amplitude. Turn
 * AMP MOD to 99 on a voice whose operators are all at sensitivity 0 and nothing whatever happens,
 * which is the single most common way of concluding that a DX7's LFO is broken.
 *
 * So this page is every number the LFO has that belongs to the whole voice, and deliberately not
 * one more. The pitch half of the pair is here, because there is one of it: P MOD sets how far
 * and PMSENS how much of that the voice's pitch takes. The amplitude half is not, because there
 * are six of it -- A M SENS is an operator's byte and lives on OPERATOR with that operator's
 * other bytes, and a tap of LFO puts it under a knob on HOME.
 *
 * Splitting them that way is the whole point of the page rather than an omission. The first thing
 * worth knowing about these numbers is which of them you set once and which you set six times,
 * and a page that is all of the first kind says that without a word.
 *
 * Three to a bank, so that a name and its value both fit in a cell. The first bank is the LFO as
 * the picture draws it -- how fast, how late, what shape. The second is how deep, with P M SENS
 * beside the two depths because P MOD does nothing without it. The third is whether it restarts
 * under each note, which is the one thing about it the picture cannot show. */
static const param_t PG_LFO[] = {
    {"SPEED",   0, P_G, 137, 99, K_NUM},
    {"DELAY",   0, P_G, 138, 99, K_NUM},
    {"WAVE",    0, P_G, 142, 5,  K_WAVE},
    {"PITCH MOD", "P MOD", P_G, 139, 99, K_NUM},
    {"AMP MOD",  "A MOD", P_G, 140, 99, K_NUM},
    {"P M SENS", "PMSENS", P_G, 143, 7, K_NUM},     /* how much the voice's pitch takes of P MOD */
    {"KEY SYNC", "K SYNC", P_G, 141, 1, K_ONOFF},
};
#define PG_LFO_WAVE 2u                          /* reached by name: a tap steps it under GLOBAL */
/* ..and the four GLOBAL puts under the knobs on HOME, where the wave has the button instead */
static const uint8_t LFO_FOC[4] = {0u, 1u, 3u, 4u};   /* SPEED DELAY P MOD A MOD */
static const param_t PG_GLO[] = {
    {"ALGORITHM", "ALGO", P_G, 134, 31, K_ALG},
    {"FEEDBACK",  "FEEDBK", P_G, 135, 7,  K_NUM},
    {"OSC SYNC",  "O SYNC", P_G, 136, 1,  K_ONOFF},
    {"TRANSPOSE", "TRANSP", P_G, 144, 48, K_TRSP},
    {"P RATE 1", "PRATE1", P_G, 126, 99, K_NUM},
    {"P RATE 2", "PRATE2", P_G, 127, 99, K_NUM},
    {"P RATE 3", "PRATE3", P_G, 128, 99, K_NUM},
    {"P RATE 4", "PRATE4", P_G, 129, 99, K_NUM},
    {"P LEVEL 1", "P LVL1", P_G, 130, 99, K_PLVL},
    {"P LEVEL 2", "P LVL2", P_G, 131, 99, K_PLVL},
    {"P LEVEL 3", "P LVL3", P_G, 132, 99, K_PLVL},
    {"P LEVEL 4", "P LVL4", P_G, 133, 99, K_PLVL},
    /* the instrument's, not the voice's: how it listens over USB. ALL is every channel, as it
     * always was; a number is that one only, and the keys send on it too. PROG CHG loads voice N+1
     * of the voice list for program N, and turning PRESETS sends one the other way. */
    {"MIDI CHANNEL", "MIDICH", P_UI, UI_MCH, 16, K_MCH},
    {"PROG CHANGE",  "PRG CH", P_UI, UI_PCH, 1,  K_ONOFF},
    /* the rate the voices are made at. 88.2K renders the operators at twice 44.1 and brings them
     * back down, so the partials over 22 kHz go rather than folding back as tones that are not in
     * the harmony; 44.1K is Dexed's, and half the work. Under that the voices are held at the
     * slower rate, and fold as the old samplers and sound cards did: 32K the first digital
     * keyboards, 22K and 11K the early computers, 8K the telephone. */
    {"SAMPLE RATE",  "S RATE", P_UI, UI_SRATE, SR_N - 1u, K_SRATE},
    /* the voices cut to fewer bits before the filter and the effects: 16 is the CD, 12 the DX7's
     * DAC and the '80s samplers, 8 the home computers before them */
    {"BIT DEPTH",    "BITS",   P_UI, UI_BITS, 3, K_BITS},
    /* the last thing before the jack: MONO is left and right summed onto both, for a mono plug,
     * which only hears the left -- and so would lose whatever the effects put on the right */
    {"OUTPUT",       "OUTPUT", P_UI, UI_OUT,  1, K_STEREO},
};
#define PG_GLO_FB 1u                            /* reached by name: HOME has a knob for it */
#define PG_GLO_SRATE 14u
#define GLO_DIAGNOSTICS_BANK ((sizeof PG_GLO / sizeof PG_GLO[0] + 3u) / 4u)
/* ..and the same eight as the PIT key gives them: a rate with the level it is heading for, the way
 * the ENVELOPE page pairs an operator's, so the first four are the note's start and the second its
 * end. A level of 50 is the note's own pitch. */
static const param_t PG_PEG[] = {
    {"P RATE 1",  "RATE 1",  P_G, 126, 99, K_NUM},
    {"P LEVEL 1", "LVL 1",   P_G, 130, 99, K_PLVL},
    {"P RATE 2",  "RATE 2",  P_G, 127, 99, K_NUM},
    {"P LEVEL 2", "LVL 2",   P_G, 131, 99, K_PLVL},
    {"P RATE 3",  "RATE 3",  P_G, 128, 99, K_NUM},
    {"P LEVEL 3", "LVL 3",   P_G, 132, 99, K_PLVL},
    {"P RATE 4",  "RATE 4",  P_G, 129, 99, K_NUM},
    {"P LEVEL 4", "LVL 4",   P_G, 133, 99, K_PLVL},
};
/* the filter, and how far across the spectrum it is drawn on: the spectrum screen's four knobs */
static const param_t PG_FLT[] = {
    {"CUTOFF",    "CUT",  P_UI, UI_FCUT,  99, K_HZ},
    {"RESONANCE", "RESO", P_UI, UI_FRES,  99, K_NUM},
    {"TYPE",      0,      P_UI, UI_FTYPE, 3,  K_FTYPE},
    {"SPECTRUM",  "SPAN", P_UI, UI_SPAN,  SP_NSPAN - 1u, K_SPAN},
};
/* the effects, four to a bank and one bank each, MIX first: the knob that says whether it is on */
static const param_t PG_EFX[] = {
    {"MIX",       0,        P_UI, UI_FX + 0,  99, K_NUM},
    {"RATE",      0,        P_UI, UI_FX + 1,  99, K_FXHZ},
    {"DEPTH",     0,        P_UI, UI_FX + 2,  99, K_NUM},
    {"WIDTH",     0,        P_UI, UI_FX + 3,  99, K_NUM},
    {"MIX",       0,        P_UI, UI_FX + 4,  99, K_NUM},
    {"RATE",      0,        P_UI, UI_FX + 5,  99, K_FXHZ},
    {"DEPTH",     0,        P_UI, UI_FX + 6,  99, K_NUM},
    {"FEEDBACK",  "FEEDBK", P_UI, UI_FX + 7,  99, K_NUM},
    {"MIX",       0,        P_UI, UI_FX + 8,  99, K_NUM},
    {"TIME",      0,        P_UI, UI_FX + 9,  99, K_DLYT},
    {"FEEDBACK",  "FEEDBK", P_UI, UI_FX + 10, 99, K_NUM},
    {"TONE",      0,        P_UI, UI_FX + 11, 99, K_NUM},
    {"MIX",       0,        P_UI, UI_FX + 12, 99, K_NUM},
    {"DECAY",     0,        P_UI, UI_FX + 13, 99, K_RT},
    {"DAMP",      0,        P_UI, UI_FX + 14, 99, K_NUM},
    {"PRE-DELAY", "PRE",    P_UI, UI_FX + 15, 99, K_PRE},
};
static const char *const FX_NAME[FX_N] = {"CHORUS", "PHASER", "DELAY", "REVERB"};
/* a tap of FX: how much of each, side by side -- the mixer, with the rest of each on its page */
static const param_t PG_MIX[] = {
    {"CHORUS MIX", "CHORUS", P_UI, UI_FX + 0,  99, K_NUM},
    {"PHASER MIX", "PHASER", P_UI, UI_FX + 4,  99, K_NUM},
    {"DELAY MIX",  "DELAY",  P_UI, UI_FX + 8,  99, K_NUM},
    {"REVERB MIX", "REVERB", P_UI, UI_FX + 12, 99, K_NUM},
};

/* ..ARP's and SEQ's: the clock both of them run on, then what is each one's own. */
static const param_t PG_ARP[] = {
    {"TEMPO",       0,         P_UI, UI_TEMPO, 99, K_BPM},
    {"ARP PATTERN", "PATTERN", P_UI, UI_APAT,  4,  K_APAT},
    {"ARP OCTAVES", "OCTAVE",  P_UI, UI_AOCT,  3,  K_AOCT},
    {"ARP LATCH",   "LATCH",   P_UI, UI_LATCH, 1,  K_ONOFF},
};
static const param_t PG_ARP2[] = {             /* ..and SELECT's second four */
    {"ARP GATE",    "GATE",    P_UI, UI_AGATE, 99, K_PCT},
    {"ARP RATE",    "RATE",    P_UI, UI_ARATE, 5,  K_ARATE},
    {"ARP SWING",   "SWING",   P_UI, UI_SWING, 25, K_SWING},
    {"ARP VELOCITY", "VELO",   P_UI, UI_AVEL,  99, K_VEL},
};
static const param_t PG_SEQ[] = {
    {"TEMPO",       0,         P_UI, UI_TEMPO, 99, K_BPM},
    {"SEQ BARS",    "BARS",    P_UI, UI_BARS,  2,  K_BARS},
    {"SEQ DIV",     "DIV",     P_UI, UI_DIV,   2,  K_DIV},
    {"CLICK",       0,         P_UI, UI_CLICK, 1,  K_ONOFF},
};

enum { PAGE_HOME = 0, PAGE_OP, PAGE_EG, PAGE_LFO, PAGE_GLO, PAGE_FX, PAGE_STEP };
/* rows: how many parameters a bank shows at once, and y0 where the first one sits. The envelope
 * page shows only two, because the graph above them is worth more than two more numbers -- and a
 * rate with the level it is heading for is the pair that actually means something together. */
static const struct { const char *name; const param_t *p; uint8_t n, rows, y0; } PAGE[] = {
    {"FM LAB",    0,      0,                                4, 56},
    {"OPERATOR",  PG_OP,  sizeof PG_OP / sizeof PG_OP[0],   4, 56},
    {"ENVELOPE",  PG_EG,  sizeof PG_EG / sizeof PG_EG[0],   2, 124},
    {"LFO",       PG_LFO, sizeof PG_LFO / sizeof PG_LFO[0], 3, 56},
    {"GLOBAL",    PG_GLO, sizeof PG_GLO / sizeof PG_GLO[0], 4, 56},
    {"EFFECTS",   PG_EFX, sizeof PG_EFX / sizeof PG_EFX[0], 4, 56},
    {"STEPS",     0,      0,                                4, 56},   /* the phrase, drawn: its own */
};
#define PAGE_ROWS ((uint32_t)PAGE[page].rows)
#define HOME_NBANK 3u                           /* HOME's screens: the diagram, the spectrum, the wave */

static uint8_t page, bank, opsel = 5;           /* opsel is the engine's operator index: 0 = OP6 .. 5 = OP1 */
static uint8_t gsel;                            /* ..or 1, the global envelope, which is not an operator,
                                                 * or 2, the pitch envelope, which is not one either */
enum { FOC_OFF = 0, FOC_EDIT, FOC_ENV, FOC_LFO, FOC_FX, FOC_SEQ, FOC_ARP };  /* a tap puts the top of a page under the knobs */
static uint8_t focus;
#define LONG_MS 400u                            /* longer than a tap, shorter than a wait */
#define FOC_N 4u
static const uint8_t FOC_BTN[FOC_N] = {BTN_EDIT, BTN_ENV, BTN_LFO, BTN_FX};
static const uint8_t FOC_MODE[FOC_N] = {FOC_EDIT, FOC_ENV, FOC_LFO, FOC_FX};
static const uint8_t FOC_PAGE[FOC_N] = {PAGE_OP, PAGE_EG, PAGE_LFO, PAGE_FX};
static const char *const FOC_NAME[] = {"EDIT", "ENV", "LFO", "FX", "SEQ", "ARP"};
static uint8_t fxsel;                           /* the effect the EFFECTS page last showed */
static uint8_t arp_pg;                          /* ARP's knobs: the first four, or SELECT's second */
static uint8_t pit_pg;                          /* the pitch envelope's on HOME: segments 1-2, or 3-4 */
static uint8_t stc;                             /* the step view's cursor */

/* ---- a few things under the four knobs, without leaving the screen you play on
 *
 * The pages are the right way to read a sound and the wrong way to shape one: the moment you are
 * listening, a full-screen list of numbers is in the way. So EDIT, ENV and LFO answer to how long
 * you hold them. Hold one and you get its page, as ever. Tap it and you stay on HOME -- diagram or
 * spectrum, still playing -- and the four knobs swing round onto the top of that page.
 *
 * Tapping EDIT gives one operator's timbre: level, ratio, detune and wave -- and while it does, that
 * operator's box on the diagram draws its wave, as tall as its LEVEL. Tapping ENV gives its envelope: attack, decay, sustain,
 * release -- which is exactly four, and is the reason those four had to exist before this did.
 * Tapping LFO gives one knob rather than four: this operator's A M SENS, which is how much of the
 * LFO's amplitude depth it takes. One knob looks thin until you count what it is worth. It is the
 * only LFO number there are six of, and carrying it from operator to operator is what turns one
 * LFO into tremolo, or into a wobble in brightness, or into both at once -- which is the thing
 * about amplitude modulation on an FM synth that is worth finding out early.
 *
 * EDIT and ENV are the first few rows of the page the button would have opened. LFO is not, and
 * it is the one place the shortcut is deliberately not the top of the menu. The LFO page is every
 * number that belongs to the whole voice; the one you reach for with a note sounding is the one
 * that does not. Putting both under the same four knobs was what made it hard to see which of
 * them you set once and which you set six times.
 *
 * The voice's half is still one press away, by the key that means "all six of them" everywhere
 * else: GLOBAL swaps the single operator knob for the LFO itself -- speed, delay, and the two
 * depths -- and puts its picture where the diagram was, exactly as it does for the envelope. Any
 * OP key brings you back to that operator's ear on it.
 *
 * SELECT steps the operator under all three, because the four knobs are fixed and the operator is
 * what varies. Pressing the button again does the same as SELECT, as it always has -- except on the
 * global LFO, where there is no operator to step and the button steps the wave instead, with its
 * picture right there to show what it became; and on the global envelope, where it switches the
 * envelope in and out of the sound, on HOME and on its page alike, keeping its four knobs.
 *
 * The black keys the panel prints OP1..OP6 under pick the operator instead of sounding whenever
 * the knobs have an operator's byte under them -- under EDIT and ENV always, under LFO until
 * GLOBAL takes the knobs off that operator. That makes some of the keyboard stop playing, so the
 * title says EDIT OP3 or LFO OP3 exactly when it does, and LFO GLOBAL when it does not.
 *
 * FX is the fourth: the four effects' MIXes, one knob each, so the knobs are a small mixer and
 * the rest of each effect waits on its page. There is nothing to step, so a second tap and SELECT do
 * nothing. None of it is an operator's,
 * so the black keys go on playing. The screen stays as it was: the effects come after the
 * analyser's tap, so the spectrum and the wave show the sound going into them.
 *
 * The filter is not under a button at all. It lives on the spectrum screen, whose four knobs are
 * CUTOFF, RESONANCE, TYPE and the spectrum's own span whenever nothing else has them -- because
 * the sound it is cutting is the one place you can watch it work. */
static uint32_t home_flt(void)                  /* the spectrum screen, with the filter under the knobs */
{
    return page == PAGE_HOME && bank == 1u && focus == FOC_OFF;
}

/* what knob e moves while focused, or 0 for "this knob does nothing" */
static const param_t *focus_at(uint32_t e)
{
    if (home_flt())
        return e < 4u ? &PG_FLT[e] : 0;
    if (focus == FOC_EDIT)
        return e < 4u ? &PG_OP[e] : 0;          /* LEVEL RATIO DETUNE WAVE */
    if (focus == FOC_ENV)                       /* ATTACK DECAY SUSTAIN RELEASE, of one of the
                                                 * operators or of the whole voice */
        return e < 4u ? (gsel == 2u ? &PG_PEG[pit_pg * 4u + e] : gsel ? &PG_GEG[e] : &PG_EG[e]) : 0;
    if (focus == FOC_LFO)                       /* the LFO itself, or this operator's ear on it */
        return gsel == 1u ? (e < 4u ? &PG_LFO[LFO_FOC[e]] : 0)
                    : (e < 1u ? &PG_OP[PG_OP_AMS] : 0);
    if (focus == FOC_FX)                        /* the four MIXes, one knob an effect */
        return e < 4u ? &PG_MIX[e] : 0;
    if (focus == FOC_SEQ)                       /* the clock and the sequencer */
        return e < 4u ? &PG_SEQ[e] : 0;
    if (focus == FOC_ARP)                       /* the clock and the arpeggiator */
        return e < 4u ? (arp_pg ? &PG_ARP2[e] : &PG_ARP[e]) : 0;
    return 0;
}

/* Do the focus knobs have a byte belonging to one operator under them? It decides several things
 * at once -- whether the black keys point, whether the title names an operator, whether the diagram
 * rings it and draws its envelope in its box, and whether SEL switches it -- and they must agree, because the title is
 * the only warning that part of the keyboard has stopped playing. */
static uint32_t foc_op(void)
{
    uint32_t e;
    const param_t *p;
    for (e = 0; e < 4u; e++)
        if ((p = focus_at(e)) != 0 && p->where == P_OP)
            return 1u;
    return 0u;
}

/* SELECT, and a second press of the button that got you here, both mean "the next operator" --
 * which is the only thing any of the three has more than one of. d is how many, and which way. */
static void foc_step(int32_t d)
{
    int32_t o;
    if (focus == FOC_ARP && (d & 1))            /* ARP has two sets of four, and no operator */
        arp_pg ^= 1u;
    if (focus == FOC_ENV && gsel == 2u) {       /* ..and so has the pitch envelope */
        if (d & 1)
            pit_pg ^= 1u;
        return;
    }
    if (focus >= FOC_FX)                        /* ..FX and SEQ, one set and no operator */
        return;
    o = (int32_t)opsel - d % 6;                 /* forward: OP1 -> OP2 -> .. */
    opsel = (uint8_t)((o + 6) % 6);
}


/* One more black key points instead of playing: the one the panel prints GLOBAL under. It picks
 * the global envelope in place of an operator, and it does so only where there is an envelope on
 * the screen to replace -- the ENVELOPE page, and the ENV knobs on HOME. Everywhere else it is an
 * ordinary A#4, because a key that means something nowhere in particular means nothing. */
#define GLO_KEY 17u                             /* A#4 */
static uint32_t glo_point(void)
{
    return page == PAGE_EG
        || ((focus == FOC_ENV || focus == FOC_LFO) && page == PAGE_HOME);
}

static uint32_t eg_glo(void)                    /* ..and it is the global envelope that is showing */
{
    return gsel == 1u && (page == PAGE_EG || (focus == FOC_ENV && page == PAGE_HOME));
}

/* The black key between OP6 and GLOBAL, which the panel prints PIT under: the stock firmware's
 * "edit the pitch envelope", and ours. It points wherever the OP keys do, and takes you to the
 * envelope that bends the note's pitch rather than an operator's level -- on HOME by putting it
 * under the ENV knobs, from a page by opening ENVELOPE on it. Its eight bytes are the voice's, a
 * DX7's own, so they also stay on GLOBAL where the panel's manual put them. */
#define PIT_KEY 15u                             /* G#4 */
static uint32_t eg_pit(void)                    /* the pitch envelope is the one showing */
{
    return gsel == 2u && (page == PAGE_EG || (focus == FOC_ENV && page == PAGE_HOME));
}

/* ..or the LFO itself in place of one operator's share of it. One flag, because GLOBAL asks the
 * same question wherever it is worth asking -- all six of them rather than this one -- and only
 * one of the two can be on the screen to ask it of. */
static uint32_t lfo_glo(void)
{
    return gsel == 1u && focus == FOC_LFO && page == PAGE_HOME;
}

static uint32_t flt_shown(void)                 /* the filter's curve: always, over the spectrum,
                                                 * which is where it lives -- flat when it is off */
{
    return bank == 1u;
}

/* ---- a click, and what it is worth
 *
 * Almost everything here is a 0..99 byte, and one of those on its own is not a thing you can hear.
 * At a click a detent that is a hundred clicks of wrist to cross a range, which is the wrong
 * resolution for the question you ask most of the time while you are learning -- what is down the
 * other end of this? So a click of those is worth five, and SEL is a shift key the other way: hold
 * it and you get the single step back for landing on a value.
 *
 * Only the 0..99 bytes. Everything else on the instrument is a list of choices rather than a range
 * -- thirty-two algorithms, the ratios, the curves, the switches -- and there a click is already
 * worth exactly one meaningful thing, so it stays one.
 *
 * SEL is the right button for it because it already sits under the thumb while the other hand is
 * on the knobs, and because its own jobs (switch the selected operator off, or solo it) are instant
 * and so have nothing to lose by waiting for the release. A knob moved while it was down cancels
 * them, so the shift and the switch never both happen on the one press. */
#define COARSE 5
static uint8_t sel_used;                        /* a knob moved while SEL was down */
static uint32_t t_sel;                          /* ..and when it went down: a tap mutes, a hold solos */

static int sel_fine(void)
{
    return (fm1_in.buttons & (1u << BTN_SEL)) != 0;
}

static int32_t p_step(const param_t *p)
{
    return p && p->max == 99 ? COARSE : 1;
}

/* Every knob read goes through here, so the step applies to the parameter knobs and only to them:
 * ALGORITHM has its own encoder and a list of 32, and SELECT walks lists. */
static int32_t knob_take(uint32_t e, int32_t step)
{
    int32_t d = fm1_enc_take(ENC_KNOB1 + e);
    if (d && sel_fine()) {
        sel_used = 1;
        step = 1;
    }
    return d * step;
}

/* A page is a flat list of parameters; these two just save repeating the table lookup, and they
 * are what every bank walker goes through. */
static uint32_t page_n(void)
{
    return eg_glo() ? 4u : eg_pit() ? 8u : PAGE[page].n;
}

static const param_t *page_at(uint32_t want)
{
    if (eg_glo())                               /* the global envelope is four rows and no more */
        return want < 4u ? &PG_GEG[want] : 0;
    if (eg_pit())                               /* the pitch envelope, two segments a bank */
        return want < 8u ? &PG_PEG[want] : 0;
    return want < PAGE[page].n ? &PAGE[page].p[want] : 0;
}

/* ---- what a bank is
 *
 * On four of the five pages a bank is simply the next `rows` parameters. ENVELOPE is the exception:
 * its first bank is all four of ADSR at once, in a single row of four cells, and the banks after it
 * are the raw segments two at a time. So a bank is a span rather than a fixed stride, and anything
 * that walks one has to ask how long it is and where it starts. */
static uint32_t bank_rows(uint32_t b)
{
    if (eg_glo() || eg_pit())                   /* four: it has no raw segments underneath */
        return 4u;
    if (page == PAGE_EG)
        return b == 0u ? 4u : 2u;
    return PAGE_ROWS;
}

static uint32_t bank_first(uint32_t b)
{
    if (eg_glo())
        return 0u;
    if (eg_pit())
        return b * 4u;
    if (page == PAGE_EG)
        return b == 0u ? 0u : 4u + (b - 1u) * 2u;
    return b * PAGE_ROWS;
}

static uint32_t page_nbank(void)
{
    uint32_t b = 0, i = 0, n = page_n();
    while (i < n && b < 32u)
        i += bank_rows(b++);
    if (page == PAGE_GLO) b++;
    return b ? b : 1u;
}

static int glo_diagnostics(void)
{
    return page == PAGE_GLO && bank == GLO_DIAGNOSTICS_BANK;
}

/* ---- the four knobs on HOME
 *
 * The algorithm diagram and the wave are the screens you look at while listening, and neither had
 * anything under the knobs. (The spectrum has: the filter's, below.) Balancing the operators against each other is most of
 * what making an FM sound is, so that is what they do: four operators at a time, levels or ratios.
 * Turn one with a note held and you watch its box fill as you hear it change, which is the whole
 * loop in one gesture.
 *
 * SELECT steps which four -- the same thing it means on every other page, the next four things
 * under the knobs -- so there is nothing new to learn, and HOME's own button goes on stepping the
 * screens. Four knobs and six operators do not divide, so the sets come in pairs: operators 1 to 4,
 * then 5 and 6 with FEEDBACK on the third knob and the fourth spare. The legend names every knob, so which set you are on is never
 * something you have to carry in your head. */
enum { HK_LVL = 0, HK_RAT };
static const struct { uint8_t what, op0; } HSET[] = {
    {HK_LVL, 1}, {HK_LVL, 5}, {HK_RAT, 1}, {HK_RAT, 5},
};
#define HSET_N (sizeof HSET / sizeof HSET[0])
static uint8_t homeset;

/* which operator knob e is on, as the panel numbers them, or 0 for "this knob does nothing" */
static uint32_t home_op(uint32_t e)
{
    uint32_t n = (uint32_t)HSET[homeset].op0 + e;
    return n <= 6u ? n : 0u;
}

/* ..and the third knob of the 5-and-6 sets, which would otherwise do nothing, is FEEDBACK: the
 * one number the diagram already draws (the bracket round the operator modulating itself) and the
 * one most worth reaching for while you are balancing levels -- 0 is a sine, 7 is nearly a saw. */
static uint32_t home_fb(uint32_t e)
{
    return e == 2u && HSET[homeset].op0 == 5u;
}

/* Go to a page; pressing the button for the page you are already on walks its banks and wraps. */
static void page_to(uint8_t p)
{
    if (page != p) {
        page = p;
        bank = 0;
    } else if (page == PAGE_HOME) {
        bank = (uint8_t)((bank + 1u) % HOME_NBANK);
    } else if (page_n()) {
        bank = (uint8_t)((bank + 1u) % page_nbank());
    }
}

/* The SELECT encoder: the next four things under the knobs, d of them and either way round -- the
 * page's next bank, HOME's next set of operators, or, focused, the next operator. It used to pick
 * the operator outright; the OP1..OP6 keys do that wherever there is one to pick, and a knob that
 * walks every list on the instrument is worth more than a second way to do the one thing. */
static uint8_t wrap_n(int32_t v, uint32_t n)
{
    v %= (int32_t)n;
    return (uint8_t)(v < 0 ? v + (int32_t)n : v);
}

static void sel_step(int32_t d)
{
    if (page == PAGE_STEP) {                    /* the step view: the cursor, round the loop */
        stc = wrap_n((int32_t)stc + d, 16u << ui[UI_BARS]);
        return;
    }
    if (home_flt())
        return;                                 /* one filter: there is no next one */
    if (focus != FOC_OFF && page == PAGE_HOME)
        foc_step(d);
    else if (page == PAGE_HOME)
        homeset = wrap_n((int32_t)homeset + d, HSET_N);
    else if (page_n())
        bank = wrap_n((int32_t)bank + d, page_nbank());
}

/* Does the bank showing have a row that belongs to one operator rather than to the whole voice?
 * Every bank of OPERATOR and ENVELOPE does; of the LFO page's two, only the one holding A M SENS.
 * Asking the bank rather than the page is what lets a single operator row live on a page that is
 * otherwise the voice's without making the whole page behave as though it were an operator's. */
static uint32_t bank_has_op(void)
{
    uint32_t i, f = bank_first(bank), n = bank_rows(bank);
    const param_t *p;
    for (i = 0; i < n; i++)
        if ((p = page_at(f + i)) != 0 && p->where == P_OP)
            return 1u;
    return 0u;
}

/* The panel prints OP1..OP6 under the low six black keys, and the stock firmware uses them that
 * way. Wherever the knobs are on one operator's bytes -- every bank of OPERATOR and ENVELOPE, the
 * and a tap of EDIT, ENV or LFO -- so do we: the key picks the operator instead of sounding,
 * because there you are pointing at one rather than playing it. The two global pictures count too,
 * or there would be no way back off them. Everywhere else they are ordinary
 * keys. Returns the engine's operator index (0 = OP6 .. 5 = OP1, as opsel counts), or -1 for
 * "this is a note". */
static const uint8_t OP_KEY[6] = {1, 3, 5, 8, 10, 13};   /* F#3 G#3 A#3 C#4 D#4 F#4 = OP1..OP6 */
static uint32_t op_point(void)                  /* are the black keys pointing rather than playing? */
{
    return bank_has_op() || eg_glo() || eg_pit() || lfo_glo() || (page == PAGE_HOME && foc_op());
}

static int op_key(uint32_t k)
{
    uint32_t i;
    if (!op_point())
        return -1;
    for (i = 0; i < 6u; i++)
        if (OP_KEY[i] == k)
            return (int)(5u - i);
    return -1;
}

/* The two black keys at the top of the keyboard, which the panel prints MONO and POLY under. They
 * do the same on OPERATOR, where you are already deciding how the voice should behave. Mono is a
 * setting of the instrument and not of the voice -- the DX7 format has no room for it, and we would
 * rather a bank saved here stay a true DX7 dump than smuggle a byte into one. */
#define MONO_KEY 20u                            /* C#5 */
#define POLY_KEY 22u                            /* D#5 */

/* The top black key is the A/B compare wherever you are editing: a tap swaps sides on its release,
 * a hold copies the side you are on over the other one -- a place to come back to before you start
 * changing things. Plain HOME, with nothing focused, is for playing, so there it is an F#5. */
#define AB_KEY 25u                              /* F#5 */
#define MIDI_TX (ui[UI_MCH] ? ui[UI_MCH] - 1u : 0u)   /* the channel the keys send on */
#define ab_point() (page != PAGE_HOME || focus != FOC_OFF)
static uint32_t ab_t;                           /* when it went down, or 0 */
static uint8_t ab_long;                         /* ..and the hold has already copied */

/* Program change, both ways. Bank select (CC 0) 0, or none, and program N is the voice list's N+1,
 * so the factory voices and the first of yours answer to a bare program change as they always
 * have; bank 1..8 and program N is slot N+1 of U1..U8. Sending, the bank always goes first. */
static uint8_t pc_bank;                         /* the last bank select heard */
static void pc_send(uint32_t vi)
{
    if (vi < DX_NVOICES) {
        midi_send(0xB0u | MIDI_TX, 0, 0);
        midi_send(0xC0u | MIDI_TX, vi, 0);
    } else {
        midi_send(0xB0u | MIDI_TX, 0, 1u + (vi - DX_NVOICES) / BANK_V);
        midi_send(0xC0u | MIDI_TX, (vi - DX_NVOICES) % BANK_V, 0);
    }
}

/* PRESETS with SEL held: to the first voice of the next bank, or back to the first of this one (and
 * from there to the one before). The banks of the voice list are the factory voices, then U1..U8;
 * of the save slots, just U1..U8. Returns the new position, which may want wrapping. */
static int32_t bank_jump(uint32_t at, int32_t enc, int voices)
{
    int32_t g = voices ? (at < DX_NVOICES ? 0 : 1 + (int32_t)((at - DX_NVOICES) / BANK_V)) : (int32_t)(at / BANK_V);
    int32_t ng = voices ? (int32_t)NBANK + 1 : (int32_t)NBANK, first;
#define GFIRST(x) (voices ? ((x) ? (int32_t)DX_NVOICES + ((x) - 1) * (int32_t)BANK_V : 0) : (x) * (int32_t)BANK_V)
    first = GFIRST(g);
    if (enc > 0)
        g += enc;
    else if ((int32_t)at != first)
        g += enc + 1;
    else
        g += enc;
    g = ((g % ng) + ng) % ng;
    return GFIRST(g);
#undef GFIRST
}

/* The 156 bytes hold the operators in the DX7's own order, OP6 first, and dx7_core.c indexes
 * DX_ALG with the same number -- so the block index and the engine's operator index are the one
 * thing, and opsel is both. (Proven by muting each block in turn under algorithm 16, which has a
 * single carrier: block 5 is the one that silences it, and carriers() puts that carrier at index 5.) */
static uint32_t p_at(const param_t *p)          /* where this parameter lives in the voice */
{
    if (p->where == P_OP && p->idx == OPB_WAVE)  /* the waves are after the DX7's 156, one a byte */
        return DX_WAVE + opsel;
    return p->where == P_OP ? (uint32_t)opsel * 21u + p->idx : p->idx;
}

/* the byte a parameter is, wherever it lives */
static uint8_t *p_ref(const param_t *p)
{
    return p->where == P_UI ? &ui[p->idx] : &edit[p_at(p)];
}

static const char *const WAVE[] = {"TRIANGLE", "SAW DOWN", "SAW UP", "SQUARE", "SINE", "SAMP/HOLD"};
/* ..and short enough to share a third of the screen with the word WAVE, where the picture of it is
 * directly above to say the rest */
static const char *const WAVE_S[] = {"TRI", "SAWD", "SAWU", "SQR", "SINE", "S/H"};
static const char *const CURVE[] = {"-LIN", "-EXP", "+EXP", "+LIN"};
static const char *const OPWAVE[DX_NWAVE] = {"SINE", "HALF SINE", "ABS SINE", "QUARTER",
                                             "ALT SINE", "CAMEL", "SQUARE", "SAW"};
static const char *const FTYPE[] = {"OFF", "LOW PASS", "BAND PASS", "HIGH PASS"};
static const char *const APAT[] = {"UP", "DOWN", "UP/DOWN", "RANDOM", "AS PLAYED"};
static const char *const ARATE[] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const char NOTE_NAME[12][3] = {"C ", "C#", "D ", "D#", "E ", "F ", "F#", "G ", "G#", "A ", "A#", "B "};
/* ---- the screen */
#define C_ACC RGB(180, 140, 255)                /* lilac, as the logo */
#define C_OFF RGB(90, 90, 90)
#define C_LBL RGB(150, 150, 150)                /* parameter names: present, but not the thing you read */
#define KEY0 53u                                /* key 0 = F3 */

static uint32_t put(char *s, uint32_t i, const char *t) { while (*t) s[i++] = *t++; return i; }
static uint32_t putn(char *s, uint32_t i, uint32_t v, uint32_t w)   /* right-aligned in w, space-padded */
{
    char d[10];
    uint32_t n = 0;
    do { d[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (w-- > n) s[i++] = ' ';
    while (n) s[i++] = d[--n];
    return i;
}

/* ---- one number where the DX7 has two
 *
 * An operator's pitch is set by a coarse multiplier and a fine one that stretches it: the note you
 * played, times coarse, times (1 + fine/100). The pair is hard to read. COARSE 2 FINE 0 and
 * COARSE 1 FINE 99 are a semitone apart and look nothing alike, and the thing you actually want to
 * say -- "twice the key", "three times the key" -- is spread across two rows.
 *
 * RATIO says it in one: a list of the whole multiples that make a harmonic, blending sound, the
 * halves between the low ones, and 1.41 because an operator at the square root of two is the
 * quickest way to hear what inharmonic means. Underneath it is still exactly those two bytes, so
 * a patch edited here is still a DX7 patch and still exports as one. A ratio this list does not
 * reach is still reachable: COARSE and FINE are the next two rows on the page, and anything they
 * are set to is displayed faithfully here even when it is between two entries.
 *
 * In FIXED mode the same two bytes stop being a multiple and become a frequency, which the DX7
 * reads as 10^(((coarse & 3) * 100 + fine) / 100) Hz -- four decades from 1 Hz up, in 400 steps.
 * The row follows: it says the frequency and the knob walks those steps, because showing "x2.00"
 * for an operator pinned at 100 Hz would be a plain lie. */
static const uint8_t RAT[][2] = {               /* coarse, fine */
    {0, 0}, {0, 50}, {1, 0}, {1, 41}, {1, 50}, {2, 0}, {2, 25}, {3, 0}, {2, 75}, {4, 0},
    {5, 0}, {6, 0}, {7, 0}, {8, 0}, {9, 0}, {10, 0}, {11, 0}, {12, 0}, {13, 0}, {14, 0},
    {15, 0}, {16, 0}, {17, 0}, {18, 0}, {19, 0}, {20, 0}, {21, 0}, {22, 0}, {23, 0}, {24, 0},
    {25, 0}, {26, 0}, {27, 0}, {28, 0}, {29, 0}, {30, 0}, {31, 0},
};
#define RAT_N (sizeof RAT / sizeof RAT[0])

static uint32_t rat_x100(uint32_t c, uint32_t f)        /* the multiple, times a hundred */
{
    return (c ? c * 100u : 50u) * (100u + (f > 99u ? 99u : f)) / 100u;
}

static uint8_t *op_byte(uint32_t op, uint32_t off) { return &edit[op * 21u + off]; }
static uint32_t rat_now(uint32_t op) { return rat_x100(*op_byte(op, 18), *op_byte(op, 19)); }

/* Where the operator sits in the list. Exact if its two bytes are an entry; otherwise the entry
 * below it, so that a turn up lands on the next one above rather than skipping it. */
static uint32_t rat_index(uint32_t op, uint32_t *exact)
{
    uint32_t v = rat_now(op), i, at = 0;
    *exact = 0;
    for (i = 0; i < RAT_N; i++) {
        uint32_t e = rat_x100(RAT[i][0], RAT[i][1]);
        if (e == v && RAT[i][0] == *op_byte(op, 18)) { *exact = 1; return i; }
        if (e <= v) at = i;
    }
    return at;
}

/* 10^(f/100) in Q12, as two tables of ten so it costs forty bytes instead of two hundred */
static const uint16_t TEN1[10] = {4096, 5157, 6492, 8173, 10289, 12953, 16306, 20529, 25844, 32536};
static const uint16_t TEN2[10] = {4096, 4191, 4289, 4389, 4491, 4596, 4703, 4812, 4924, 5039};
static uint32_t fix_step(uint32_t op) { return (*op_byte(op, 18) & 3u) * 100u + (*op_byte(op, 19) > 99u ? 99u : *op_byte(op, 19)); }
static uint32_t fix_hz_x100(uint32_t x)         /* the frequency those steps mean, times a hundred */
{
    uint32_t d = x / 100u, f = x % 100u, mul = 25u;
    uint32_t q12 = ((uint32_t)TEN1[f / 10u] * TEN2[f % 10u]) >> 12;     /* 10^(f/100), Q12 */
    while (d--) mul *= 10u;                     /* the decade goes in before the shift, or the top
                                                 * one would round away its last digit */
    return (q12 * mul) >> 10;                   /* 25 and 10 rather than 100 and 12: same value, a
                                                 * quarter of the room needed to hold it */
}

/* SEL held: not the list but every ratio the two bytes can make, in order. COARSE 1..31 times
 * 1.00..1.99, or a half times the same, overlap all the way up, so the next one above or below is
 * the nearest of 32 candidates -- the least FINE that gets past the current value, coarse by coarse.
 * Around x1 that is a hundredth a click, which is what a little detune-by-ratio wants. Ties go to
 * the smaller FINE, so x3.00 is coarse 3 rather than coarse 2 and a half again. */
static void rat_free(uint32_t op, int32_t dir)
{
    uint32_t v = rat_now(op), c, best = dir > 0 ? 0xFFFFFFFFu : 0u, bc = 0, bf = 0, found = 0;
    for (c = 0; c < 32u; c++) {
        int32_t f;
        uint32_t x;
        if (dir > 0)                            /* the least FINE above v */
            f = c ? (int32_t)(v / c) + 1 - 100 : (int32_t)(2u * v) + 2 - 100;
        else                                    /* the most FINE below it */
            f = c ? (int32_t)((v - 1u) / c) - 100 : (int32_t)(2u * v) - 1 - 100;
        if (dir > 0 && f < 0) f = 0;
        if (dir < 0 && f > 99) f = 99;
        if (f < 0 || f > 99)
            continue;
        x = rat_x100(c, (uint32_t)f);
        if (dir > 0 ? x <= v : x >= v)
            continue;
        if (!found || (dir > 0 ? x < best : x > best) || (x == best && (uint32_t)f < bf)) {
            best = x;
            bc = c;
            bf = (uint32_t)f;
            found = 1;
        }
    }
    if (!found)                                 /* the end of the range: stay */
        return;
    *op_byte(op, 18) = (uint8_t)bc;
    *op_byte(op, 19) = (uint8_t)bf;
}

/* Turn an operator's pitch by one or more clicks, whichever of the two things it currently means.
 * The HOME knobs drive this for an operator you are not editing, so it takes one. */
static void rat_turn(uint32_t op, int32_t enc)
{
    mi_panel_target(19u+5u-op); mi_panel_target(25u+5u-op);
    if (*op_byte(op, 17)) {                     /* FIXED: walk the four decades in steps */
        int32_t x = (int32_t)fix_step(op) + enc;
        x = x < 0 ? 0 : x > 399 ? 399 : x;
        *op_byte(op, 18) = (uint8_t)(x / 100);
        *op_byte(op, 19) = (uint8_t)(x % 100);
    } else if (sel_fine()) {                    /* RATIO, SEL held: every ratio there is */
        for (; enc > 0; enc--) rat_free(op, 1);
        for (; enc < 0; enc++) rat_free(op, -1);
    } else {                                    /* RATIO: walk the list */
        uint32_t exact;
        int32_t x = (int32_t)rat_index(op, &exact);
        /* rat_index leaves us on the entry below an off-list ratio, so turning down from there is
         * already a step and must not take another */
        x += exact || enc > 0 ? enc : enc + 1;
        x = x < 0 ? 0 : x > (int32_t)RAT_N - 1 ? (int32_t)RAT_N - 1 : x;
        *op_byte(op, 18) = RAT[x][0];
        *op_byte(op, 19) = RAT[x][1];
    }
}

/* Move one parameter by one knob's worth, wherever the knob happened to be. The focus knobs and
 * the knobs on a page are the same four knobs on the same bytes, so they must also be the same
 * arithmetic -- including the two rows that write a second byte with the first. */
static void fm_midi_touch(const param_t *p)
{
    uint32_t target = MI_NONE;
    if (p->where == P_UI) {
        if (p->idx >= UI_FX && p->idx < UI_FX+16u) target = fm_midi_fx_targets[p->idx-UI_FX];
        else if (p->idx >= UI_GATK && p->idx <= UI_GREL) target = 35u+p->idx-UI_GATK;
        else if (p->idx == UI_FCUT) target = MI_CUTOFF;
        else if (p->idx == UI_FRES) target = MI_RESONANCE;
    } else if (p->where == P_OP) {
        if (p->idx == 16u) target = 13u+5u-opsel;
        else if (p->idx == 18u) target = 19u+5u-opsel;
        else if (p->idx == 19u) target = 25u+5u-opsel;
    } else {
        if (p->idx == 135u) target = 31;
        else if (p->idx == 137u) target = 32;
        else if (p->idx == 139u) target = 33;
        else if (p->idx == 140u) target = 34;
    }
    mi_panel_target(target);
}

static void p_turn(const param_t *p, int32_t enc)
{
    fm_midi_touch(p);
    int32_t v;
    if (p->kind == K_RAT) {
        rat_turn(opsel, enc);
        return;
    }
    v = (int32_t)*p_ref(p) + (p->kind == K_TIME || p->kind == K_DEC ? -enc : enc);
    if (p->kind == K_OPWAVE || p->kind == K_FTYPE)  /* a ring, not a range: past SAW is SINE again */
        v = ((v % ((int32_t)p->max + 1)) + (int32_t)p->max + 1) % ((int32_t)p->max + 1);
    v = v < 0 ? 0 : v > (int32_t)p->max ? (int32_t)p->max : v;
    *p_ref(p) = (uint8_t)v;
    if (p->kind == K_DEC)                       /* the second stage of the decay goes with the first */
        edit[p_at(p) + 1u] = (uint8_t)v;
    if (p->kind == K_SUS)                       /* and the level it was heading for, so the two are flat */
        edit[p_at(p) - 1u] = (uint8_t)v;
}

/* an operator's pitch written out, as a multiple of the key or as the frequency it is pinned to */
static uint32_t put_ratio(char *s, uint32_t i, uint32_t op)
{
    uint32_t n;
    if (!*op_byte(op, 17)) {
        n = rat_now(op);
        s[i++] = 'x';
        i = putn(s, i, n / 100u, 1);
        s[i++] = '.';
        s[i++] = (char)('0' + (n / 10u) % 10u);
        return (s[i++] = (char)('0' + n % 10u)), i;
    }
    n = fix_hz_x100(fix_step(op));              /* fewer decimals the higher it goes, to stay in the box */
    i = putn(s, i, n / 100u, 1);
    if (n < 100000u) {
        s[i++] = '.';
        s[i++] = (char)('0' + (n / 10u) % 10u);
        if (n < 1000u) s[i++] = (char)('0' + n % 10u);
    }
    return put(s, i, "HZ");
}

/* a parameter's value as the DX7 means it, not as the byte happens to be stored */
static uint32_t putval(char *s, uint32_t i, const param_t *p, uint32_t v)
{
    switch (p->kind) {
    case K_RAT:  return put_ratio(s, i, opsel);
    case K_SPAN: i = putn(s, i, SP_SPAN[v < SP_NSPAN ? v : 0] / 1000u, 1); return put(s, i, "KHZ");
    case K_DET:  s[i++] = v < 7u ? '-' : '+'; return putn(s, i, v < 7u ? 7u - v : v - 7u, 1);
    case K_MODE: return put(s, i, v ? "FIXED" : "RATIO");
    case K_ONOFF: return put(s, i, v ? "ON" : "OFF");
    case K_STEREO: return put(s, i, v ? "MONO" : "STEREO");
    case K_CURVE: return put(s, i, CURVE[v & 3u]);
    case K_WAVE: return put(s, i, WAVE[v < 6u ? v : 0]);
    case K_FTYPE: return put(s, i, FTYPE[v & 3u]);
    case K_OPWAVE: return put(s, i, OPWAVE[v & 7u]);
    case K_HZ: {                                /* three figures, which is all a hundred steps need */
        uint32_t hz = FLT_HZ[v < 100u ? v : 99u];
        if (hz < 1000u) { i = putn(s, i, hz, 1); return put(s, i, "HZ"); }
        i = putn(s, i, hz / 1000u, 1);
        s[i++] = '.';
        s[i++] = (char)('0' + (hz / 100u) % 10u);
        if (hz < 10000u)
            s[i++] = (char)('0' + (hz / 10u) % 10u);
        return put(s, i, "KHZ");
    }
    case K_FXHZ: {                              /* 0.05HZ, 1.2HZ, 10HZ */
        uint32_t c = FX_RATE[v < 100u ? v : 99u];
        i = putn(s, i, c / 100u, 1);
        if (c < 1000u) {
            s[i++] = '.';
            s[i++] = (char)('0' + (c / 10u) % 10u);
            if (c < 100u) s[i++] = (char)('0' + c % 10u);
        }
        return put(s, i, "HZ");
    }
    case K_DLYT: {
        uint32_t ms = DLY_T[v < 100u ? v : 99u] * 10u / 441u;
        if (ms < 1000u) { i = putn(s, i, ms, 1); return put(s, i, "MS"); }
        i = putn(s, i, ms / 1000u, 1);
        s[i++] = '.';
        s[i++] = (char)('0' + (ms / 100u) % 10u);
        s[i++] = (char)('0' + (ms / 10u) % 10u);
        return put(s, i, "S");
    }
    case K_RT: {
        uint32_t t = RV_RT[v < 100u ? v : 99u];
        i = putn(s, i, t / 10u, 1);
        if (t < 100u) { s[i++] = '.'; s[i++] = (char)('0' + t % 10u); }
        return put(s, i, "S");
    }
    case K_PRE: i = putn(s, i, v, 1); return put(s, i, "MS");
    case K_BPM: i = putn(s, i, 40u + 2u * v, 1); return put(s, i, " BPM");
    case K_APAT: return put(s, i, APAT[v < 5u ? v : 0u]);
    case K_AOCT: i = putn(s, i, v + 1u, 1); return put(s, i, v ? " OCTS" : " OCT");
    case K_BARS: return put(s, i, v == 0u ? "1 BAR" : v == 1u ? "2 BARS" : "4 BARS");
    case K_DIV: return put(s, i, v == 0u ? "1/16" : v == 1u ? "1/8" : "1/4");
    case K_PCT: i = putn(s, i, v + 1u, 1); return put(s, i, "%");
    case K_ARATE: return put(s, i, ARATE[v < 6u ? v : 3u]);
    case K_SWING: i = putn(s, i, 50u + v, 1); return put(s, i, "%");
    case K_VEL: return putn(s, i, v ? (v * 127u + 49u) / 99u : 1u, 1);
    case K_MCH: return v ? putn(s, i, v, 1) : put(s, i, "ALL");
    case K_BITS: i = putn(s, i, 24u - CRUSH_SH[v < 4u ? v : 3u], 1); return put(s, i, " BIT");
    case K_SRATE: {
        static const char *const SR_NAME[SR_N] = {"8K", "11K", "16K", "22K", "32K", "44.1K", "88.2K"};
        return put(s, i, SR_NAME[v < SR_N ? v : SR_OS]);
    }
    case K_PLVL: {                              /* 32 of the engine's units are an octave: 3/8 a semitone */
        int32_t t = DX_PITCH[v < 100u ? v : 99u] * 15;   /* tenths of a semitone, times four */
        uint32_t a = (uint32_t)((t < 0 ? -t : t) + 2) / 4u;
        if (a)
            s[i++] = t < 0 ? '-' : '+';
        i = putn(s, i, a / 10u, 1);
        s[i++] = '.';
        s[i++] = (char)('0' + a % 10u);
        return i;
    }
    case K_ALG:  return putn(s, i, v + 1u, 1);
    case K_BRK:                                 /* 0..99 is A-1 upwards */
        i = put(s, i, NOTE_NAME[(v + 21u) % 12u]);
        return putn(s, i, (v + 21u) / 12u, 1);
    case K_TRSP: s[i++] = v < 24u ? '-' : '+'; return putn(s, i, v < 24u ? 24u - v : v - 24u, 1);
    case K_TIME:
    case K_DEC:  return putn(s, i, 99u - (v < 99u ? v : 99u), 1);   /* a rate, said as a time */
    case K_MSR:
        if (v == 0u) return put(s, i, "OFF");    /* the short way round to the same place */
        /* fall through */
    case K_MSO:
        if (v >= 99u) return put(s, i, "OFF");   /* a stage that never moves */
        /* fall through */
    case K_MS: {                                /* this one knows what its time actually is */
        uint32_t ms = GMS[v < 100u ? v : 99u];
        if (ms < 1000u) { i = putn(s, i, ms, 1); return put(s, i, "MS"); }
        i = putn(s, i, ms / 1000u, 1);
        s[i++] = '.';
        s[i++] = (char)('0' + (ms / 100u) % 10u);
        return put(s, i, "S");
    }
    default: return putn(s, i, v, 1);
    }
}

/* what the voice line says when a HOME knob moves, since with no note held the boxes show nothing */
static char hk_say[20];
static void say_knob(uint32_t n)
{
    uint32_t op = 6u - n, i = put(hk_say, 0, "OP");
    i = putn(hk_say, i, n, 1);
    if (HSET[homeset].what == HK_LVL) {
        i = put(hk_say, i, " LVL ");
        i = putn(hk_say, i, *op_byte(op, 16), 1);
    } else {
        hk_say[i++] = ' ';
        i = put_ratio(hk_say, i, op);
    }
    hk_say[i] = 0;
    say(hk_say);
}

static void draw_voice(uint32_t vi, uint32_t y)
{
    char s[32], nm[11];
    uint32_t i = 0;
    if (flash_msg) {                            /* something arrived, or was written: say so here */
        draw_text_box(0, y, 240, &FONT_S, flash_msg, C_ACC, 1);
        return;
    }
    if (saving) {                               /* the slot to write to, and the name to write */
        int32_t x, cw;
        uint32_t k;
        i = put(s, i, "SAVE ");
        i = put_slot(s, i, save_slot);          /* zero-padded: the line must not shift sideways */
        s[i++] = ' ';
        s[i] = 0;
        cw = FONT_S.adv[glyph(&FONT_S, ' ')];   /* the font is fixed width: one cell per character */
        cv_begin(240, FONT_S.h, C_BLACK);
        x = ((int32_t)240 - (text_w(&FONT_S, s) + 10 * cw)) / 2;
        /* the slot number is accented when that slot already holds a sound of yours, so the one
         * thing you cannot undo -- writing over something -- is visible without costing a word */
        x = cv_text(x, 0, &FONT_S, s, slot_free(save_slot) ? C_LBL : C_ACC);
        for (k = 0; k < 10u; k++) {
            char c1[2];
            c1[0] = save_name[k]; c1[1] = 0;
            if (k == save_cur) {                /* the letter the knob is on: accented, and ruled
                                                 * underneath so that it shows on a blank too */
                cv_rect(x, (int32_t)FONT_S.h - 2, cw, 2, C_ACC);
                cv_text(x, 0, &FONT_S, c1, C_ACC);
            } else
                cv_text(x, 0, &FONT_S, c1, C_WHITE);
            x += cw;
        }
        cv_blit(0, y);
        return;
    }
    s[i++] = (char)('A' + ab);                  /* which side of the compare you are listening to */
    s[i++] = ' ';
    if (vi < DX_NVOICES) {
        s[i++] = ' ';
        i = putn(s, i, vi + 1u, 2);
    } else
        i = put_slot(s, i, vi - DX_NVOICES);    /* one of yours */
    s[i++] = ' ';
    voice_name(vi, nm);
    i = put(s, i, nm);
    if (modified) { s[i++] = ' '; s[i++] = '*'; }
    s[i] = 0;
    draw_text_box(0, y, 240, &FONT_S, s, modified ? C_ACC : C_WHITE, 1);
}

/* ---- the algorithm, drawn from the engine's own routing table
 *
 * DX_ALG[alg][op] is what dx_compute_ext actually obeys, so a diagram derived from it cannot
 * disagree with what you hear. Per operator: bits 0-1 the bus it writes (0 = the output, so a
 * carrier), bit 2 add rather than replace, bits 4-5 the bus it reads, bit 6 feedback in, bit 7
 * feedback out. Operators run in index order, which is OP6 first.
 *
 * So the modulators of an operator are whichever operators wrote its input bus since that bus was
 * last overwritten. A bus is not cleared when it is read -- dx_compute_ext leaves it in place --
 * which is how one operator comes to modulate several carriers, as in algorithms 19 to 25. */
static uint32_t carriers(uint32_t alg)          /* bit op: operator at index op feeds the output */
{
    uint32_t op, m = 0;
    for (op = 0; op < 6u; op++)
        if ((DX_ALG[alg & 31u][op] & 3u) == 0u)
            m |= 1u << op;
    return m;
}

#define ALG_UNSET 255u
static uint8_t a_mods[6];                       /* bit m: operator m modulates this one */
static uint8_t a_row[6], a_col[6];              /* 0 = the bottom row, where the carriers are */
static uint8_t a_ncol, a_nrow, a_fb;            /* a_fb: bit op, operator op is in the feedback loop */

static void alg_mods(uint32_t alg)
{
    uint8_t pend[3] = {0, 0, 0};                /* who has written bus 1 and 2 and not been read yet */
    uint32_t op;
    a_fb = 0;
    for (op = 0; op < 6u; op++) {
        uint32_t fl = DX_ALG[alg & 31u][op], inb = (fl >> 4) & 3u, outb = fl & 3u;
        a_mods[op] = inb ? pend[inb] : 0u;
        if (outb) pend[outb] = (uint8_t)((fl & 4u) ? pend[outb] | (1u << op) : (1u << op));
        if (fl & 0xC0u) a_fb |= (uint8_t)(1u << op);
    }
}

/* Place the tree the way the DX7 manual draws it: carriers along the bottom, a modulator one row
 * above the highest thing it feeds, and a shared modulator centred over everything it feeds.
 * Walked with an explicit stack -- the audio ISR borrows this stack, and RULES.md says not to
 * spend it. Checked against the printed algorithm chart for all 32. */
static void alg_layout(uint32_t alg)
{
    uint8_t stack[8], took[8];
    uint32_t sp, op, k, col = 0, maxcol = 0, maxrow = 0;
    alg_mods(alg);
    for (op = 0; op < 6u; op++) { a_row[op] = (uint8_t)ALG_UNSET; a_col[op] = (uint8_t)ALG_UNSET; }

    /* Rows. a_mods[op] only ever names operators of lower index, because the bus has to be written
     * before it is read and the engine runs the operators in index order. So one descending pass
     * settles every row: by the time it reaches an operator, everything it feeds is already done. */
    for (op = 0; op < 6u; op++)
        if ((DX_ALG[alg & 31u][op] & 3u) == 0u) a_row[op] = 0;
    for (op = 6u; op-- > 0u; ) {
        if (a_row[op] == (uint8_t)ALG_UNSET) continue;
        for (k = 0; k < op; k++)
            if ((a_mods[op] >> k & 1u) &&
                (a_row[k] == (uint8_t)ALG_UNSET || a_row[k] < a_row[op] + 1u))
                a_row[k] = (uint8_t)(a_row[op] + 1u);
    }

    /* Columns, depth first from each carrier in DX7 order. An operator whose modulators this walk
     * had to place sits centred over them; one that brought nothing new takes the next free
     * column. That is what keeps the carriers of algorithm 24 side by side instead of stacked. */
    for (op = 6u; op-- > 0u; ) {
        if (a_row[op] != 0u || a_col[op] != (uint8_t)ALG_UNSET) continue;
        sp = 0; stack[0] = (uint8_t)op; took[0] = 0;
        for (;;) {
            uint32_t cur = stack[sp], kid = 6u;
            for (k = 6u; k-- > 0u; )            /* modulators in display order, OP1 first */
                if ((a_mods[cur] >> k & 1u) && a_col[k] == (uint8_t)ALG_UNSET) { kid = k; break; }
            if (kid < 6u && sp < 7u) {
                took[sp] = 1; stack[++sp] = (uint8_t)kid; took[sp] = 0; continue;
            }
            if (took[sp]) {
                uint32_t lo = 255u, hi = 0;
                for (k = 0; k < 6u; k++)
                    if ((a_mods[cur] >> k & 1u) && a_col[k] != (uint8_t)ALG_UNSET) {
                        if (a_col[k] < lo) lo = a_col[k];
                        if (a_col[k] > hi) hi = a_col[k];
                    }
                a_col[cur] = (uint8_t)((lo + hi) / 2u);
            } else {
                a_col[cur] = (uint8_t)col++;
            }
            if (!sp) break;
            sp--;
        }
    }
    for (op = 0; op < 6u; op++)                 /* an operator nothing reaches still has to go somewhere */
        if (a_col[op] == (uint8_t)ALG_UNSET) { a_row[op] = 0; a_col[op] = (uint8_t)col++; }

    /* A modulator feeding more than one operator gets centred over all of them. Done bottom up, so
     * the operators it feeds have their final columns by the time it is moved. */
    for (k = 1u; k < 6u; k++)
        for (op = 0; op < 6u; op++) {
            uint32_t n = 0, lo = 255u, hi = 0, c;
            if (a_row[op] != k) continue;
            for (c = 0; c < 6u; c++)
                if (a_mods[c] >> op & 1u) {
                    n++;
                    if (a_col[c] < lo) lo = a_col[c];
                    if (a_col[c] > hi) hi = a_col[c];
                }
            if (n > 1u) a_col[op] = (uint8_t)((lo + hi) / 2u);
        }

    for (op = 0; op < 6u; op++) {
        if (a_col[op] > maxcol) maxcol = a_col[op];
        if (a_row[op] > maxrow) maxrow = a_row[op];
    }
    a_ncol = (uint8_t)(maxcol + 1u);
    a_nrow = (uint8_t)(maxrow + 1u);
}

/* ---- the shape of an envelope, worked out by stepping it the way the engine does
 *
 * A DX7 envelope is four (rate, level) pairs. The level is where the segment is heading and the
 * rate is how fast it gets there, so the same rate gives a different *time* depending on how far
 * it has to travel -- which is the single most confusing thing about programming a DX7, and the
 * reason it is drawn to scale rather than as four equal slopes.
 *
 * The maths below is dx_env_advance and dx_env_sample, solved for the segment length instead of
 * run one block at a time: falling is linear, and rising is linear within each octave of level,
 * so a handful of exact steps covers it.
 *
 * It is worked out once and drawn at two sizes: the width of the screen on the ENVELOPE page, and
 * thumbnail-sized inside every operator's box on the algorithm diagram. */
#define EG_FULL (16 << 24)                      /* the level the engine saturates at */

static int32_t eg_target(const dx_env_t *e, uint32_t i)
{
    int32_t a = (int32_t)(dx_scaleoutlevel(e->levels[i]) >> 1);
    a = (a << 6) + e->outlevel - 4256;
    return (a < 16 ? 16 : a) << 16;
}

static int32_t eg_inc(const dx_env_t *e, uint32_t i)
{
    int32_t q = ((int32_t)e->rates[i] * 41) >> 6;
    q += e->rate_scaling;
    if (q > 63) q = 63;
    return (4 + (q & 3)) << (2 + DX_LG_N + (q >> 2));
}

/* how many blocks dx_env_sample would need to walk from `from` to `to` */
static uint32_t eg_blocks(int32_t from, int32_t to, int32_t inc)
{
    uint32_t n = 0;
    if (inc <= 0) return 1u;
    if (to <= from) {                           /* falling is linear, and the last step overshoots */
        uint32_t d = (uint32_t)((from - to + inc - 1) / inc);
        return d ? d : 1u;
    }
    if (from < (1716 << 16)) from = 1716 << 16; /* the jump a rising segment starts with */
    while (from < to && n < 1000000u) {
        int32_t band = (((from >> 24) + 1) << 24);
        int32_t step = (((17 << 24) - from) >> 24) * inc;   /* the shift floors: this is the factor dx_env_sample uses */
        int32_t lim = to < band ? to : band;
        uint32_t k;
        if (step <= 0) break;
        k = (uint32_t)((lim - from + step - 1) / step);
        if (!k) k = 1u;
        n += k;
        from += (int32_t)k * step;
    }
    return n ? n : 1u;            /* the block that jumps to 1716 also takes a step */
}

/* The six corners of the drawn envelope: the four segments, with a flat stretch between the third
 * and the fourth for as long as the key is held. x gets the horizontal position of each corner and
 * lvl the level there, so corners 3 and 4 share a level and are the ends of the flat. `hold` is how
 * wide that stretch is drawn and `minw` the narrowest a segment may be, since a segment too fast to
 * see still has to be wide enough to point at. */
static void eg_shape(const dx_env_t *e, int32_t x0, int32_t x1, int32_t hold, int32_t minw,
                     int32_t *x, int32_t *lvl)
{
    int32_t t[4], span, acc;
    uint32_t b[4], i, tot = 0;

    lvl[0] = 0;
    for (i = 0; i < 4u; i++) {
        t[i] = eg_target(e, i);
        b[i] = eg_blocks(lvl[i], t[i], eg_inc(e, i));
        if (b[i] > 60000u) b[i] = 60000u;       /* a 90 s segment must not squash the other three */
        lvl[i + 1] = t[i];
        tot += b[i];
    }
    if (!tot) tot = 1u;

    span = x1 - x0 - hold;
    acc = 0;
    x[0] = x0;
    for (i = 0; i < 4u; i++) {                  /* widths in proportion to real time */
        acc += (int32_t)b[i];
        x[i + 1] = x0 + (int32_t)(((int64_t)acc * span) / (int32_t)tot);
        if (x[i + 1] < x[i] + minw) x[i + 1] = x[i] + minw;
        if (i == 2) x[3] += hold;               /* the key is held here, for as long as you hold it */
    }
    if (x[4] > x1) x[4] = x1;
}

/* where the diagram lives, and how big a box is */
#define ALG_Y 64
#define ALG_H 136
/* cv_begin clamps a canvas that will not fit instead of failing, which shows up as a stale strip at
 * the bottom of the screen rather than as an error. Catch the mismatch at compile time instead. */
typedef char fmlab_canvas_fits[(240u * ALG_H <= CV_MAX) ? 1 : -1];
static int32_t a_bw, a_bh, a_x0, a_dx, a_dy, a_ybase;

static void alg_metrics(void)
{
    int32_t used;
    a_dx = 236 / (int32_t)a_ncol;
    a_bw = a_dx - 6;
    if (a_bw > 40) a_bw = 40;
    if (a_bw < 14) a_bw = 14;
    a_dy = (ALG_H - 10) / (int32_t)a_nrow;
    if (a_dy > 44) a_dy = 44;                   /* two rows should not sprawl over the whole screen */
    a_bh = a_dy - 6;
    if (a_bh > 26) a_bh = 26;
    if (a_bh < 10) a_bh = 10;
    a_x0 = (240 - a_dx * (int32_t)a_ncol) / 2;
    used = a_dy * (int32_t)a_nrow;              /* centre the stack, output bus and all */
    a_ybase = ALG_H - 6 - (ALG_H - 10 - used) / 2;
}
static int32_t a_bx(uint32_t op) { return a_x0 + a_dx * (int32_t)a_col[op] + (a_dx - a_bw) / 2; }
static int32_t a_by(uint32_t op) { return a_ybase - a_dy * ((int32_t)a_row[op] + 1) + (a_dy - a_bh) / 2; }

/* How loud an operator is right now, 0..255. level_in is what the engine feeds to exp2 to get the
 * operator's gain, so it is already logarithmic -- which is the useful thing to see, and is how a
 * DX7 envelope is specified in the first place. A 32-bit aligned read can race the ISR; the worst
 * that does is draw one frame from a value a block old. */
static uint32_t op_now(uint32_t op)
{
    uint32_t v = v_last;
    int32_t l;
    if (v >= NV || !dxv[v].on || !(v_lit >> v & 1u))
        return 0;
    l = dxv[v].level_in[op];
    if (l <= 0) return 0;
    l >>= 16;                                   /* 0 .. 16 << 8 */
    return l >= 4080 ? 255u : (uint32_t)l * 255u / 4080u;
}

/* The envelope the ISR is actually running for this operator, or 0 if nothing is sounding. */
static const dx_env_t *op_live(uint32_t op)
{
    uint32_t v = v_last;
    if (v >= NV || !dxv[v].on || !(v_lit >> v & 1u) || (opmute >> op & 1u))
        return 0;
    return &dxv[v].env[op];
}

/* A level as a row inside a box `h` tall: zero sits on the floor, full scale just under the lid.
 * The level is already logarithmic -- one unit is 6 dB -- which is the useful thing to see, and is
 * how a DX7 envelope is specified in the first place. */
static int32_t alg_ey(int32_t level, int32_t h)
{
    if (level < 0) level = 0;
    if (level > EG_FULL) level = EG_FULL;
    return (h - 2) - (int32_t)(((int64_t)level * (h - 3)) / EG_FULL);
}

/* How wide the held stretch is drawn in a box. Small, because a box has no room to spare and the
 * flat is the one part of the picture that means "for as long as you like" rather than a duration. */
static int32_t alg_hold(void) { int32_t h = (a_bw - 2) / 8; return h < 2 ? 2 : h; }

/* The curve for the one operator that gets one, worked out only when it changes. It depends on
 * nothing but that operator's bytes, which one it is and how wide a box is, so a checksum over all
 * three is enough to know -- cheaper than finding every place that writes `edit`, and unlike a
 * dirty flag it cannot go stale. */
static int32_t alg_ex[5], alg_el[5];
static uint32_t alg_esig;

static void alg_eg_build(void)
{
    const uint8_t *o = &edit[opsel * 21u];
    uint32_t i, sig = (uint32_t)a_bw * 2654435761u + opsel;
    dx_env_t e;
    int rs = 0, ol;
    for (i = 0; i < 21u; i++)
        sig = sig * 31u + o[i];
    if (sig == alg_esig)
        return;
    alg_esig = sig;
    ol = dx_op_level(o, 60, 100, &rs);          /* middle C, as the ENVELOPE page reckons it */
    dx_env_init(&e, o, o + 4, ol, rs);
    eg_shape(&e, 0, a_bw - 3, alg_hold(), 1, alg_ex, alg_el);
}

/* One cycle of a wave, w columns wide, centred on row cy and reaching a rows either side of it,
 * drawn as a line: each column runs from where the last one ended to where this one is, so a SQUARE
 * gets its two upright edges and a SAW its drop. axis 0: no centre line. Read from dx_wave itself,
 * so it is the shape the engine plays and not a drawing of one. */
static void alg_wave(int32_t x, int32_t cy, int32_t w, int32_t a, uint32_t wave, uint16_t line, uint16_t axis)
{
    int32_t c, py = cy;
    if (axis)
        cv_rect(x, cy, w, 1, axis);
    for (c = 0; c < w; c++) {
        int32_t v = dx_wave((int32_t)(((uint32_t)c << 24) / (uint32_t)w), (int)wave);
        int32_t yy = cy - (int32_t)(((int64_t)v * a + (1 << 23)) >> 24), lo = yy, hi = yy;
        if (c) {
            if (py < lo) lo = py;
            if (py > hi) hi = py;
        }
        cv_rect(x + c, lo, 1, hi - lo + 1, line);
        py = yy;
    }
}

/* The operator's colour at brightness t (0..255). Warm if it is in the feedback loop, so the one
 * operator that is modulating itself stands out from the ones that are not. */
static uint16_t op_hue(uint32_t op, uint32_t t)
{
    if (!(a_fb >> op & 1u))
        return RGB((t * 150u) / 255u, (t * 120u) / 255u, t);
    return RGB(t, (t * 150u) / 255u, (t * 90u) / 255u);
}

/* Every operator is the same shape: an outline that is always there, filling from the left with
 * how loud that operator is right now. Carriers used to be drawn solid and modulators hollow, but
 * the diagram already says which is which -- a carrier is the one with a wire down to the output
 * bus -- so the fill is free to mean something you cannot otherwise see. Watch a four-operator
 * stack and the modulators empty out before the carrier does, which is why the timbre moves.
 *
 * Except for one box. Tap ENV and the operator those knobs are on swaps its bar for its
 * envelope -- time left to right, level upwards, the shape filled in solid so that a box this
 * small still reads. (Tap EDIT and it draws one cycle of its wave instead, scaled the same way --
 * see alg_wave.) The whole shape is scaled by that operator's output LEVEL, so turning LEVEL
 * raises or collapses the entire curve with no note held at all, which a bar could never show;
 * and while a note is held the part of it already walked is drawn bright, so the boundary between
 * the two is the point the envelope has reached. It is the one operator you are looking at, so it
 * is the one that can afford the detail -- the other five stay bars, which is what makes it stand
 * out -- and it gives its number up to make room, which the white ring round it has already said.
 *
 * a_bh is never less than fifteen -- six operators is the deepest a column can go, and the metrics
 * give even that fifteen rows -- so there is always room for a curve. */
static void alg_box(int32_t x, int32_t y, uint32_t op, int sel)
{
    uint32_t off = (opmute >> op) & 1u, wave = edit[DX_WAVE + op] & 7u;
    int wshape = focus == FOC_EDIT && page == PAGE_HOME && op == opsel;
    int shape = focus == FOC_ENV && !eg_glo() && op == opsel;
    uint16_t frame = off ? RGB(70, 70, 70) : op_hue(op, 105u);
    int32_t w = a_bw - 2, c;
    char s[2];

    cv_rect(x, y, a_bw, 1, frame);
    cv_rect(x, y + a_bh - 1, a_bw, 1, frame);
    cv_rect(x, y, 1, a_bh, frame);
    cv_rect(x + a_bw - 1, y, 1, a_bh, frame);

    if (shape) {
        const dx_env_t *live = off ? 0 : op_live(op);
        uint16_t solid = off ? RGB(56, 56, 56) : op_hue(op, 140u);
        uint16_t walked = off ? RGB(84, 84, 84) : op_hue(op, 255u);
        int32_t cx[6], cyv[6], cy[40], k, px = -1;

        /* the polyline: the four segments, with the held stretch spliced in between the third
         * corner and the fourth, and the release landing in the bottom right-hand corner */
        cx[0] = alg_ex[0]; cx[1] = alg_ex[1]; cx[2] = alg_ex[2];
        cx[3] = alg_ex[3] - alg_hold(); cx[4] = alg_ex[3]; cx[5] = w - 1;
        if (cx[3] < cx[2]) cx[3] = cx[2];
        for (k = 0; k < 5; k++)
            cyv[k] = alg_ey(alg_el[k < 4 ? k : 3], a_bh);
        cyv[5] = a_bh - 2;
        for (c = 0, k = 0; c < w; c++) {        /* the curve as one height per column */
            int32_t dx;
            while (k < 4 && c >= cx[k + 1]) k++;
            dx = cx[k + 1] - cx[k];
            cy[c] = dx <= 0 ? cyv[k + 1] : cyv[k] + (cyv[k + 1] - cyv[k]) * (c - cx[k]) / dx;
        }
        if (live) {                             /* how far along it the envelope has got */
            uint32_t ix = live->ix < 4u ? live->ix : 3u;
            int32_t a = ix == 3u ? cx[4] : cx[ix], b = ix == 3u ? cx[5] : cx[ix + 1];
            int32_t lo = alg_el[ix], hi = alg_el[ix + 1];
            px = hi == lo ? b : a + (int32_t)(((int64_t)(live->level - lo) * (b - a)) / (hi - lo));
            if (px < 0) px = 0;
            if (px > w) px = w;
        }
        for (c = 0; c < w; c++)
            cv_rect(x + 1 + c, y + cy[c], 1, a_bh - 1 - cy[c], c < px ? walked : solid);
    } else if (wshape) {
        int32_t a = ((a_bh - 5) / 2) * (int32_t)edit[op * 21u + 16u] / 99;
        alg_wave(x + 1, y + a_bh / 2, w, a < 1 ? 1 : a, wave,
                 off ? RGB(110, 110, 110) : op_hue(op, 255u), off ? RGB(48, 48, 48) : op_hue(op, 70u));
    } else {
        int32_t fw = w * (int32_t)(off ? 0u : op_now(op)) / 255;
        /* The body of the fill is kept dim and the level itself drawn as a bright edge on top of
         * it. That leaves the number legible wherever the edge happens to be, and it is easier to
         * read a line than the end of a block when two operators differ by a little. */
        if (fw > 0) {
            cv_rect(x + 1, y + 1, fw, a_bh - 2, op_hue(op, 140u));
            cv_rect(x + fw - 1, y + 1, 2, a_bh - 2, op_hue(op, 255u));
        }
        if (wave && !shape) {                   /* not a sine: say so in the corner, small */
            int32_t mw = w - 4 < 9 ? w - 4 : 9;
            alg_wave(x + 2, y + a_bh - 5, mw, 2, wave, off ? RGB(110, 110, 110) : C_WHITE, 0);
        }
    }
    if (a_fb >> op & 1u) {                      /* the feedback loop, round the right-hand side,
                                                 * as bright as FEEDBACK is high: dim at 0, where
                                                 * nothing goes round it */
        uint32_t fb = edit[135] > 7u ? 7u : edit[135], t = 70u + fb * 26u;
        uint16_t fc = off ? RGB(60, 45, 35) : RGB(t, (t * 150u) / 200u, (t * 90u) / 200u);
        cv_line(x + a_bw, y + 3, x + a_bw + 4, y + 3, fc);
        cv_line(x + a_bw + 4, y + 3, x + a_bw + 4, y + a_bh - 3, fc);
        cv_line(x + a_bw + 4, y + a_bh - 3, x + a_bw, y + a_bh - 3, fc);
    }
    if (sel) {                                  /* the operator the OP and ENV pages are editing */
        cv_rect(x - 3, y - 3, a_bw + 6, 1, C_WHITE);
        cv_rect(x - 3, y + a_bh + 2, a_bw + 6, 1, C_WHITE);
        cv_rect(x - 3, y - 3, 1, a_bh + 6, C_WHITE);
        cv_rect(x + a_bw + 2, y - 3, 1, a_bh + 6, C_WHITE);
    }
    s[0] = (char)('0' + (6u - op));
    s[1] = 0;
    if (!shape && !wshape && a_bh >= 14)        /* the curve needs the room more than the number */
        cv_text(x + (a_bw - 5) / 2, y + (a_bh - 13) / 2, &FONT_S, s, off ? RGB(110, 110, 110) : C_WHITE);
    if (off)                                    /* struck through: switched off */
        cv_line(x + 1, y + a_bh - 2, x + a_bw - 2, y + 1, RGB(190, 85, 85));
}

static void draw_alg(void)
{
    uint32_t op, m, k, cs;
    alg_layout(edit[134]);
    alg_metrics();
    cs = carriers(edit[134]);
    alg_eg_build();
    cv_begin(240, ALG_H, C_BLACK);
    for (op = 0; op < 6u; op++) {               /* the wires first, so the boxes sit on top of them */
        int32_t cx = a_bx(op) + a_bw / 2, ty = a_by(op) + a_bh;
        m = a_mods[op];
        for (k = 0; k < 6u; k++) {
            /* a wire out of a switched-off operator carries nothing: draw it as nearly gone, so
             * the picture shows what is reaching the output and what is not */
            uint16_t wc = (opmute >> k & 1u) ? RGB(38, 38, 38) : C_OFF;
            if (!(m >> k & 1u)) continue;
            cv_line(a_bx(k) + a_bw / 2, a_by(k) + a_bh, a_bx(k) + a_bw / 2, ty + 3, wc);
            cv_line(a_bx(k) + a_bw / 2, ty + 3, cx, ty + 3, wc);
        }
        if (m) cv_line(cx, ty + 3, cx, ty, C_OFF);
    }
    for (op = 0; op < 6u; op++)                 /* a carrier reaches the output: show where it goes */
        if (cs >> op & 1u)
            cv_line(a_bx(op) + a_bw / 2, a_by(op) + a_bh, a_bx(op) + a_bw / 2, a_ybase + 3,
                    (opmute >> op & 1u) ? RGB(38, 38, 38) : C_OFF);
    /* The output bus goes red when every carrier is switched off. Solo a modulator and you hear
     * nothing -- which is the point, because a modulator is never heard, only felt -- and this is
     * the line that says so rather than leaving you wondering what broke. */
    cv_rect(a_x0, a_ybase + 3, a_dx * (int32_t)a_ncol, 2, (cs & ~(uint32_t)opmute) ? C_OFF : RGB(130, 50, 50));
    for (op = 0; op < 6u; op++)
        alg_box(a_bx(op), a_by(op), op, op == opsel && foc_op());
    cv_blit(0, ALG_Y);
}

/* Only the boxes, for the live levels: redrawing the whole diagram several times a second would
 * cost more of the CPU budget than the thing it is drawing. The patch is wide enough for the
 * selection ring on the left and the feedback bracket on the right, both of which alg_box draws,
 * and is clamped so a box against the right-hand edge cannot blit off the screen. */
static void draw_alg_levels(void)
{
    uint32_t op;
    alg_eg_build();
    for (op = 0; op < 6u; op++) {
        int32_t x = a_bx(op) - 3, y = a_by(op) - 3, w = a_bw + 13;
        if (w > a_dx + 3) w = a_dx + 3;         /* never reach into the next column's box */
        if (x < 0) x = 0;
        if (x + w > 240) w = 240 - x;
        cv_begin((uint32_t)w, (uint32_t)(a_bh + 6), C_BLACK);
        alg_box(a_bx(op) - x, 3, op, op == opsel && foc_op());
        cv_blit((uint32_t)x, (uint32_t)(ALG_Y + y));
    }
}

/* ---- the spectrum, HOME's second screen
 *
 * What the instrument is putting out, 1024 points of it, on a linear frequency axis. Linear is the
 * whole point: FM builds its harmonics as sidebands at the carrier plus and minus whole multiples
 * of the modulator, so on a linear axis they are a row of evenly spaced lines, and raising a
 * modulator's LEVEL makes that row visibly spread outward. That is the one picture worth having.
 *
 * It analyses the output and not a single operator, which sounds like the lesser thing but is not:
 * ARP and SEQ already switch an operator off and solo one, so soloing a carrier gives you exactly
 * that branch of the algorithm, with nothing here having to make a second claim about what the
 * engine is doing.
 *
 * The faint lines are whole multiples of the note being played. Partials that sit on them are
 * harmonic and partials that sit between them are not, which is the difference between a COARSE of
 * 2 and a COARSE of 2 with DETUNE, or between a ratio of 3 and a ratio of 3.5, and it is far easier
 * to see here than to hear. */
#define SP_Y ALG_Y
typedef char fmlab_spectrum_fits[(240u * SP_H <= CV_MAX) ? 1 : -1];

static uint8_t spspan = 2, sp_note;             /* the span the settings chose, and the note to mark up */
static uint8_t sp_pk[240];                      /* a peak that falls back slowly, so a pluck reads */

/* middle A is 440: one octave of equal temperament in Q8 Hz, starting at MIDI note 0 */
static const uint16_t SP_NOTE_HZ[12] = {2093, 2218, 2349, 2489, 2637, 2794,
                                        2960, 3136, 3322, 3520, 3729, 3951};
static uint32_t sp_hz(uint32_t note)
{
    if (note > 127u)
        note = 127u;
    return ((uint32_t)SP_NOTE_HZ[note % 12u] << (note / 12u)) >> 8;
}

#define C_FLT RGB(110, 200, 160)                /* the filter's curve over the spectrum */

static void draw_spectrum(void)
{
    uint32_t span = SP_SPAN[spspan], c, k, grid, f0 = 0;
    if (v_lit) {                                /* something is sounding: mark its harmonics */
        int32_t n = (int32_t)sp_note + (int32_t)edit[144] - 24;   /* the patch's own transpose */
        f0 = sp_hz((uint32_t)(n < 0 ? 0 : n));
    }
    cv_begin(240, SP_H, C_BLACK);
    /* Nothing crosses the plot: a ruler along the top for the harmonics and a scale along the
     * floor for the kilohertz, so the only tall things on the screen are the partials. */
    cv_rect(0, SP_H / 3u, 240, 1, RGB(22, 22, 22));       /* -24 dB */
    cv_rect(0, SP_H * 2u / 3u, 240, 1, RGB(22, 22, 22));  /* -48 dB */
    cv_rect(0, SP_H - 1, 240, 1, RGB(48, 48, 48));        /* and the floor the bars stand on */
    grid = span <= 2000u ? 500u : span <= 5000u ? 1000u : span <= 10000u ? 2000u : 5000u;
    for (k = grid; k < span; k += grid)
        cv_rect((int32_t)(k * 240u / span), SP_H - 5, 1, 4, RGB(70, 70, 70));
    /* The harmonics, but only the first sixteen and only while they are far enough apart to be
     * read as a comb: past that they stop being a ruler and turn into a hedge. */
    if (f0 * 240u / span >= 4u)
        for (k = 1u; f0 && k <= 16u && k * f0 < span; k++)
            cv_rect((int32_t)(k * f0 * 240u / span), 0, 1, k == 1u ? 9 : 5,
                    k == 1u ? RGB(150, 118, 210) : RGB(96, 76, 136));
    for (c = 0; c < 240u; c++) {
        uint32_t b0 = (c * span / 240u) * SP_N / FS;
        uint32_t b1 = ((c + 1u) * span / 240u) * SP_N / FS;
        uint32_t h = 0;
        if (b1 <= b0)
            b1 = b0 + 1u;                       /* a column narrower than a bin still shows one */
        if (b1 > SP_BINS)
            b1 = SP_BINS;
        for (k = b0; k < b1; k++)
            if (sp_bin[k] > h)
                h = sp_bin[k];
        sp_pk[c] = (uint8_t)(h > sp_pk[c] ? h : sp_pk[c] > 2u ? sp_pk[c] - 2u : 0u);
        if (h) {
            cv_rect((int32_t)c, (int32_t)(SP_H - h), 1, (int32_t)h, RGB(52, 62, 120));
            cv_rect((int32_t)c, (int32_t)(SP_H - h), 1, 1, C_WHITE);
        }
        if (sp_pk[c] > h)
            cv_rect((int32_t)c, (int32_t)(SP_H - sp_pk[c]), 1, 1, C_ACC);
    }
    /* The filter's own curve over what it is doing to the sound, on the spectrum's scale and its
     * linear axis, with the filter's 0 dB on the -24 dB line so a resonant peak has room above it.
     * Partials under the slope are the ones it is taking away. Its own colour, because lilac is
     * already the peaks. */
    if (flt_shown()) {
        uint32_t t = ui[UI_FTYPE];
        int32_t prev = 0, l20 = (int32_t)sp_log2_q8(20u * 16u);
        for (c = 0; c < 240u; c++) {
            uint32_t f16 = (2u * c + 1u) * span * 8u / 240u;      /* the column's middle, in Hz/16 */
            int32_t pos = (((int32_t)sp_log2_q8(f16) - l20) * 2543) >> 8;   /* 99 / log2(1000) */
            int32_t db = t ? flt_db16(pos, ui[UI_FCUT], ui[UI_FRES], t) : 0;
            int32_t yy = (int32_t)(SP_H / 3u) - db * (int32_t)SP_H / SP_RANGE;
            yy = yy < 0 ? 0 : yy > (int32_t)SP_H - 2 ? (int32_t)SP_H - 2 : yy;
            if (c)
                cv_line((int32_t)c - 1, prev, (int32_t)c, yy, C_FLT);
            prev = yy;
        }
    }
    /* The two things you have to know to read it, written on it: the note the ruler belongs to,
     * and how far across the screen reaches. */
    {
        char s[16];
        uint32_t i = 0;
        if (v_lit) {
            i = put(s, 0, NOTE_NAME[sp_note % 12u]);
            if (i && s[i - 1u] == ' ')
                i--;
            i = putn(s, i, sp_note / 12u >= 1u ? sp_note / 12u - 1u : 0u, 1);   /* middle C is C4 */
            s[i] = 0;
            cv_text(3, 14, &FONT_S, s, RGB(120, 100, 160));
        }
        i = put(s, 0, "0-");
        i = putn(s, i, span / 1000u, 1);
        i = put(s, i, "K");
        s[i] = 0;
        cv_text((int32_t)(237u - text_w(&FONT_S, s)), 14, &FONT_S, s, RGB(80, 80, 80));
    }
    cv_blit(0, SP_Y);
}

/* ---- the waveform, HOME's third screen
 *
 * The same tap the spectrum reads, drawn the other way up: amplitude against time, the way an
 * oscilloscope draws it. The spectrum says which partials there are; this says what they add up to,
 * which is the shape the ear gets -- and a modulator's LEVEL coming up bends a sine into it in front
 * of you.
 *
 * It stands still because it is triggered on the note. The period is known from the key, so the
 * screen is two periods long and starts on the steepest rising zero crossing inside the first one --
 * which an FM wave may cross several times a cycle, but crosses steepest at the same point each
 * cycle. The ring is 23 ms, so below about C3 it shows one period, and below F2 the whole ring as
 * it comes. The height follows the loudness, with a slow fall, so a quiet sound still fills it.
 * The ring is copied into the FFT's buffer first, which the spectrum is not using while this shows. */
static int32_t sc_pk = 512;

static void draw_scope(void)
{
    uint32_t i, c, per = 0, w = SP_N, trig = 0, r = sp_wr - SP_N;
    int32_t pk = 0, best = -1, mid = (int32_t)SP_H / 2, half = (int32_t)SP_H / 2 - 16, prev = 0;   /* under the labels */
    int32_t *x = sp_re;
    for (i = 0; i < SP_N; i++) {
        x[i] = sp_ring[(r + i) & (SP_N - 1u)];
        if ((x[i] < 0 ? -x[i] : x[i]) > pk)
            pk = x[i] < 0 ? -x[i] : x[i];
    }
    if (v_lit) {
        int32_t n = (int32_t)sp_note + (int32_t)edit[144] - 24;   /* the patch's own transpose */
        uint32_t f0 = sp_hz((uint32_t)(n < 0 ? 0 : n));
        per = f0 ? FS / f0 : 0u;
    }
    if (per && per * 3u <= SP_N)
        w = per * 2u;
    else if (per && per * 2u <= SP_N)
        w = per;
    else
        per = 0;
    for (i = 1u; per && i <= per; i++)          /* the steepest way up through zero */
        if (x[i - 1u] < 0 && x[i] >= 0 && x[i] - x[i - 1u] > best) {
            best = x[i] - x[i - 1u];
            trig = i;
        }
    if (w == SP_N)
        trig = 0;
    sc_pk = pk > sc_pk - sc_pk / 8 ? pk : sc_pk - sc_pk / 8;
    if (sc_pk < 512)
        sc_pk = 512;                            /* silence stays a flat line, not magnified hiss */

    cv_begin(240, SP_H, C_BLACK);
    cv_rect(0, mid, 240, 1, RGB(40, 40, 40));
    cv_rect(0, mid - half, 240, 1, RGB(22, 22, 22));
    cv_rect(0, mid + half, 240, 1, RGB(22, 22, 22));
    for (c = 0; c < 240u; c++) {
        int32_t lo, hi, last, ylo, yhi;
        if (w >= 240u) {                        /* several samples a column: all of them, as a span */
            uint32_t a = trig + c * w / 240u, b = trig + (c + 1u) * w / 240u;
            if (b <= a)
                b = a + 1u;
            lo = hi = x[a];
            for (i = a + 1u; i < b && i < SP_N; i++) {
                if (x[i] < lo) lo = x[i];
                if (x[i] > hi) hi = x[i];
            }
            last = x[(b - 1u) < SP_N ? b - 1u : SP_N - 1u];
        } else {                                /* fewer: read between them */
            uint32_t q = c * w * 256u / 240u, k = trig + (q >> 8);
            int32_t f = (int32_t)(q & 255u), a = x[k], b = x[k + 1u < SP_N ? k + 1u : k];
            lo = hi = last = a + (((b - a) * f) >> 8);
        }
        if (c) {                                /* joined to the column before */
            if (prev < lo) lo = prev;
            if (prev > hi) hi = prev;
        }
        prev = last;
        yhi = mid - hi * half / sc_pk;
        ylo = mid - lo * half / sc_pk;
        {
            int32_t yl = mid - last * half / sc_pk;
            if (yl < mid)
                cv_rect((int32_t)c, yl, 1, mid - yl, RGB(52, 62, 120));
            else if (yl > mid)
                cv_rect((int32_t)c, mid + 1, 1, yl - mid, RGB(52, 62, 120));
        }
        cv_rect((int32_t)c, yhi, 1, ylo - yhi + 1, C_WHITE);
    }
    {
        char s[16];
        uint32_t t;
        if (v_lit) {
            i = put(s, 0, NOTE_NAME[sp_note % 12u]);
            if (i && s[i - 1u] == ' ')
                i--;
            i = putn(s, i, sp_note / 12u >= 1u ? sp_note / 12u - 1u : 0u, 1);
            s[i] = 0;
            cv_text(3, 4, &FONT_S, s, RGB(120, 100, 160));
        }
        t = w * 100u / (FS / 100u);             /* the width of the screen, in tenths of a ms */
        i = putn(s, 0, t / 10u, 1);
        s[i++] = '.';
        s[i++] = (char)('0' + t % 10u);
        i = put(s, i, "MS");
        s[i] = 0;
        cv_text((int32_t)(237u - text_w(&FONT_S, s)), 4, &FONT_S, s, RGB(80, 80, 80));
    }
    cv_blit(0, SP_Y);
}

/* ---- the envelope on the page it has to itself */
#define EG_Y 44
#define EG_H 72                                 /* beside the two raw rows; ADSR needs only one */
#define EG_H_TALL 104
#define EGC_Y 152u                              /* where the row of four ADSR cells sits */
static int32_t eg_h = EG_H;                     /* how tall the graph is on the bank being shown */
static int32_t eg_y(int32_t level)              /* level is already logarithmic: 1 unit = 6 dB */
{
    if (level < 0) level = 0;
    if (level > EG_FULL) level = EG_FULL;
    return (eg_h - 3) - (int32_t)(((int64_t)level * (eg_h - 4)) / EG_FULL);
}

/* The four segments to scale, a held-sustain gap between 3 and 4, and a marker where the envelope
 * of the operator on screen has actually got to. `seg` is a mask of what is being edited: bits 0
 * to 3 are the segments, bit 4 the flat stretch where the key is held. The ADSR bank owns all of
 * it at once and gets no tick marks, because singling out everything singles out nothing. */
#define EG_HOLD 16u
#define EG_ALL 31u
static void draw_eg(const dx_env_t *e, const dx_env_t *live, uint32_t seg)
{
    int32_t x[5], lvl[5];
    uint32_t i;
    const int32_t x0 = 4, x1 = 236;

    eg_shape(e, x0, x1, 22, 3, x, lvl);
    cv_begin(240, (uint32_t)eg_h, C_BLACK);
    cv_rect(0, eg_h - 2, 240, 1, RGB(40, 40, 40));

    for (i = 0; i < 4u; i++) {
        uint32_t on = seg >> i & 1u;
        uint16_t c = on ? C_ACC : RGB(95, 100, 135);
        int32_t xa = (i == 3) ? x[3] : x[i], ya = eg_y(lvl[i]), yb = eg_y(lvl[i + 1]);
        if (i == 3) {                           /* the sustain, flat, before the release */
            cv_line(x[2] + 1, eg_y(lvl[3]), x[3], eg_y(lvl[3]),
                    (seg & EG_HOLD) ? C_ACC : RGB(90, 95, 120));
            ya = eg_y(lvl[3]);
        }
        cv_line(xa, ya, x[i + 1], yb, c);
        if (on && seg != EG_ALL) {              /* the ends of what the knobs are moving */
            cv_line(xa, eg_h - 2, xa, eg_h - 6, C_ACC);
            cv_line(x[i + 1], eg_h - 2, x[i + 1], eg_h - 6, C_ACC);
        }
    }

    if (live) {                                 /* where this operator's envelope is right now */
        uint32_t ix = live->ix < 4u ? live->ix : 3u;
        int32_t lo = lvl[ix], hi = lvl[ix + 1], l = live->level, xa, xb, px;
        xa = (ix == 3u) ? x[3] : x[ix];
        xb = x[ix + 1];
        px = (hi == lo) ? xb : xa + (int32_t)(((int64_t)(l - lo) * (xb - xa)) / (hi - lo));
        if (px < x0) px = x0;
        if (px > x1) px = x1;
        cv_rect(px - 1, eg_y(l) - 1, 3, 3, C_WHITE);
    }
    cv_blit(0, EG_Y);
}

/* The global envelope's picture. It is not a dx_env_t and it does not share eg_y: its level is a
 * plain amplitude and its axis is linear, where an operator's is 6 dB a unit. Drawing the two the
 * same way would be the lie that they are the same kind of thing.
 *
 * The time axis is proportional to the knob rather than to the millisecond, because the knob is
 * exponential and the milliseconds are not: two seconds of release drawn to scale beside a five
 * millisecond attack is one line and a dot. Proportional to the knob, a turn moves the picture by
 * as much as it moves the knob, which is the thing you are actually looking for -- and the number
 * under each slope says what it really is. In the middle is a fixed stretch for the key being
 * held, which is not a duration and should not be drawn as one.
 *
 * A stage switched off is drawn flat rather than wide: DECAY OFF stays at full and the sustain
 * line is not where the note goes, RELEASE OFF runs level off the right-hand edge, which is the
 * picture of a note this envelope has stopped having an opinion about. */
static int32_t g_plot_ratio(int32_t value, uint32_t width, uint32_t denominator)
{
    /* Q24 gains times a <=232-pixel span fit unsigned 32 bits, not signed 32 bits. */
    uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
    int32_t pixels = (int32_t)(magnitude*width/denominator);
    return value < 0 ? -pixels : pixels;
}

static void draw_geg(int32_t y, int32_t h)
{
    const int32_t x0 = 4, x1 = 236, hold = 40;
    int32_t top = 2, bot = h - 3, sy, wa, wd, wr, xa, xd, xh;
    uint32_t ta = ui[UI_GATK], td = ui[UI_GDEC], tr = ui[UI_GREL], tot;
    uint32_t sus = ui[UI_GSUS];
    int32_t live_sus = g_sus();                /* one snapshot: audio may change it between UI reads */
    uint32_t dof = td >= 99u, rof = tr >= 99u || tr == 0u;   /* nowhere to fall, either end */
    uint32_t off = ui[UI_GOFF];                 /* switched out: the shape kept, in grey */
    uint16_t c = off ? RGB(70, 70, 85) : C_ACC;

    sy = dof ? top : bot - (int32_t)((uint32_t)(bot - top) * sus / 99u);
    if (dof) td = 0u;
    if (rof) tr = 0u;
    tot = ta + td + tr;
    if (!tot) {
        wa = wd = wr = 2;
    } else {
        int32_t span = x1 - x0 - hold - 6;      /* 2 px a segment, whatever its time */
        wa = 2 + (int32_t)((uint32_t)span * ta / tot);
        wd = 2 + (int32_t)((uint32_t)span * td / tot);
        wr = 2 + (int32_t)((uint32_t)span * tr / tot);
    }
    xa = x0 + wa;
    xd = xa + wd;
    xh = xd + hold;
    if (rof) wr = x1 - xh;                      /* level, all the way out */

    cv_begin(240, (uint32_t)h, C_BLACK);
    cv_rect(0, h - 2, 240, 1, RGB(40, 40, 40));
    cv_line(x0, bot, xa, top, c);               /* up to full */
    cv_line(xa, top, xd, sy, c);                /* down to the sustain, or along the top */
    cv_line(xd, sy, xh, sy, off ? c : RGB(90, 95, 120));   /* held there, for as long as you hold it */
    cv_line(xh, sy, xh + wr, rof ? sy : bot, c);   /* and away, or not */

    if (off)
        cv_text((240 - text_w(&FONT_L, "OFF")) / 2, (h - 32) / 2, &FONT_L, "OFF", C_WHITE);
    else {   /* where the newest note's own copy of it has actually got to */
        uint32_t v = v_last;
        if (v < NV && dxv[v].on && (v_lit >> v & 1u) && genv[v].st != GS_OFF) {
            int32_t g = genv[v].g, py = bot - (int32_t)(((int64_t)g * (bot - top)) >> 24), px = xh;
            if (genv[v].st == GS_ATK)
                px = x0 + (int32_t)(((int64_t)g * wa) >> 24);
            else if (genv[v].st == GS_DEC)
                px = (live_sus >= G_ONE) ? xd
                   : xa + g_plot_ratio(G_ONE-g,(uint32_t)wd,(uint32_t)(G_ONE-live_sus));
            else if (rof)
                px = xh + wr;
            else if (live_sus > 0)
                px = xh + g_plot_ratio(live_sus-g,(uint32_t)wr,(uint32_t)live_sus);
            if (px < x0) px = x0;
            if (px > x1) px = x1;
            cv_rect(px - 1, py - 1, 3, 3, C_WHITE);
        }
    }
    cv_blit(0, (uint32_t)y);
}

/* The pitch envelope's picture. Pitch, not loudness: the line across the middle is the note you
 * played, up is sharp and down is flat, and the scale is in octaves -- one either way unless a
 * level reaches further, labelled in semitones so a LEVEL's number and its height say the same
 * thing. A DX7's pitch envelope starts at LEVEL 4, goes to 1, 2 and 3 at their RATEs, holds at 3
 * while the key is down, and goes back to 4 when it comes up.
 *
 * A rate here is a speed rather than a time -- the same rate crosses an octave in twice the time it
 * crosses a fifth -- so each slope is drawn as wide as it really takes, distance over speed, square
 * rooted so a slow glide and a snap can share a screen. Two levels the same take no time at all, so
 * they get no width. `seg` is the segments the knobs are on, bright; bit 0 is the first. */
static void draw_peg(int32_t y, int32_t h, uint32_t seg)
{
    const int32_t x0 = 4, x1 = 236, hold = 40;
    int32_t top = 3, bot = h - 4, mid = (top + bot) / 2, half = (bot - top) / 2;
    int32_t lv[5], x[6], span, k, mx = 0, sum = 0, w[4];
    uint32_t i, off = ui[UI_POFF];              /* switched out: the shape kept, in grey */
    char s[8];

    lv[0] = DX_PITCH[edit[133] % 100u];         /* from LEVEL 4, through 1, 2, 3, and back to 4 */
    lv[1] = DX_PITCH[edit[130] % 100u];
    lv[2] = DX_PITCH[edit[131] % 100u];
    lv[3] = DX_PITCH[edit[132] % 100u];
    lv[4] = DX_PITCH[edit[133] % 100u];
    for (i = 0; i < 5u; i++)
        if ((lv[i] < 0 ? -lv[i] : lv[i]) > mx)
            mx = lv[i] < 0 ? -lv[i] : lv[i];
    span = 32 * ((mx + 31) / 32);               /* whole octaves, 32 units each */
    if (span < 32) span = 32;
    for (i = 0; i < 4u; i++) {                  /* distance over speed, square rooted */
        int32_t d = lv[i + 1u] - lv[i];
        uint32_t t = (uint32_t)(d < 0 ? -d : d) * 4096u / DX_PRATE[edit[126u + i] % 100u], r = 0;
        while ((r + 1u) * (r + 1u) <= t)
            r++;
        w[i] = (int32_t)r;
        sum += w[i];
    }
    for (i = 0; i < 4u; i++) {                  /* to the room there is, around the held stretch */
        int32_t room = x1 - x0 - hold - 8;
        w[i] = sum ? 2 + w[i] * room / sum : 2 + room / 4;
    }
    x[0] = x0;
    x[1] = x[0] + w[0];
    x[2] = x[1] + w[1];
    x[3] = x[2] + w[2];
    x[4] = x[3] + hold;
    x[5] = x[4] + w[3];

    cv_begin(240, (uint32_t)h, C_BLACK);
    cv_rect(0, h - 2, 240, 1, RGB(40, 40, 40));
    for (k = 32; k <= span; k += 32) {          /* an octave either way, and the note itself */
        cv_rect(x0, mid - k * half / span, x1 - x0, 1, RGB(26, 26, 32));
        cv_rect(x0, mid + k * half / span, x1 - x0, 1, RGB(26, 26, 32));
    }
    cv_rect(x0, mid, x1 - x0, 1, RGB(48, 48, 60));
    i = 0;
    s[i++] = '+';
    i = putn(s, i, (uint32_t)(span * 3 / 8), 1);
    i = put(s, i, " ST");
    s[i] = 0;
    cv_text(6, top, &FONT_S, s, RGB(80, 80, 80));
    s[0] = '-';
    cv_text(6, bot - 14, &FONT_S, s, RGB(80, 80, 80));
    {
        int32_t yy[5];
        for (i = 0; i < 5u; i++)
            yy[i] = mid - lv[i] * half / span;
        for (i = 0; i < 4u; i++) {
            int32_t xa = i == 3u ? x[4] : x[i], xb = i == 3u ? x[5] : x[i + 1u];
            uint16_t c = off ? RGB(70, 70, 85) : (seg >> i & 1u) ? C_ACC : RGB(95, 100, 135);
            cv_line(xa, yy[i], xb, yy[i + 1u], c);
            if (!off && (seg >> i & 1u) && seg != 0xFu) {
                cv_line(xa, h - 2, xa, h - 6, C_ACC);
                cv_line(xb, h - 2, xb, h - 6, C_ACC);
            }
        }
        cv_line(x[3], yy[3], x[4], yy[3], off ? RGB(70, 70, 85) : RGB(90, 95, 120));   /* held there while the key is */
        if (off)
            cv_text((240 - text_w(&FONT_L, "OFF")) / 2, (h - 32) / 2, &FONT_L, "OFF", C_WHITE);
        else {   /* where the newest note has got to */
            uint32_t v = v_last;
            if (v < NV && dxv[v].on && (v_lit >> v & 1u)) {
                const dx_penv_t *pe = &dxv[v].penv;
                uint32_t ix = pe->ix;
                int32_t l = pe->level >> 12, px, py;   /* a 128th of a unit */
                if (ix >= 4u)
                    px = x[5];
                else if (ix == 3u && pe->down)
                    px = x[4];                  /* holding */
                else {
                    int32_t a = lv[ix] * 128, b = lv[ix + 1u] * 128;
                    int32_t xa = ix == 3u ? x[4] : x[ix], xb = ix == 3u ? x[5] : x[ix + 1u];
                    px = a == b ? xb : xa + (l - a) * (xb - xa) / (b - a);
                }
                if (px < x0) px = x0;
                if (px > x1) px = x1;
                py = mid - l * half / (span * 128);
                cv_rect(px - 1, py - 1, 3, 3, C_WHITE);
            }
        }
    }
    cv_blit(0, (uint32_t)y);
}

/* ---- the LFO, drawn
 *
 * Four numbers of the LFO's seven are things you can only really judge by looking: what shape it
 * is, how fast, how long before it arrives, and -- the one no number tells you at all -- where in
 * its cycle it happens to be right now. So the page draws it, the way the ENVELOPE page draws an
 * envelope, and for the same reason: adjusting a shape you cannot see is no way to learn what the
 * shape does.
 *
 * The wave is read out of the engine's own arithmetic rather than redrawn by hand. lfo_at is
 * dx_lfo_sample with the state left alone, and lfo_dly is dx_lfo_delay solved for a block number
 * instead of stepped to it, so the picture cannot drift away from the sound -- if the engine has
 * a quirk, the quirk is on the screen.
 *
 * The plot is four cycles wide whatever the speed, so the shape stays readable at both ends of
 * the range, and SPEED shows itself twice over: in how far the delay ramp reaches across those
 * four cycles, and in how fast the marker runs. The wave is drawn twice -- once dim at full
 * height, which is the LFO as it will be, and once bright at the height the delay is currently
 * letting through. With DELAY at 0 the two coincide and you see one bright wave; wind it up and
 * the bright one shrinks back into the dim one, which says "not yet, and this is how much of it"
 * without needing a second time axis or leaving the screen blank. */
#define LFO_CYC 4u

static int32_t lfo_at(uint32_t wave, uint32_t phase, uint8_t rnd)
{
    int32_t x;
    switch (wave) {                             /* dx_lfo_sample, without advancing anything */
    case 0:
        x = (int32_t)(phase >> 7);
        x ^= -(int32_t)(phase >> 31);
        return x & ((1 << 24) - 1);
    case 1: return (int32_t)((~phase ^ (1u << 31)) >> 8);
    case 2: return (int32_t)((phase ^ (1u << 31)) >> 8);
    case 3: return (int32_t)(((~phase) >> 7) & (1u << 24));
    case 4: return (1 << 23) + (dx_sin((int32_t)(phase >> 8)) >> 1);
    case 5: return ((int32_t)(rnd ^ 0x80) + 1) << 16;
    }
    return 1 << 23;
}

/* how much of the LFO is through the delay after n render blocks, Q24 */
static int32_t lfo_dly(uint32_t n, uint32_t inc, uint32_t inc2)
{
    uint64_t ds;
    uint32_t n1;
    if (inc == ~0u)                             /* DELAY 0: open from the first block */
        return 1 << 24;
    n1 = (1u << 31) / inc;                      /* blocks spent before it starts to open at all */
    if (n <= n1)
        return 0;
    ds = (uint64_t)(1u << 31) + (uint64_t)(n - n1) * inc2;
    if (ds > 0xFFFFFFFFull)
        return 1 << 24;
    return (int32_t)((ds >> 7) & ((1 << 24) - 1));
}

static void draw_lfo_plot(int32_t y, int32_t h)
{
    const int32_t x0 = 4, x1 = 236, top = 3, bot = h - 4;
    const uint32_t w = (uint32_t)(x1 - x0);
    uint32_t wave = edit[142] > 5u ? 0u : edit[142];
    uint32_t delta = DX_LFO_DELTA[edit[137] % 100u];
    uint32_t span, inc, inc2, i, prevc = 0xFFFFFFFFu;
    uint8_t rnd = 0;
    int32_t pd = 0, pb = 0;
    int a = 99 - (int)(edit[138] > 99u ? 99u : edit[138]);

    if (!delta)
        delta = 1u;
    span = (uint32_t)((((uint64_t)LFO_CYC) << 32) / delta);
    if (a == 99) {                              /* dx_lfo_reset, for the delay alone */
        inc = inc2 = ~0u;
    } else {
        a = (16 + (a & 15)) << (1 + (a >> 4));
        inc = DX_LFO_UNIT * (uint32_t)a;
        a &= 0xff80;
        inc2 = DX_LFO_UNIT * (uint32_t)a;
    }

    cv_begin(240, (uint32_t)h, C_BLACK);
    cv_rect(0, h - 2, 240, 1, RGB(40, 40, 40));
    cv_rect(x0, (top + bot) / 2, (int32_t)w, 1, RGB(38, 38, 46));   /* the middle: no change */

    for (i = 0; i <= w; i++) {
        uint64_t nb = (uint64_t)i * span / w;
        uint64_t ph = nb * delta;
        uint32_t c = (uint32_t)(ph >> 32);
        int32_t v, yd, yb;
        if (c != prevc) {                       /* sample and hold picks a new one each cycle */
            if (prevc != 0xFFFFFFFFu)
                rnd = (uint8_t)((rnd * 179 + 17) & 0xff);
            prevc = c;
        }
        v = lfo_at(wave, (uint32_t)ph, rnd);
        yd = bot - (int32_t)(((int64_t)v * (bot - top)) >> 24);
        v = (int32_t)(((int64_t)v * lfo_dly((uint32_t)nb, inc, inc2)) >> 24);
        yb = bot - (int32_t)(((int64_t)v * (bot - top)) >> 24);
        if (i) {
            cv_line(x0 + (int32_t)i - 1, pd, x0 + (int32_t)i, yd, RGB(70, 74, 100));
            cv_line(x0 + (int32_t)i - 1, pb, x0 + (int32_t)i, yb, C_ACC);
        }
        pd = yd;
        pb = yb;
    }

    {   /* where the newest note's own copy of it has actually got to, and how much is through */
        uint32_t v = v_last;
        if (v < NV && dxv[v].on && (v_lit >> v & 1u)) {
            uint32_t ph = dxv[v].lfo.phase, ds = dxv[v].lfo.delaystate;
            int32_t d = ds >= (1u << 31) ? (int32_t)((ds >> 7) & ((1 << 24) - 1)) : 0;
            int32_t lv = lfo_at(wave, ph, dxv[v].lfo.rnd);
            int32_t px = x0 + (int32_t)((((uint64_t)ph * w) >> 32) / LFO_CYC);
            int32_t py;
            lv = (int32_t)(((int64_t)lv * d) >> 24);
            py = bot - (int32_t)(((int64_t)lv * (bot - top)) >> 24);
            cv_rect(px - 1, py - 1, 3, 3, d > (1 << 23) ? C_WHITE : RGB(120, 120, 140));
        }
    }
    cv_blit(0, (uint32_t)y);
}

/* ---- the filter, drawn
 *
 * Its response, on the axis the ear uses: 20 Hz to 20 kHz evenly by octave, +24 dB to -48 dB, with
 * 100 Hz, 1 kHz and 10 kHz marked. The line across is 0 dB -- what goes through untouched -- and
 * the upright one is where CUTOFF is. Off, it is that flat line and nothing else, which is exactly
 * what off does. The curve is the filter's own arithmetic rather than a sketch of a filter, so a
 * resonant peak is as tall here as it is in the sound. */
/* The envelope to draw: the live one if this operator is sounding, otherwise one built the way a
 * note at middle C would build it, so the page is never blank. The shape always comes from `edit`,
 * so a switched-off operator still shows the envelope it would have -- but it gets no marker,
 * because the one the ISR is running has been taken to the floor and would sit there. */
static void draw_eg_now(uint32_t bank)
{
    /* bank 0 is all four ADSR knobs, so it owns the whole envelope and nothing is singled out;
     * the banks after it are the raw segments, one to a bank. */
    static const uint8_t EG_SEG[] = {EG_ALL, 0x1u, 0x2u, 0x4u, 0x8u};
    uint32_t seg = EG_SEG[bank < sizeof EG_SEG ? bank : 0];
    dx_env_t scratch;
    eg_h = bank == 0u || eg_pit() ? EG_H_TALL : EG_H;   /* a bank of four cells needs one row,
                                                         * so the graph gets the rest */
    if (eg_glo()) {
        draw_geg(EG_Y, eg_h);
        return;
    }
    if (eg_pit()) {
        draw_peg(EG_Y, eg_h, bank ? 0xCu : 0x3u);
        return;
    }
    {
    uint32_t v = v_last;
    const uint8_t *o = &edit[opsel * 21u];
    int rs = 0, ol = dx_op_level(o, 60, 100, &rs);
    dx_env_init(&scratch, o, o + 4, ol, rs);
    if (v < NV && dxv[v].on && (v_lit >> v & 1u) && !(opmute >> opsel & 1u))
        draw_eg(&scratch, &dxv[v].env[opsel], seg);
    else
        draw_eg(&scratch, 0, seg);
    }
}

/* ---- RATIO is two rows in one, and changes its name with the mode. So the name, the short name
 * under the knob, the bar under the value and the question "has this row changed?" all have to be
 * asked for rather than read straight off the table. */
static const char *p_name(const param_t *p)
{
    return (p->kind == K_RAT && *op_byte(opsel, 17)) ? "FIXED HZ" : p->name;
}
static const char *p_knob(const param_t *p)
{
    if (p->kind == K_RAT && *op_byte(opsel, 17)) return "FIX HZ";
    return p->knob ? p->knob : p->name;
}
static uint32_t p_bar(const param_t *p, uint32_t v, uint32_t *max)
{
    uint32_t exact;
    if (p->kind == K_TIME || p->kind == K_DEC) { *max = p->max; return 99u - (v < 99u ? v : 99u); }
    if (p->kind != K_RAT) { *max = p->max; return v; }
    if (*op_byte(opsel, 17)) { *max = 399u; return fix_step(opsel); }
    *max = RAT_N - 1u;
    return rat_index(opsel, &exact);
}
static uint32_t p_cache(const param_t *p)       /* RATIO watches three bytes, not one */
{
    if (p->kind == K_RAT)
        return ((uint32_t)(*op_byte(opsel, 17) != 0) << 12) |
               ((uint32_t)*op_byte(opsel, 18) << 7) | *op_byte(opsel, 19);
    return *p_ref(p);
}

/* One parameter row: name on the left, value on the right, and a bar underneath so the amount is
 * visible at a glance. The knob that moves it is KNOB1..4, top to bottom. */
static void draw_row(uint32_t slot, const param_t *p, uint32_t v)
{
    char s[32];
    uint32_t y = PAGE[page].y0 + slot * 32u, i;
    cv_begin(240, 30, C_BLACK);
    if (p) {
        uint32_t max, n = p_bar(p, v, &max);
        uint32_t w = max ? (228u * n) / max : 0u;
        cv_rect(6, 24, 228, 4, RGB(45, 45, 45));
        if (w) cv_rect(6, 24, (int32_t)w, 4, C_ACC);
    }
    cv_blit(0, y);
    if (!p) {
        draw_text_box(0, y, 240, &FONT_S, "", C_OFF, 1);
        return;
    }
    i = put(s, 0, p_name(p));
    s[i] = 0;
    draw_text_box(8, y, 150, &FONT_S, s, C_LBL, 0);
    i = putval(s, 0, p, v);
    s[i] = 0;
    draw_text_box(150, y, 84, &FONT_S, s, C_WHITE, 2);
}

/* ADSR four to a row. Two columns of two would have fitted as well, but these cells line up with
 * the four knobs and with the legend under them, and a value you cannot match to a knob is half a
 * value. The bar is a quarter as wide as a full row's and says exactly the same thing.
 *
 * The LFO page has three to a bank instead, because four names and four values in a quarter of
 * the screen each came out as four letters of every name, and WAVE's value ran over its own. A
 * third each is room for both; the legend under them still sits under the knobs. */
static void draw_eg_cells(void)
{
    uint32_t e, nc = bank_rows(bank) < 4u ? bank_rows(bank) : 4u;
    uint32_t cw = nc == 3u ? 80u : 60u, in = cw - 6u;
    cv_begin(240, 32, C_BLACK);
    for (e = 0; e < nc; e++) {
        const param_t *p = page_at(bank_first(bank) + e);
        uint32_t max, n, w;
        if (e)
            cv_rect((int32_t)(e * cw), 2, 1, 22, RGB(44, 44, 44));
        if (!p)
            continue;
        n = p_bar(p, *p_ref(p), &max);
        w = max ? (in * n) / max : 0u;
        cv_rect((int32_t)(e * cw + 3u), 26, (int32_t)in, 4, RGB(45, 45, 45));
        if (w)
            cv_rect((int32_t)(e * cw + 3u), 26, (int32_t)w, 4, C_ACC);
    }
    cv_blit(0, EGC_Y);
    for (e = 0; e < nc; e++) {                  /* the text goes on top, as it does on a full row */
        const param_t *p = page_at(bank_first(bank) + e);
        char s[16];
        uint32_t i;
        if (!p)
            continue;
        if (cw != 60u) {                        /* wide: the value as wide as it needs, the name
                                                 * in whatever is left */
            uint32_t vw, v = *p_ref(p);
            i = p->kind == K_WAVE ? put(s, 0, WAVE_S[v < 6u ? v : 0u]) : putval(s, 0, p, v);
            s[i] = 0;
            vw = (uint32_t)text_w(&FONT_S, s) + 1u;
            if (vw > in)
                vw = in;
            draw_text_box(e * cw + 3u + in - vw, EGC_Y + 4u, (int32_t)vw, &FONT_S, s, C_WHITE, 2);
            if (in - vw >= 10u) {
                i = put(s, 0, p_knob(p));
                s[i] = 0;
                draw_text_box(e * cw + 3u, EGC_Y + 4u, (int32_t)(in - vw - 4u), &FONT_S, s, C_LBL, 0);
            }
            continue;
        }
        if (eg_glo() || eg_pit()) {             /* a real time needs five characters; the knob
                                                 * legend under it is already naming the four */
            i = putval(s, 0, p, *p_ref(p));
            s[i] = 0;
            draw_text_box(e * 60u + 3u, EGC_Y + 4u, 54, &FONT_S, s, C_WHITE, 1);
            continue;
        }
        i = put(s, 0, p_knob(p));
        s[i] = 0;
        draw_text_box(e * 60u + 3u, EGC_Y + 4u, 28, &FONT_S, s, C_LBL, 0);
        i = putval(s, 0, p, *p_ref(p));
        s[i] = 0;
        draw_text_box(e * 60u + 31u, EGC_Y + 4u, 26, &FONT_S, s, C_WHITE, 2);
    }
}

/* The focus knobs have no row on the screen to write into, so they borrow the voice line, which is
 * where every other "you just did something" message goes. */
static void say_param(const param_t *p)
{
    uint32_t i = put(hk_say, 0, p_knob(p));
    hk_say[i++] = ' ';
    i = putval(hk_say, i, p, *p_ref(p));
    hk_say[i] = 0;
    say(hk_say);
}

/* HOME's banner, which stops being the instrument's name the moment the knobs stop being the
 * instrument's and become one operator's. */
static void draw_home_title(void)
{
    char s[32];
    uint32_t i;
    if (focus == FOC_OFF)
        i = put(s, 0, home_flt() ? "FILTER" : bank == 2u ? "SCOPE" : APP_NAME);
    else if (focus == FOC_FX)
        i = put(s, 0, "FX MIX");
    else if (eg_glo())
        i = put(s, 0, "ENV GLOBAL");
    else if (eg_pit())
        i = put(s, 0, "PITCH");
    else if (lfo_glo())
        i = put(s, 0, "LFO GLOBAL");
    else {
        i = put(s, 0, FOC_NAME[focus - 1u]);
        if (focus == FOC_ARP)
            i = put(s, i, arp_pg ? " 2/2" : " 1/2");
        if (foc_op()) {                         /* ..of one operator, and which, if it is one */
            i = put(s, i, " OP");
            i = putn(s, i, 6u - opsel, 1);
        }
    }
    s[i] = 0;
    lcd_fill(0, 18, 240, 32, C_BLACK);
    draw_text_box(0, 18, 240, &FONT_L, s, C_ACC, 1);
}

static void draw_head(void)
{
    char s[32];
    uint32_t i, nb, perop;
    /* Focused, the title stops naming the screen and names what the knobs and the black keys have
     * become -- which is the only warning you get that part of the keyboard has stopped playing.
     * Either way it names an operator exactly when one of the knobs is on that operator's byte. */
    if (focus != FOC_OFF && page == PAGE_HOME) {
        i = put(s, 0, FOC_NAME[focus - 1u]);
        perop = foc_op();
    } else {
        i = put(s, 0, page == PAGE_FX ? FX_NAME[bank < FX_N ? bank : 0u] : PAGE[page].name);
        perop = bank_has_op();
    }
    if (eg_glo() || lfo_glo()) {                /* not an operator at all: the whole voice */
        i = put(s, i, "  GLOBAL");
        perop = 0;
    }
    if (eg_pit()) {                             /* ..and nor is this */
        i = put(s, i, "  PITCH");
        perop = 0;
    }
    if (perop) {
        i = put(s, i, "  OP");
        i = putn(s, i, 6u - opsel, 1);
        /* a carrier is heard; a modulator is only felt. Which one an operator is depends on the
         * algorithm, so it has to be read out of the table rather than remembered. */
        i = put(s, i, (carriers(edit[134]) >> opsel & 1u) ? " CAR" : " MOD");
        if (opmute >> opsel & 1u)
            i = put(s, i, " OFF");
    }
    nb = page_nbank();
    if (nb > 1u) {
        i = put(s, i, "  ");
        i = putn(s, i, bank + 1u, 1);
        s[i++] = '/';
        i = putn(s, i, nb, 1);
    }
    s[i] = 0;
    draw_text_box(0, 24, 240, &FONT_S, s, C_ACC, 1);
}

/* ---- what the four knobs do, under the four knobs
 *
 * The knobs are a row of four under the screen; the parameters they move are a column of four on
 * it. Everyone works that mapping out in a second or two and then works it out again every time
 * they come back to the instrument. This strip says it instead: four cells, left to right, in the
 * order your hand finds them. On the pages that already name each parameter beside its value it
 * is saying the same thing twice, which is the price of its being in the same place on every
 * screen -- somewhere you can glance without first deciding whether it is worth glancing at. */
#define LEGEND_Y 208u
#define STATUS_Y 224u

static void draw_diagnostics(uint32_t voices)
{
    static const char *const names[] = {"CPU","PEAK","SAMPLE RATE","BUILD"};
    char text[64];
    uint32_t i, n;
#ifndef DINGHY_HOST
    cpu_profile_t parts = cpu_profiles[cpu_profile_index];
#endif
    cv_begin(240,72,C_BLACK);
#ifdef DINGHY_HOST
    cv_text(4,0,&FONT_S,"HOST / NO DEVICE TIMING",C_LBL);
    n = putn(text,put(text,0,"VOICES "),voices,1);
    text[put(text,n," / 8")] = 0;
    cv_text(4,18,&FONT_S,text,C_WHITE);
    cv_text(4,36,&FONT_S,"LATE: NOT DEVICE",C_WHITE);
#else
    n = putn(text,put(text,0,"PK SYN "),parts.synth,1);
    n = putn(text,put(text,n,"% FX "),parts.fx,1); text[put(text,n,"%")] = 0;
    cv_text(4,0,&FONT_S,text,C_LBL);
    n = putn(text,put(text,0,"CTL "),parts.control,1);
    n = putn(text,put(text,n,"% POST "),parts.post,1); text[put(text,n,"%")] = 0;
    cv_text(4,18,&FONT_S,text,C_WHITE);
    n = putn(text,put(text,0,"VOICES "),voices,1);
    text[putn(text,put(text,n," / 8  LATE "),cpu_late,1)] = 0;
    cv_text(4,36,&FONT_S,text,C_WHITE);
#endif
    cv_text(4,54,&FONT_S,"SEL: RESET PEAK / LATE",C_LBL);
    cv_blit(0,44);
    for (i = 0; i < 4u; i++) {
        if (i < 2u) {
#ifdef DINGHY_HOST
            n = put(text,0,"NOT DEVICE");
#else
            n = putn(text,0,i ? parts.percent : cpu_q8*100u/256u,1);
            n = put(text,n,"%");
#endif
        } else if (i == 2u) n = putval(text,0,&PG_GLO[PG_GLO_SRATE],ui[UI_SRATE]);
        else n = put(text,0,APP_VER);
        text[n] = 0;
        cv_begin(240,16,C_BLACK);
        cv_text(8,0,&FONT_S,names[i],C_LBL);
        cv_text(234-text_w(&FONT_S,text),0,&FONT_S,text,C_WHITE);
        cv_blit(0,120u+i*16u);
    }
}

static void draw_legend(void)
{
    uint32_t e;
    cv_begin(240, 16, C_BLACK);
    for (e = 1u; e < 4u; e++)   /* without these, two seven-letter names run into one word */
        cv_rect((int32_t)(e * 60u), 4, 1, 8, RGB(44, 44, 44));
    for (e = 0; e < 4u; e++) {
        const char *s = 0;
        char buf[8];
        if ((focus != FOC_OFF && page == PAGE_HOME) || home_flt()) {
            const param_t *p = focus_at(e);     /* one operator, four of the things it is made of */
            if (p)
                s = p_knob(p);
        } else if (page == PAGE_HOME) {
            uint32_t n = home_op(e);             /* four operators at a time, levels or ratios */
            if (home_fb(e))
                s = p_knob(&PG_GLO[PG_GLO_FB]);
            else if (n) {
                uint32_t i = put(buf, 0, HSET[homeset].what == HK_LVL ? "LVL " : "RAT ");
                buf[i++] = (char)('0' + n);
                buf[i] = 0;
                s = buf;
            }
        } else if (page == PAGE_STEP) {
            s = e == 0u ? "LENGTH" : p_knob(&PG_SEQ[e - 1u]);   /* ..and SEQ's TEMPO, BARS, DIV */
        } else {
            const param_t *p = e < bank_rows(bank) ? page_at(bank_first(bank) + e) : 0;
            if (p)
                s = p_knob(p);
        }
        if (s) {
            uint32_t w = text_w(&FONT_S, s), pad = w + 6u < 60u ? (60u - w) / 2u : 3u;
            cv_text((int32_t)(e * 60u + pad), 0, &FONT_S, s, sel_fine() ? C_ACC : C_OFF);
        }
    }
    cv_blit(0, LEGEND_Y);
}

/* ---- the two corners
 *
 * Sixteen pixels across the top of every screen that nothing else ever wanted: the page title sits
 * at 24 and HOME's banner at 18. Two things go in it, one at each end, and neither is worth a word.
 *
 * Left, the battery. Three bars, because three is all the precision there is to have -- the
 * divider on PB1 and the SARADC's reference are both undocumented, so the thresholds below are
 * measurements rather than arithmetic, taken from dxsloop (GPL-3.0, Leo Kuroshita) which arrived
 * at them on real hardware. The whole useful span is about sixty counts wide, which is why the
 * reading is smoothed over thirty-two samples before it gets here. Plugged into a host the bars
 * cycle instead: there is no charger status line on this chip, so a host being there is the only
 * evidence of charging available, and an animation says "being charged" without claiming to know
 * how far along it is.
 *
 * Right, which of HOME's three screens you are on. The screen itself tells you which one you are
 * looking at; what it cannot tell you is that there are others, which is the whole job of a row
 * of pips. Pages other than HOME already say 1/4 in their title, so they get none. On GLOBAL, the
 * page of the instrument rather than the sound, that corner says which FM Lab this is instead. */
#define BATT_1 531                              /* raw counts: anything under this is the last bar */
#define BATT_2 561
#define BATT_3 591

static uint32_t corner_sig = 1;

static uint32_t batt_bars(void)
{
    return batt_raw >= BATT_3 ? 3u : batt_raw >= BATT_2 ? 2u : batt_raw >= BATT_1 ? 1u : 0u;
}

static void draw_corners(void)
{
    uint32_t charging = fm1_usb_host() != 0, lvl = batt_bars(), pips, ver, k, sig;
    uint16_t c = charging ? C_ACC : lvl == 0u ? RGB(255, 90, 90) : lvl == 1u ? RGB(235, 170, 60) : C_OFF;
    if (charging)
        lvl = 1u + (fm1_ms / 600u) % 3u;        /* filling, over and over, while a host is there */
    pips = page == PAGE_HOME ? HOME_NBANK : 0u;
    ver = page == PAGE_GLO;
    sig = lvl * 7u + charging * 97u + pips * 1013u + (uint32_t)bank * 31u + c + ver * 50021u;
    if (sig == corner_sig)
        return;
    corner_sig = sig;
    cv_begin(240, 16, C_BLACK);
    cv_rect(6, 3, 17, 1, c);                    /* the cell: a box, and the nub on its end */
    cv_rect(6, 11, 17, 1, c);
    cv_rect(6, 3, 1, 9, c);
    cv_rect(22, 3, 1, 9, c);
    cv_rect(23, 5, 2, 5, c);
    for (k = 0; k < lvl; k++)
        cv_rect(8 + (int32_t)k * 5, 5, 3, 5, c);
    for (k = 0; k < pips; k++)
        cv_rect(238 - (int32_t)pips * 10 + (int32_t)k * 10, 5, 6, 6, k == bank ? C_ACC : RGB(56, 56, 64));
    if (ver) {                                  /* tools/build.py --release X.Y.Z sets it; DEV if not */
        const char *v = APP_VER[0] >= '0' && APP_VER[0] <= '9' ? "V" APP_VER : APP_VER;
        cv_text(234 - text_w(&FONT_S, v), 0, &FONT_S, v, C_OFF);
    }
    cv_blit(0, 0);
}

static void draw_status(int32_t oct, uint32_t nv, uint32_t cpu, uint32_t pk)
{
#ifdef DINGHY_HOST
    if (glo_diagnostics()) {
        draw_text_box(0,STATUS_Y,240,&FONT_S,"HOST / NOT DEVICE TIMING",C_OFF,1);
        return;
    }
#endif
    char s[32];
    uint32_t i = put(s, 0, "OCT");
    s[i++] = oct < 0 ? '-' : '+';
    s[i++] = (char)('0' + (oct < 0 ? -oct : oct));
    if (monomode) {                             /* the voice count says nothing when there is one */
        i = put(s, i, " MONO");
    } else {
        i = put(s, i, " VOX");
        i = putn(s, i, nv, 1);
    }
    i = put(s, i, " CPU");
    i = putn(s, i, cpu > 999u ? 999u : cpu, 3);
    s[i++] = '%';
    i = put(s, i, " PK");
    i = putn(s, i, pk > 999u ? 999u : pk, 3);
    s[i++] = '%';
    s[i] = 0;
    draw_text_box(0, STATUS_Y, 240, &FONT_S, s, pk > 85u ? RGB(255, 110, 110) : C_OFF, 1);
}

/* ---- the main loop */
static void load_voice(uint32_t vi)
{
    if (vi < DX_NVOICES) {
        memcpy(edit, DX_VOICES[vi], 156);
        memset(edit + DX_WAVE, 0, 6);           /* the factory voices are a DX7's: sines */
    } else
        dx_unpack(bankv + (vi - DX_NVOICES) * 128u, edit);
    dx_sanitize(edit);
    modified = 0;
    opmute = 0;                                 /* a new voice arrives with all six operators on */
    patch_publish();
}

/* The operator switches. ON/OFF is the primitive and SOLO is everything-else-off, which is the
 * pair that gets used: ON/OFF to ask what one operator was contributing, SOLO to hear it alone.
 * Soloing a modulator is silent, and is meant to be -- see the output bus in draw_alg. */
static void say_op(uint32_t op, const char *what)
{
    uint32_t i = put(hk_say, 0, "OP");
    i = putn(hk_say, i, 6u - op, 1);
    hk_say[i++] = ' ';
    i = put(hk_say, i, what);
    hk_say[i] = 0;
    say(hk_say);
}

static void op_toggle(uint32_t op)
{
    opmute ^= (uint8_t)(1u << op);
    say_op(op, (opmute >> op) & 1u ? "OFF" : "ON");
    patch_publish();
}

static void op_solo(uint32_t op)
{
    uint8_t only = (uint8_t)(0x3Fu & ~(1u << op));
    opmute = (opmute == only) ? 0u : only;      /* solo the same one twice: everything back on */
    if (opmute)
        say_op(op, "SOLO");
    else
        say("ALL OPS ON");
    patch_publish();
}

/* A/B. The switch swaps the two patches outright, so every page keeps reading `edit` and nothing
 * else in the file has to know the compare exists. The voice number and the edited mark travel
 * with the patch, or the name under the diagram would belong to the other side. */
static void ab_swap(uint16_t *voice)
{
    uint32_t i;
    uint8_t t;
    for (i = 0; i < DX_VSZ; i++) { t = edit[i]; edit[i] = other[i]; other[i] = t; }
    t = modified;   modified = other_mod;   other_mod = t;
    { uint16_t w = *voice; *voice = other_voice; other_voice = w; }
    ab ^= 1u;
    patch_publish();
}

static void ab_copy(uint16_t voice)             /* this side onto the other: a place to come back to */
{
    memcpy(other, edit, DX_VSZ);
    other_mod = modified;
    other_voice = voice;
}

/* ---- .syx in and out
 *
 * Thirty years of DX7 patches are a MIDI cable away, and reading other people's voices is most of
 * how anyone learns this. The FM-1 speaks the DX7's own dump format, so a .syx file off the net
 * goes straight in and a voice you built here comes straight back out.
 *
 *   F0 43 0n 09 20 00 <4096> sum F7   32 voices: a whole bank, written to flash -- the bank you are
 *                                     in (the voice's, or on a factory voice the last one saved to)
 *   F0 43 0n 00 01 1B  <155> sum F7   one voice: into the editor, as a DX7 does -- it lands in what
 *                                     you are playing and you choose whether to keep it
 *   F0 43 2n 09 F7 / F0 43 2n 00 F7   the host asking for either of those back
 *
 * The channel nibble is ignored on the way in and sent as 0. A bad checksum is still our message:
 * saying so is more use than handing it to the updater, which would only drop it.
 *
 * This runs in the main loop, from platform_poll, so a bank write stops the sound for the ~100 ms
 * of the two erases. That is the only time FM Lab deliberately interrupts itself. */
static uint8_t sx_bank;                         /* the bank you are in: the voice's, or the last saved to */

static void say_bank(uint32_t b, const char *what)   /* "BANK 2 RECEIVED" */
{
    static char m[24];
    uint32_t i = put(m, 0, "BANK ");
    m[i++] = (char)('1' + b);
    m[i++] = ' ';
    i = put(m, i, what);
    m[i] = 0;
    say(m);
}

static int app_sysex(const uint8_t *p, uint32_t n)
{
    uint8_t u[DX_VSZ];
    uint32_t sub, b = sx_bank;
    if (n < 3u || p[0] != 0x43u)
        return 0;                               /* not Yamaha's: let the updater look at it */
    sub = p[1] >> 4;
    if (sub == 2u && n == 3u) {                 /* a request for a dump */
        if (p[2] == 0x09u) {
            memcpy(bankmsg + 6, bankv + b * BANK_V * 128u, BANK_V * 128u);
            bankmsg[6u + BANK_V * 128u] = dx_syx_sum(bankmsg + 6, BANK_V * 128u);
            say_bank(b, sysex_send(bankmsg, sizeof bankmsg) ? "SEND FAILED" : "SENT");
        } else if (p[2] == 0x00u) {
            uint8_t m[163];
            m[0] = 0xF0; m[1] = 0x43; m[2] = 0x00; m[3] = 0x00; m[4] = 0x01; m[5] = 0x1B;
            memcpy(m + 6, edit, 155);           /* the editor, not the stored voice: what you hear */
            m[161] = dx_syx_sum(m + 6, 155);
            m[162] = 0xF7;
            say(sysex_send(m, sizeof m) ? "VOICE SEND FAILED" : "VOICE SENT");
        } else
            return 0;
        return 1;
    }
    if (sub != 0u || n < 5u)
        return 0;
    if (p[2] == 0x09u && p[3] == 0x20u && p[4] == 0x00u && n == 5u + BANK_V * 128u + 1u) {
        if (dx_syx_sum(p + 5, BANK_V * 128u) != p[n - 1u])
            say("BANK BAD CHECKSUM");
        else {
            memcpy(bankv + b * BANK_V * 128u, p + 5, BANK_V * 128u);
            say_bank(b, bank_put(b) ? "WRITE FAILED" : "RECEIVED");
        }
        return 1;
    }
    if (p[2] == 0x00u && p[3] == 0x01u && p[4] == 0x1Bu && n == 5u + 155u + 1u) {
        if (dx_syx_sum(p + 5, 155u) != p[n - 1u])
            say("VOICE BAD CHECKSUM");
        else {
            memset(u, 0, sizeof u);             /* the DX7's 155 has no room for a wave: sines */
            memcpy(u, p + 5, 155);
            dx_sanitize(u);
            memcpy(edit, u, DX_VSZ);
            opmute = 0;
            modified = 1;                       /* it is not any stored voice: it is an edit */
            patch_publish();
            say("VOICE RECEIVED");
        }
        return 1;
    }
    return 0;
}

/* ---- the clock, the arpeggiator and the sequencer: the main loop's half
 *
 * The arpeggiator takes the keys you would otherwise have played. Unlatched it walks the ones held
 * down, and stops when you let go. Latched it keeps walking the last chord after you let go, and
 * the next key pressed with none held starts a new chord in its place -- which is the one that
 * leaves both hands free. ARP's first tap puts its knobs on HOME; a tap there turns it on and off.
 *
 * The sequencer is a loop of BARS, and REC is one time round it. Stopped, REC counts a bar in on
 * the click and then records from the downbeat; playing, it records over what is there, from
 * wherever the loop is. Either way it stops recording by itself when it has been all the way round,
 * so there is no button to hit in time, and the loop goes on playing. Every note snaps to the
 * nearest DIV -- its start and its end -- so the playing can be loose and still loop. A hold of REC
 * throws the phrase away. Notes are counted on the ISR's clock, not the main loop's, so what is
 * recorded is exactly where the loop will play it. */
static uint8_t seq_run, seq_gen, seq_cin, arp_on, rec, pl_dirty;   /* rec: 1 to begin where the loop is,
                                                                   * 2 taking notes */
static uint8_t chord[ARP_MAX], chord_n;
static uint8_t chord_ord[ARP_MAX];              /* the same keys, in the order they went down */
static uint32_t arpkeys;                        /* the keys that are the arpeggiator's, down */
static sqn_t sq[SQ_MAX];                        /* the phrase */
static uint8_t sq_n;
static uint32_t rec_from, rec_to;               /* the recording, in steps since PLAY */
static uint32_t rkeys, rk_st[27];               /* keys down being recorded, and the step each began on */
static uint8_t rk_note[27];

static uint32_t bpm(void)
{
    return 40u + 2u * ui[UI_TEMPO];
}

static uint32_t sq_len(void) { return 16u << ui[UI_BARS]; }
static uint32_t sq_div(void) { return 1u << ui[UI_DIV]; }  /* in sixteenths */

/* Where the loop is, in Q8 sixteenths since PLAY -- or 0 when it is not running, or the ISR has not
 * yet taken this PLAY. The ISR can land between two reads, so they are read until they agree. */
static uint32_t clock_at(uint32_t *q8)
{
    uint32_t st, ph, g;
    do {
        st = pl_at;
        ph = pl_atph;
        g = pl_atgen;
    } while (st != pl_at || ph != pl_atph);
    if (!seq_run || g != seq_gen)
        return 0;
    *q8 = st * 256u + ph * 256u / (FS * 15u * 256u / bpm());
    return 1;
}

static uint32_t q_round(uint32_t q8)            /* the nearest DIV, in sixteenths */
{
    uint32_t d = sq_div();
    return (q8 + d * 128u) / (d * 256u) * d;
}

static void chord_add(uint8_t n)
{
    uint32_t i, j;
    for (i = 0; i < chord_n && chord[i] < n; i++)
        ;
    if ((i < chord_n && chord[i] == n) || chord_n >= ARP_MAX)
        return;
    for (j = chord_n; j > i; j--)
        chord[j] = chord[j - 1u];
    chord[i] = n;
    chord_ord[chord_n] = n;
    chord_n++;
}

static void chord_del(uint8_t n)
{
    uint32_t i;
    for (i = 0; i < chord_n && chord[i] != n; i++)
        ;
    if (i >= chord_n)
        return;
    for (; i + 1u < chord_n; i++)
        chord[i] = chord[i + 1u];
    for (i = 0; chord_ord[i] != n; i++)
        ;
    for (; i + 1u < chord_n; i++)
        chord_ord[i] = chord_ord[i + 1u];
    chord_n--;
}

static void sq_put(uint8_t note, uint32_t st, uint32_t len)   /* st in steps since PLAY */
{
    uint32_t i, n = sq_len(), pos = (st - seq_cin) & (n - 1u);
    if (len > n)
        len = n;
    for (i = 0; i < sq_n; i++)                  /* played again where it already was: replaced */
        if (sq[i].step == pos && sq[i].note == note)
            break;
    if (i == sq_n) {
        if (sq_n >= SQ_MAX) {
            say("SEQ FULL");
            return;
        }
        sq_n++;
    }
    sq[i].step = (uint8_t)pos;
    sq[i].note = note;
    sq[i].len = (uint8_t)(len ? len : 1u);
    pl_dirty = 1;
}

static void rec_down(uint32_t k, uint8_t note)  /* a key played while recording */
{
    uint32_t q, r;
    if (rec != 2u || !clock_at(&q))
        return;
    r = q_round(q);
    if (r < rec_from)                           /* the count-in, and not close enough to its end */
        return;
    rkeys |= 1u << k;
    rk_st[k] = r;
    rk_note[k] = note;
}

static void rec_close(uint32_t k, uint32_t e)   /* key k's note ends at step e */
{
    rkeys &= ~(1u << k);
    sq_put(rk_note[k], rk_st[k], e > rk_st[k] ? e - rk_st[k] : sq_div());
}

static void rec_up(uint32_t k)
{
    uint32_t q;
    if (rkeys >> k & 1u)
        rec_close(k, clock_at(&q) ? q_round(q) : 0u);
}

static void rec_stop(void)                      /* what is still held ends here */
{
    uint32_t k, q, e = clock_at(&q) ? q_round(q) : 0u;
    if (rec == 2u && e > rec_to)
        e = rec_to;
    for (k = 0; k < 27u; k++)
        if (rkeys >> k & 1u)
            rec_close(k, e);
    rec = 0;
}

static void seq_start(uint32_t cin)             /* from the top, after cin steps of count-in */
{
    seq_run = 1;
    seq_gen++;
    seq_cin = (uint8_t)cin;
}

static void rec_poll(void)                      /* every pass: begin an overdub, end a recording */
{
    uint32_t q;
    if (!rec || !clock_at(&q))
        return;
    if (rec == 1u) {
        rec_from = q >> 8;
        rec_to = rec_from + sq_len();
        rec = 2;
    } else if ((q >> 8) >= rec_to) {
        rec_stop();
        say(sq_n ? "REC DONE" : "NOTHING RECORDED");
    }
}

static uint32_t counting_in(void)               /* REC is waiting for the downbeat */
{
    uint32_t q;
    return rec == 2u && (!clock_at(&q) || (q >> 8) < rec_from);
}

static void pl_sync(void)                       /* hand the ISR what it plays: every pass, as the
                                                 * effects are, because it is cheaper than tracking */
{
    static const uint8_t ARP_DIV[6] = {1, 2, 3, 4, 6, 8};   /* RATE: steps to the beat */
    play_t *d = &plbuf[plcur ^ 1u];
    uint32_t a, i, g = ui[UI_AGATE] + 1u, v = ui[UI_AVEL];
    d->step_q8 = FS * 15u * 256u / bpm();       /* 44100 * 15 * 256 still fits */
    a = FS * 60u * 256u / bpm() / ARP_DIV[ui[UI_ARATE] < 6u ? ui[UI_ARATE] : 3u];   /* 52 s Q8 at most */
    d->a_len[0] = a / 50u * (50u + ui[UI_SWING]);   /* the on-beat step the longer; the pair the same */
    d->a_len[1] = 2u * a - d->a_len[0];
    for (i = 0; i < 2u; i++)
        d->a_gate[i] = d->a_len[i] / 100u * g;
    d->vel = (uint8_t)(v ? (v * 127u + 49u) / 99u : 1u);
    d->run = seq_run;
    d->gen = seq_gen;
    d->cin = seq_cin;
    d->click = ui[UI_CLICK];
    d->arp = arp_on;
    d->pat = ui[UI_APAT];
    d->oct = ui[UI_AOCT];
    d->an = chord_n;
    memcpy(d->chord, chord, chord_n);
    memcpy(d->ord, chord_ord, chord_n);
    d->sq_n = sq_n;
    d->sq_len = (uint8_t)sq_len();
    memcpy(d->sq, sq, sq_n * sizeof sq[0]);
    RING_PUBLISH();
    plcur ^= 1u;
}

/* ---- the step view
 *
 * The phrase as a grid: a row of sixteen sixteenths to each bar, a filled cell where a note starts
 * and a dim one for as long as it is held. The cursor (SELECT) is a white frame; the keys that
 * start on it are the keys lit on the keyboard, and pressing a key there puts that note on it or
 * takes it off. Under a playing loop the step it is on is marked beneath the row. Only the cells
 * that change are drawn, so following the loop costs a cell or two a step. */
#define ST_Y 60u                                /* the first row; one more each 30 down */
static uint8_t cell_drawn[SQ_MAX];              /* what each cell last showed; 0xFF: draw it */

static uint32_t st_x(uint32_t i)                /* 14 a cell, and 2 more between beats */
{
    return 6u + i * 14u + (i >> 2) * 2u;
}

static void draw_steps(void)
{
    uint8_t occ[SQ_MAX];                        /* 2: a note starts here; 1: one is held over it */
    uint32_t i, j, n = sq_len(), q, ph = 0xFFu;
    memset(occ, 0, sizeof occ);
    for (i = 0; i < sq_n; i++) {
        if (sq[i].step >= n)
            continue;                           /* past the loop: kept, but not played */
        for (j = 1; j < sq[i].len && j < n; j++)
            if (!occ[(sq[i].step + j) & (n - 1u)])
                occ[(sq[i].step + j) & (n - 1u)] = 1;
    }
    for (i = 0; i < sq_n; i++)
        if (sq[i].step < n)
            occ[sq[i].step] = 2;
    if (clock_at(&q) && (q >> 8) >= seq_cin)
        ph = ((q >> 8) - seq_cin) & (n - 1u);
    for (i = 0; i < SQ_MAX; i++) {
        uint32_t x = st_x(i & 15u), y = ST_Y + (i >> 4) * 30u;
        uint8_t c = (uint8_t)(i >= n ? 0x80u :
                              occ[i] | (i == stc ? 4u : 0u) | (i == ph ? 8u : 0u));
        if (c == cell_drawn[i])
            continue;
        cell_drawn[i] = c;
        lcd_fill(x - 1u, y - 1u, 14, 21, C_BLACK);   /* its own, and not a pixel of the next */
        if (c & 0x80u)
            continue;
        if (c & 4u)                             /* the cursor: a frame round the cell */
            lcd_fill(x - 1u, y - 1u, 14, 16, RGB(255, 255, 255));
        lcd_fill(x, y, 12, 14, (c & 3u) == 2u ? C_ACC : (c & 3u) == 1u ? RGB(80, 62, 125) :
                               (i & 3u) ? RGB(34, 34, 34) : RGB(62, 62, 62));   /* a beat's first, lighter */
        if (c & 8u)                             /* the loop is here */
            lcd_fill(x, y + 16u, 12, 3, RGB(255, 200, 80));
    }
}

static uint32_t st_find(uint32_t note)          /* the note starting on the cursor, or sq_n */
{
    uint32_t i;
    for (i = 0; i < sq_n; i++)
        if (sq[i].step == stc && sq[i].note == note)
            break;
    return i;
}

static uint32_t st_toggle(uint8_t note)         /* a key on the step view: 1 if it went in */
{
    uint32_t i = st_find(note), n = sq_len(), d = sq_div();
    if (i < sq_n) {                             /* there: out */
        for (; i + 1u < sq_n; i++)
            sq[i] = sq[i + 1u];
        sq_n--;
        pl_dirty = 1;
        return 0;
    }
    if (sq_n >= SQ_MAX) {
        say("SEQ FULL");
        return 0;
    }
    sq[sq_n].step = stc;
    sq[sq_n].note = note;
    sq[sq_n].len = (uint8_t)(d < n ? d : n);
    sq_n++;
    pl_dirty = 1;
    return 1;
}

static void st_len(int32_t d)                   /* LENGTH, of every note on the cursor */
{
    uint32_t i, k = 0, n = sq_len(), first = 0;
    for (i = 0; i < sq_n; i++) {
        int32_t v;
        if (sq[i].step != stc)
            continue;
        v = (int32_t)sq[i].len + d;
        sq[i].len = (uint8_t)(v < 1 ? 1 : v > (int32_t)n ? (int32_t)n : v);
        if (!k++)
            first = sq[i].len;
    }
    if (!k) {
        say("EMPTY STEP");
        return;
    }
    k = put(hk_say, 0, "LENGTH ");
    k = putn(hk_say, k, first, 1);
    hk_say[k] = 0;
    say(hk_say);
    pl_dirty = 1;
}

/* Only ever append to this: settings_load takes a short store and leaves the rest at default,
 * so a new field costs nothing, but moving one would hand the old bytes to the wrong name. */
typedef struct { int8_t oct; uint8_t voice; uint8_t slot; uint8_t mono; uint8_t span; uint8_t rsv;
                 uint8_t genv[G_N]; uint8_t flt[3]; uint8_t fx[4 * FX_N]; uint8_t goff;
                 uint8_t pmark; uint8_t play[PL_N]; uint8_t sq_n; sqn_t sq[SQ_MAX];
                 uint8_t play2[PL2_N]; uint8_t poff; uint8_t mch; uint8_t pch; uint8_t os; uint8_t bits;
                 uint8_t omono;
                 uint8_t voice_hi;              /* the voice is voice + 256 voice_hi: there are 276 */
                 uint8_t srate;                 /* SR_OS less the step, so 0, the default, is 88.2K. `os`
                                                 * is still written, for whether that is 88.2K: a store
                                                 * from before has only that, and its OFF was 44.1K */
                 mi_config_t midi;
               } settings_t;
typedef char fmlab_settings_512[(sizeof(settings_t) <= 512u) ? 1 : -1];
#define FM_LEGACY_SETTINGS_BYTES (offsetof(settings_t,srate)+1u)
#define PL_MARK 0xA5u                           /* the play block is this one: an earlier layout is ignored */

/* The instrument's own bytes have moved: into the settings with them. A new span also drops the
 * held peaks, which belonged to the old axis. */
static void set_take(settings_t *set)
{
    uint32_t k;
    set->midi = mi_config;
    for (k = 0; k < G_N; k++)
        set->genv[k] = ui[UI_GATK + k];
    for (k = 0; k < 3u; k++)
        set->flt[k] = ui[UI_FCUT + k];
    for (k = 0; k < 4u * FX_N; k++)
        set->fx[k] = ui[UI_FX + k];
    set->goff = ui[UI_GOFF];
    set->poff = ui[UI_POFF];
    set->mch = ui[UI_MCH];
    set->pch = ui[UI_PCH];
    set->os = ui[UI_SRATE] == SR_OS;
    set->srate = (uint8_t)(SR_OS - ui[UI_SRATE]);
    set->bits = 3u - ui[UI_BITS];               /* saved the other way up: 0, the default, is 24 */
    set->omono = ui[UI_OUT];
    set->pmark = PL_MARK;
    for (k = 0; k < PL_N; k++)
        set->play[k] = ui[UI_TEMPO + k];
    for (k = 0; k < PL2_N; k++)
        set->play2[k] = ui[UI_AGATE + k];
    set->sq_n = sq_n;
    memcpy(set->sq, sq, sizeof sq);
    if (ui[UI_SPAN] != spspan) {
        spspan = ui[UI_SPAN];
        set->span = spspan;
        for (k = 0; k < 240u; k++)
            sp_pk[k] = 0;
    }
}

static int fm_midi_dispatch(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t kind = st & 0xF0u;
    int action;
    if (st >= 0xF0u || (ui[UI_MCH] && (st & 15u)+1u != ui[UI_MCH])) return MI_CONTROL;
    if (d1 > 127u || (kind != 0xC0u && kind != 0xD0u && d2 > 127u)) {
        say("INVALID MIDI"); mi_invalid(); return MI_CONTROL;
    }
    if (kind == 0x90u && d2) { sp_note = (uint8_t)d1; ev_push(EV_ON,d1,d2); return MI_CONTROL; }
    if (kind == 0x80u || kind == 0x90u) { ev_push(EV_OFF,d1,0); return MI_CONTROL; }
    action = mi_receive(kind,d1,d2);
    if (action == MI_PEDAL) ev_push(EV_SUSTAIN,d2 >= 64u,0);
    else if (action == MI_RESET) ev_push(EV_SUSTAIN,0,0);
    else if (action == MI_SOUND_OFF || action == MI_NOTES_OFF) {
        if (rec) rec_stop();
        seq_run = arp_on = 0; chord_n = 0; arpkeys = rkeys = 0;
        pl_sync(); ev_push(action == MI_SOUND_OFF ? EV_SILENCE : EV_RELEASE_ALL,0,0);
    } else if (action == MI_LEARNED) say("CC LEARNED");
    else if (action == MI_RESERVED) { say("RESERVED CC"); mi_learn = 0; mi_drawn = ~0u; }
    return action;
}

static int fm_settings_write(const settings_t *set)
{
    settings_t verify;
    uint32_t n = mi_default_config(&set->midi) ? FM_LEGACY_SETTINGS_BYTES : sizeof *set;
    int rc;
    memcpy(&verify,set,sizeof verify);
    rc = settings_save(set,n);
    if (!rc) rc = settings_load(&verify,sizeof verify);
    if (!rc && (settings_loaded_length != n || memcmp(set,&verify,sizeof verify))) rc = -1;
    return rc;
}

static int fm_settings_midi_valid(const settings_t *set)
{
    return mi_valid(&set->midi) && (settings_loaded_length <= FM_LEGACY_SETTINGS_BYTES ||
           settings_loaded_length == sizeof *set);
}

static void app_main(void)
{
    settings_t set = {0, 0, 0, 0, 2, 0, {0, 50, 99, 99}, {60, 0, 0}, {FX_DEFAULTS}, 1,
                      PL_MARK, {PLAY_DEFAULTS}, 0, {{0, 0, 0}}, {PLAY2_DEFAULTS}, 0, 0, 1, 1, 0, 0, 0, 0};
    uint32_t prev = 0, k, t_change = 0, dirty = 0, t_meter = 0, repaint = 1, repaint_meter = 1;
    uint32_t sounding = 0;                      /* keys that really started a note: a page change must not strand one */
    uint32_t t_live = 0, eg_dirty = 1, eg_live = 0, t_diagnostics = 0, diagnostics_fresh = 1;
    uint32_t nv_drawn = ~0u, cpu_drawn = ~0u, pk_drawn = ~0u;
    uint64_t vkey_drawn = ~0ull;
    uint16_t voice;                             /* where in the voice list: factory, then U1-01 .. U8-32 */
    int32_t oct_drawn = 99;
    uint8_t held[27] = {0};
    uint16_t shown[4] = {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
    uint8_t page_drawn = 255, bank_drawn = 255, bank_drawn2 = 255, op_drawn = 255, mono_drawn = 255;
    uint8_t gsel_drawn = 255;
    uint8_t foc_drawn = 255;
    uint32_t t_hold[FOC_N] = {0, 0, 0, 0}, long_fired[FOC_N] = {0, 0, 0, 0};  /* how long each is down */
    uint32_t t_rec = 0, rec_long = 0;           /* REC: a tap or a hold */
    uint32_t t_seq = 0, seq_long = 0;           /* SEQ: likewise -- the knobs, or the step view */
    uint32_t t_midi = 0, midi_long = 0, midi_rejected = 0;
    uint32_t t_aud = 0;                         /* the step view: when the cursor's notes began */
    uint8_t aud[8], aud_n = 0, stc_was = 0;
    uint8_t latch_was;
    uint8_t alg_drawn = 255, alg_op_drawn = 255, mute_drawn = 255, leg_drawn = 255;
    uint32_t lfo_sig_drawn = ~0u;               /* the LFO picture on HOME follows its own bytes */

    bank_init();                                /* before the first load_voice: a slot may be the voice */
    mi_init(); set.midi = mi_config;
    settings_load(&set, sizeof set);
    if (fm_settings_midi_valid(&set)) { mi_config = set.midi; mi_publish(); }
    else { midi_rejected = 1; set.midi = mi_config; }
    voice = (uint16_t)(set.voice + 256u * set.voice_hi);
    if (set.oct < -2 || set.oct > 2) set.oct = 0;
    if (voice >= NVOICE) voice = 0;
    if (set.span >= SP_NSPAN) set.span = 2;
    spspan = set.span;
    ui[UI_SPAN] = spspan;
    for (k = 0; k < G_N; k++)
        ui[UI_GATK + k] = set.genv[k] > 99u ? 99u : set.genv[k];
    ui[UI_FCUT] = set.flt[0] > 99u ? 99u : set.flt[0];
    ui[UI_FRES] = set.flt[1] > 99u ? 99u : set.flt[1];
    ui[UI_FTYPE] = set.flt[2] > 3u ? 0u : set.flt[2];
    for (k = 0; k < 4u * FX_N; k++)
        ui[UI_FX + k] = set.fx[k] > 99u ? 99u : set.fx[k];
    ui[UI_GOFF] = set.goff ? 1u : 0u;
    ui[UI_POFF] = set.poff ? 1u : 0u;
    ui[UI_MCH] = set.mch > 16u ? 0u : set.mch;
    ui[UI_PCH] = set.pch ? 1u : 0u;
    ui[UI_SRATE] = set.srate > SR_OS ? SR_OS : SR_OS - set.srate;
    if (ui[UI_SRATE] == SR_OS && !set.os)      /* OVERSAMPLE OFF, from before there were steps */
        ui[UI_SRATE] = SR_OS - 1u;
    os_want = ui[UI_SRATE] == SR_OS;
    sr_hz = SR_HZ[ui[UI_SRATE]];
    ui[UI_BITS] = set.bits > 3u ? 3u : 3u - set.bits;
    crush_sh = CRUSH_SH[ui[UI_BITS]];
    ui[UI_OUT] = set.omono ? 1u : 0u;
    out_mono = ui[UI_OUT];
    if (set.pmark == PL_MARK) {
        static const uint8_t PMAX[PL_N] = {99, 4, 3, 1, 2, 2, 1};
        static const uint8_t PMAX2[PL2_N] = {99, 5, 25, 99};
        for (k = 0; k < PL_N; k++)
            ui[UI_TEMPO + k] = set.play[k] > PMAX[k] ? PMAX[k] : set.play[k];
        for (k = 0; k < PL2_N; k++)
            ui[UI_AGATE + k] = set.play2[k] > PMAX2[k] ? PMAX2[k] : set.play2[k];
        for (k = 0; k < set.sq_n && k < SQ_MAX; k++)   /* one bad note and it is not a phrase we wrote */
            if (set.sq[k].step >= SQ_MAX || !set.sq[k].len || set.sq[k].len > SQ_MAX ||
                set.sq[k].note > 127u)
                break;
        if (set.sq_n <= SQ_MAX && k == set.sq_n) {
            sq_n = set.sq_n;
            memcpy(sq, set.sq, sizeof sq);
        }
    }
    latch_was = ui[UI_LATCH];
    glo_sync();
    flt_sync();
    fx_sync();
    sp_init();
    set.mono = set.mono ? 1u : 0u;
    ev_push(EV_MONO, set.mono, 0);              /* the ISR owns the mode: tell it what was stored */
    save_slot = set.slot;
    load_voice(voice);
    ab_copy(voice);                         /* both sides start as the same sound */
    memcpy(pbuf[pcur ^ 1u], edit, DX_VSZ);         /* both halves valid before the ISR ever reads one */

    lcd_fill(0, 0, 240, 240, C_BLACK);
    if (midi_rejected) say("MIDI DATA REJECTED");

    for (;;) {
        uint32_t keys, down, up, rel, b, lit = 0, nv = 0, e;
        uint32_t base = (uint32_t)(KEY0 + 12 * set.oct);
        int32_t enc;
        uint8_t st, d1, d2;
        sx_bank = (uint8_t)(voice >= DX_NVOICES ? (voice - DX_NVOICES) / BANK_V : set.slot / BANK_V);
        platform_poll();                    /* (a bank arriving in there goes into sx_bank) */
        glo_sync();                         /* whatever moved the four bytes, the ISR has them */
        flt_sync();                         /* ..and the filter's three, if they moved */
        fx_sync();                          /* ..and the effects' sixteen */
        pl_sync();                          /* ..and what the clock plays */
        os_want = ui[UI_SRATE] == SR_OS;    /* ..and the rate it renders at */
        sr_hz = SR_HZ[ui[UI_SRATE] < SR_N ? ui[UI_SRATE] : SR_OS];
        crush_sh = CRUSH_SH[ui[UI_BITS] & 3u];   /* ..and at how many bits */
        out_mono = ui[UI_OUT];              /* ..and to one channel or two */

        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {             /* a key stops the note it started: the octave may change */
            if (down >> k & 1u) {
                int fn = mi_editor ? -1 : op_key(k);
                if (fn >= 0) {
                    opsel = (uint8_t)fn;        /* pointing at an operator, not playing one */
                    gsel = 0;
                } else if (!mi_editor && glo_point() && k == GLO_KEY) {
                    gsel = 1;                   /* ..or at the envelope over all six of them */
                    bank = 0;
                    repaint = 1;
                } else if (!mi_editor && op_point() && k == PIT_KEY) {
                    gsel = 2;                   /* ..or at the one that bends the note's pitch */
                    if (page == PAGE_HOME)
                        focus = FOC_ENV;
                    else if (page != PAGE_EG) {
                        focus = FOC_OFF;
                        page_to(PAGE_EG);
                    }
                    if (page == PAGE_EG)
                        bank = 0;
                    repaint = 1;
                } else if (!mi_editor && k == AB_KEY && ab_point()) {
                    ab_t = fm1_ms ? fm1_ms : 1u;    /* the compare: decided on the way up */
                    ab_long = 0;
                } else if (!mi_editor && op_point() && (k == MONO_KEY || k == POLY_KEY)) {
                    set.mono = k == MONO_KEY;
                    ev_push(EV_MONO, set.mono, 0);
                    say(set.mono ? "MONO" : "POLY");
                    t_change = fm1_ms;
                    dirty = 1;
                } else if (page == PAGE_STEP) {   /* onto the cursor's step, or off it */
                    held[k] = (uint8_t)(base + k);
                    if (st_toggle(held[k])) {   /* heard going in, not coming out */
                        sp_note = held[k];
                        sounding |= 1u << k;
                        ev_push(EV_ON, held[k], 100);
                    }
                } else {
                    held[k] = (uint8_t)(base + k);
                    sp_note = held[k];          /* the analyser marks the harmonics of the last note */
                    if (rec)
                        rec_down(k, held[k]);   /* into the phrase, and heard as it goes in */
                    if (arp_on) {               /* the arpeggiator's, not a note of its own */
                        if (ui[UI_LATCH] && !arpkeys)
                            chord_n = 0;        /* a fresh chord replaces the latched one */
                        arpkeys |= 1u << k;
                        chord_add(held[k]);
                    } else {
                        sounding |= 1u << k;
                        ev_push(EV_ON, held[k], 100);
                        midi_send(0x90u | MIDI_TX, held[k], 100);
                    }
                }
            }
            if ((up >> k & 1u) && k == AB_KEY && ab_t) {
                if (!ab_long) {                 /* a tap: the other side */
                    ab_swap(&voice);
                    dirty = 1;
                    t_change = fm1_ms;
                }
                ab_t = 0;
            }
            if (up >> k & 1u)
                rec_up(k);
            if ((up >> k & 1u) && (arpkeys >> k & 1u)) {
                arpkeys &= ~(1u << k);
                if (!ui[UI_LATCH])
                    chord_del(held[k]);
            }
            if ((up >> k & 1u) && (sounding >> k & 1u)) {
                sounding &= ~(1u << k);
                ev_push(EV_OFF, held[k], 0);
                midi_send(0x80u | MIDI_TX, held[k], 0);
            }
        }

        if (ab_t && !ab_long && (keys >> AB_KEY & 1u) && fm1_ms - ab_t >= LONG_MS) {
            ab_long = 1;                        /* a hold: this side onto the other */
            ab_copy(voice);
            say(ab ? "B COPIED TO A" : "A COPIED TO B");
        }

        b = fm1_input_edges(&rel);
        if (mi_editor && (b & ((1u << BTN_HOME) | (1u << BTN_EDIT) | (1u << BTN_ENV) |
            (1u << BTN_LFO) | (1u << BTN_FX) | (1u << BTN_ARP) | (1u << BTN_SEQ)))) {
            mi_editor = mi_learn = 0; page_drawn = 255;
        }
        if (b && !(b & (1u << BTN_SAVE)) && b != 1u << BTN_SEL)
            saving = 0;                                    /* anything else backs out of the save --
                                                            * but SEL, the shift for PRESETS' banks */
        if (b & (1u << BTN_OCT_DOWN) && set.oct > -2) { set.oct--; dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && set.oct < 2) { set.oct++; dirty = 1; t_change = fm1_ms; }
        /* A page button moves to its page; pressing it again walks that page's banks of parameters,
         * so the button that got you there is also the one that shows you the rest. SELECT does
         * it too, either way round -- it is the shorter reach when you are already turning knobs. */
        if (b & (1u << BTN_HOME)) {                        /* and the way back out of focus */
            if (focus != FOC_OFF) {
                focus = FOC_OFF;                           /* the keys and the knobs, as they were */
                if (page != PAGE_HOME)
                    page_to(PAGE_HOME);
            } else
                page_to(PAGE_HOME);
        }
        if (b & (1u << BTN_GLO)) {
            if (mi_editor) mi_slot_turn(1);
            else { page_to(PAGE_GLO); t_midi = fm1_ms ? fm1_ms : 1u; midi_long = 0; }
        }
        if (t_midi && !midi_long && fm1_ms-t_midi >= LONG_MS &&
            ((fm1_in.buttons | rel) & (1u << BTN_GLO))) {
            midi_long = 1; mi_open_editor(); saving = 0;
        }
        if (rel & (1u << BTN_GLO)) t_midi = 0;
        /* EDIT, ENV and LFO are the three buttons with a page whose top is worth having under the
         * knobs while you play, and they are the three that answer to how long you hold them. Held past LONG_MS: the page, and pressing again walks its
         * banks, exactly as every page button does -- a second hold is not needed, because once you
         * are on the page the button owns, a tap of it can only sensibly mean the next bank. Asking
         * for focus from there would be asking to leave, which is what HOME is for. Released
         * anywhere else: focus, on HOME, and pressing again steps the operator. The short action
         * waits for the release because the long one replaces it -- there is no way to know which
         * you meant until you let go or run out of time, and firing both would be worse than
         * firing neither. */
        for (k = 0; k < FOC_N; k++) {
            uint32_t m = 1u << FOC_BTN[k];
            if (b & m) {
                t_hold[k] = fm1_ms ? fm1_ms : 1u;
                long_fired[k] = 0;
            }
            if (t_hold[k] && !long_fired[k] && (fm1_in.buttons & m) &&
                fm1_ms - t_hold[k] >= LONG_MS) {
                long_fired[k] = 1;
                focus = FOC_OFF;
                page_to(FOC_PAGE[k]);
                if (page == PAGE_FX)
                    bank = fxsel;               /* the effect you were on */
            }
            if (rel & m) {
                if (!long_fired[k]) {
                    if (FOC_MODE[k] == FOC_ENV && (eg_glo() || eg_pit())) {   /* the global or the
                                                 * pitch envelope: in or out; SELECT has the pages */
                        if (eg_pit()) {
                            ui[UI_POFF] ^= 1u;
                            say(ui[UI_POFF] ? "PITCH ENV OFF" : "PITCH ENV ON");
                        } else {
                            ui[UI_GOFF] ^= 1u;
                            say(ui[UI_GOFF] ? "GLOBAL ENV OFF" : "GLOBAL ENV ON");
                        }
                        set_take(&set);
                        dirty = 1;
                        t_change = fm1_ms;
                        eg_dirty = 1;
                        alg_drawn = 255;                       /* HOME draws it from the diagram's branch */
                        if (page == PAGE_EG)
                            draw_eg_now(bank);                 /* now, not on the next tick */
                    }
                    else if (page == FOC_PAGE[k])
                        page_to(FOC_PAGE[k]);                  /* already there: the next bank */
                    else if (focus == FOC_MODE[k] && page == PAGE_HOME) {
                        if (FOC_MODE[k] == FOC_LFO && lfo_glo()) {  /* no operator to step: the wave */
                            edit[142] = (uint8_t)((edit[142] + 1u) % 6u);
                            say_param(&PG_LFO[PG_LFO_WAVE]);
                            modified = 1;
                            patch_publish();
                        } else {
                            foc_step(1);                       /* again: the next one */
                        }
                    }
                    else {
                        focus = FOC_MODE[k];
                        if (page != PAGE_HOME)
                            page_to(PAGE_HOME);
                    }
                }
                t_hold[k] = 0;
            }
        }
        /* SEL switches the selected operator: a tap off and on, a hold (LONG_MS) solo. Both wait
         * for the release, because while it is down it is also the fine-adjust shift, and a knob
         * turned under it has spoken for the press -- so a hold cannot fire on the clock the way
         * the page buttons' do, or every fine adjustment would end in a solo. */
        if (b & (1u << BTN_SEL) && mi_editor) {
            mi_toggle_learn(); sel_used = 1;
        } else if (b & (1u << BTN_SEL)) {
            sel_used = saving;                             /* while saving it is only the shift */
            t_sel = fm1_ms;
            draw_legend();                                 /* the names go bright: a click is one */
        }
        if ((rel & (1u << BTN_SEL)) && !mi_editor) {
            draw_legend();
            if (!sel_used && glo_diagnostics()) {
                cpu_request_reset(); diagnostics_fresh = repaint_meter = 1;
                say("PEAK / LATE RESET");
            } else if (!sel_used && (page != PAGE_HOME || foc_op())) {   /* plain HOME has no operator to pick */
                if (fm1_ms - t_sel >= LONG_MS)
                    op_solo(opsel);                        /* only the selected operator */
                else
                    op_toggle(opsel);                      /* the selected operator, off and on */
            }
        }
        /* ARP: the first tap puts its knobs on HOME, the way ENV and the rest do; a tap there turns
         * it on and off. SEQ puts the sequencer's knobs there, and has nothing to switch. */
        if (b & (1u << BTN_ARP)) {
            if (focus == FOC_ARP && page == PAGE_HOME) {
                arp_on ^= 1u;
                chord_n = 0;
                arpkeys = 0;
                say(arp_on ? (ui[UI_LATCH] ? "ARP ON, LATCHED" : "ARP ON") : "ARP OFF");
            } else {
                focus = FOC_ARP;
                if (page != PAGE_HOME)
                    page_to(PAGE_HOME);
            }
        }
        if (ui[UI_LATCH] != latch_was) {                   /* LATCH turned: the chord is the keys still down */
            latch_was = ui[UI_LATCH];
            chord_n = 0;
            for (k = 0; k < 27u; k++)
                if (arpkeys >> k & 1u)
                    chord_add(held[k]);
        }
        /* SEQ: a tap puts its knobs on HOME; a hold opens the step view */
        if (b & (1u << BTN_SEQ)) {
            t_seq = fm1_ms ? fm1_ms : 1u;
            seq_long = 0;
        }
        if (t_seq && !seq_long && (fm1_in.buttons & (1u << BTN_SEQ)) && fm1_ms - t_seq >= LONG_MS) {
            seq_long = 1;
            focus = FOC_SEQ;
            if (stc >= sq_len())
                stc = 0;
            stc_was = stc;                      /* arriving is not moving: no audition */
            page_to(PAGE_STEP);
        }
        if (rel & (1u << BTN_SEQ)) {
            if (t_seq && !seq_long) {
                focus = FOC_SEQ;
                if (page != PAGE_HOME)
                    page_to(PAGE_HOME);
            }
            t_seq = 0;
        }
        /* REC: a tap records, or stops recording; a hold clears -- decided on the clock, so the
         * hold does not have to wait for the release to be sure. */
        if (b & (1u << BTN_REC)) {
            t_rec = fm1_ms ? fm1_ms : 1u;
            rec_long = 0;
        }
        if (t_rec && !rec_long && (fm1_in.buttons & (1u << BTN_REC)) && fm1_ms - t_rec >= LONG_MS) {
            rec_long = 1;
            rkeys = 0;
            rec = 0;
            seq_run = 0;
            sq_n = 0;
            pl_dirty = 1;
            say("SEQ CLEARED");
        }
        if (rel & (1u << BTN_REC)) {
            if (t_rec && !rec_long) {
                if (rec) {
                    rec_stop();
                    say("REC DONE");
                } else if (seq_run) {
                    rkeys = 0;
                    rec = 1;                               /* over what is playing, from where it is */
                    say("REC: OVERDUB");
                } else {
                    seq_start(16u);                        /* a bar of clicks, then the downbeat */
                    rkeys = 0;
                    rec = 2;
                    rec_from = 16u;
                    rec_to = 16u + sq_len();
                    say("REC: COUNT-IN");
                }
            }
            t_rec = 0;
        }
        if (b & (1u << BTN_PLAY)) {
            if (seq_run) {
                if (rec)
                    rec_stop();
                seq_run = 0;
                say("STOP");
            } else if (!sq_n) {
                say("NOTHING RECORDED");
            } else {
                seq_start(0u);
                say("PLAY");
            }
        }
        rec_poll();
        if (pl_dirty) {
            pl_dirty = 0;
            set_take(&set);
            dirty = 1;
            t_change = fm1_ms;
        }
        if (b & (1u << BTN_SAVE)) {                        /* press to pick a slot, press again to write */
            if (mi_editor) {
                mi_clear();
                set_take(&set); dirty = 1; t_change = fm1_ms;
            } else if (!saving) {
                saving = 1;
                save_slot = set.slot;
                name_begin();
            } else {
                saving = 0;
                memcpy(edit + 145, save_name, 10);     /* the name goes in with the sound */
                if (bank_store(save_slot, edit) == 0) {
                    voice = (uint16_t)(DX_NVOICES + save_slot);
                    set.slot = save_slot;
                    modified = 0;                          /* the slot now holds exactly this */
                    say("SAVED");
                    dirty = 1;
                    t_change = fm1_ms;
                } else
                    say("SAVE FAILED");
            }
        }

        if ((enc = fm1_enc_take(ENC_PRESETS)) != 0) {      /* the voice, or the slot to save to */
            if (mi_editor) {
                if (mi_group_turn(enc)) { set_take(&set); dirty = 1; t_change = fm1_ms; }
            } else {
            if (sel_fine())                                /* SEL held: a bank at a time */
                sel_used = 1;
            if (saving) {
                int32_t s2 = sel_fine() ? (int32_t)bank_jump(save_slot, enc, 0) : (int32_t)save_slot + enc;
                while (s2 < 0) s2 += (int32_t)NUSER;
                save_slot = (uint8_t)(s2 % (int32_t)NUSER);
            } else {
                int32_t vi = sel_fine() ? (int32_t)bank_jump(voice, enc, 1) : (int32_t)voice + enc;
                while (vi < 0) vi += NVOICE;
                voice = (uint16_t)(vi % NVOICE);
                load_voice(voice);
                if (ui[UI_PCH])                 /* so whatever is listening can follow */
                    pc_send(voice);
                dirty = 1;
                t_change = fm1_ms;
            }
            }
        }
        if ((enc = fm1_enc_take(ENC_SELECT)) != 0) {       /* the next four things under the knobs */
            if (mi_editor) {
                mi_slot_turn(enc);
            } else if (saving) {                                  /* or, while saving, which letter */
                int32_t c2 = (int32_t)save_cur + enc;
                while (c2 < 0) c2 += 10;
                save_cur = (uint8_t)(c2 % 10);
            } else
                sel_step(enc);
        }
        if ((enc = fm1_enc_take(ENC_ALGORITHM)) != 0) {    /* its own knob, on any page */
            if (mi_editor) {
                if (mi_target_turn(enc)) { set_take(&set); dirty = 1; t_change = fm1_ms; }
            }
            else if (saving) {                                  /* or, while saving, what that letter is */
                int32_t c2 = (int32_t)name_ix(save_name[save_cur]) + enc;
                while (c2 < 0) c2 += (int32_t)NAME_NCH;
                save_name[save_cur] = NAME_CH[c2 % (int32_t)NAME_NCH];
            } else {
                int32_t a = (int32_t)edit[134] + enc;
                edit[134] = (uint8_t)(a < 0 ? 0 : a > 31 ? 31 : a);
                modified = 1;
                patch_publish();
            }
        }
        if (mi_editor)
            for (e = 0; e < 4u; e++)
                if (mi_knob(e,fm1_enc_take(ENC_KNOB1+e))) { set_take(&set); dirty = 1; t_change = fm1_ms; }
        if (page == PAGE_FX)
            fxsel = bank;                       /* the page and the tap agree on which effect */
        if (page == PAGE_HOME && (focus != FOC_OFF || home_flt())) {   /* focused: four things about
                                                                        * one operator, or the filter */
            for (e = 0; e < 4u; e++) {
                const param_t *p = focus_at(e);
                if ((enc = knob_take(e, p_step(p))) == 0 || !p)
                    continue;
                p_turn(p, enc);
                say_param(p);                   /* HOME has no rows, so the voice line says it */
                if (p->where == P_UI) {         /* the instrument's bytes are not the voice: turning
                                                 * one must not call the sound edited */
                    set_take(&set);
                    dirty = 1;
                    eg_dirty = 1;               /* HOME is showing the shape: it just changed */
                    t_change = fm1_ms;
                    continue;
                }
                eg_dirty = 1;                   /* the pitch envelope's picture is these bytes */
                modified = 1;
                patch_publish();
            }
        } else if (page == PAGE_HOME) {         /* the four knobs are four operators, either screen */
            for (e = 0; e < 4u; e++) {
                uint32_t n = home_op(e), op;
                if (home_fb(e)) {               /* 0..7: every click is worth hearing */
                    int32_t v;
                    if ((enc = knob_take(e, 1)) == 0)
                        continue;
                    mi_panel_target(31);
                    v = (int32_t)edit[135] + enc;
                    edit[135] = (uint8_t)(v < 0 ? 0 : v > 7 ? 7 : v);
                    say_param(&PG_GLO[PG_GLO_FB]);
                    modified = 1;
                    patch_publish();
                    continue;
                }
                if ((enc = knob_take(e, HSET[homeset].what == HK_LVL ? COARSE : 1)) == 0 || !n)
                    continue;
                op = 6u - n;                    /* the panel counts OP1 first, the voice OP6 */
                if (HSET[homeset].what == HK_LVL) {
                    mi_panel_target(13u+5u-op);
                    int32_t v = (int32_t)*op_byte(op, 16) + enc;
                    *op_byte(op, 16) = (uint8_t)(v < 0 ? 0 : v > 99 ? 99 : v);
                } else
                    rat_turn(op, enc);
                say_knob(n);
                modified = 1;
                patch_publish();
            }
        }
        if (page == PAGE_STEP) {                /* LENGTH of the cursor's notes; TEMPO, BARS, DIV */
            for (e = 0; e < 4u; e++) {
                const param_t *p = e ? &PG_SEQ[e - 1u] : 0;
                int32_t v;
                if ((enc = knob_take(e, p ? p_step(p) : 1)) == 0)
                    continue;
                if (!p) {
                    st_len(enc);
                    continue;
                }
                v = (int32_t)ui[p->idx] + enc;
                ui[p->idx] = (uint8_t)(v < 0 ? 0 : v > (int32_t)p->max ? (int32_t)p->max : v);
                say_param(p);
                set_take(&set);
                dirty = 1;
                t_change = fm1_ms;
            }
            if (stc >= sq_len())                /* fewer bars: the cursor comes back in */
                stc = 0;
            if (stc != stc_was || (aud_n && fm1_ms - t_aud > 250u)) {   /* the audition ends */
                for (k = 0; k < aud_n; k++)
                    ev_push(EV_OFF, aud[k], 0);
                aud_n = 0;
            }
            if (stc != stc_was) {               /* moved, stopped: hear what is on it */
                stc_was = stc;
                if (!seq_run) {
                    for (k = 0; k < sq_n && aud_n < 8u; k++)
                        if (sq[k].step == stc) {
                            aud[aud_n++] = sq[k].note;
                            ev_push(EV_ON, sq[k].note, 100);
                        }
                    t_aud = fm1_ms;
                }
            }
        } else if (aud_n) {                     /* left with one sounding */
            for (k = 0; k < aud_n; k++)
                ev_push(EV_OFF, aud[k], 0);
            aud_n = 0;
        }
        if (glo_diagnostics())
            for (e = 0; e < 4u; e++) {
                if (fm1_enc_take(ENC_KNOB1+e) && sel_fine()) sel_used = 1;
            }
        for (e = 0; e < bank_rows(bank); e++) {            /* KNOB1..n: the parameters on screen */
            uint32_t pi = bank_first(bank) + e;
            const param_t *p;
            int32_t v;
            if ((p = page_at(pi)) == 0 || (enc = knob_take(e, p_step(p))) == 0)
                continue;
            fm_midi_touch(p);
            if (p->kind == K_RAT) {
                rat_turn(opsel, enc);
                modified = 1;
                patch_publish();
                continue;
            }
            if (p->where == P_UI) {                        /* a switch of the instrument, not the sound */
                v = (int32_t)ui[p->idx] + enc;
                ui[p->idx] = (uint8_t)(v < 0 ? 0 : v > (int32_t)p->max ? (int32_t)p->max : v);
                set_take(&set);
                if (bank >= page_nbank())                  /* fewer parameters than there were banks */
                    bank = (uint8_t)(page_nbank() - 1u);
                dirty = 1;
                t_change = fm1_ms;
                repaint = 1;
                continue;
            }
            v = (int32_t)*p_ref(p) + (p->kind == K_TIME || p->kind == K_DEC ? -enc : enc);
            v = v < 0 ? 0 : v > (int32_t)p->max ? (int32_t)p->max : v;
            *p_ref(p) = (uint8_t)v;
            if (p->kind == K_DEC)               /* the second stage of the decay goes with the first */
                edit[p_at(p) + 1u] = (uint8_t)v;
            if (p->kind == K_SUS)               /* and the level it was heading for, so the two are flat */
                edit[p_at(p) - 1u] = (uint8_t)v;
            modified = 1;
            patch_publish();
        }

        while (midi_get(&st, &d1, &d2)) {       /* MIDI in, USB and the jack: any channel, or the one picked */
            int action = fm_midi_dispatch(st,d1,d2);
            if (action == MI_LEARNED) { set_take(&set); dirty = 1; t_change = fm1_ms; }
            if (action != MI_IGNORED) continue;
            if ((st & 0xF0u) == 0xB0u && d1 == 0u) pc_bank = d2;   /* bank select, for the next PC */
            else if ((st & 0xF0u) == 0xC0u && ui[UI_PCH] && !saving &&
                     (pc_bank >= 1u && pc_bank <= NBANK ? d1 < BANK_V : 1)) {   /* 0..127 are all in the list */
                /* bank 0 (or none): program N is the voice list's N+1; bank 1..8: slot N+1 of U1..U8 */
                voice = (uint16_t)(pc_bank >= 1u && pc_bank <= NBANK ?
                                      DX_NVOICES + (pc_bank - 1u) * BANK_V + d1 : d1);
                load_voice(voice);
                dirty = 1;
                t_change = fm1_ms;
                repaint = 1;
            }
        }

        base = (uint32_t)(KEY0 + 12 * set.oct);
        for (k = 0; k < NV; k++)                /* what sounds, on the keys of this octave */
            if (v_lit >> k & 1u) {
                nv++;
                if (page != PAGE_STEP && vst[k].note >= base && vst[k].note < base + 27u)
                    lit |= 1u << (vst[k].note - base);
            }
        if (page == PAGE_STEP)                  /* ..or, on the step view, what is on the cursor */
            for (k = 0; k < sq_n; k++)
                if (sq[k].step == stc && sq[k].note >= base && sq[k].note < base + 27u)
                    lit |= 1u << (sq[k].note - base);
        for (k = 0; k < 27u; k++)
            fm1_led_key(KEY_LED(k), lit >> k & 1u);
        {                                       /* the OP1..OP6 keys: all six glow where they work,
                                                 * and the one you are editing is lit outright */
            int opk = (int)op_point();
            for (k = 0; k < 6u; k++)
                fm1_led_key_glow(KEY_LED(OP_KEY[k]), opk);
            if (opk && !eg_glo() && !eg_pit() && !lfo_glo())
                fm1_led_key(KEY_LED(OP_KEY[5u - opsel]), 1);
            fm1_led_key_glow(KEY_LED(PIT_KEY), opk);
            if (eg_pit())
                fm1_led_key(KEY_LED(PIT_KEY), 1);
            fm1_led_key_glow(KEY_LED(GLO_KEY), (int)glo_point());
            if (eg_glo() || lfo_glo())
                fm1_led_key(KEY_LED(GLO_KEY), 1);
            fm1_led_key_glow(KEY_LED(MONO_KEY), opk);
            fm1_led_key_glow(KEY_LED(POLY_KEY), opk);
            if (opk)                            /* and the mode you are in is the one that is lit */
                fm1_led_key(KEY_LED(set.mono ? MONO_KEY : POLY_KEY), 1);
            fm1_led_key_glow(KEY_LED(AB_KEY), (int)ab_point());   /* the compare; lit on side B */
            fm1_led_key(KEY_LED(AB_KEY), ab_point() && ab != 0u);
        }
        /* The octave buttons: lit for a shift that way, and pulsing at the end of the travel. The
         * pulse rides the glow layer, so the LED dips rather than going dark -- a limit, not an
         * alarm. ~1.1 Hz, lit for two thirds of it. */
        {
            uint32_t ph = (fm1_ms % 900u) < 600u;
            int lo = set.oct < 0, hi = set.oct > 0;
            fm1_led_key(BTN_OCT_DOWN, lo && (set.oct > -2 || ph));
            fm1_led_key(BTN_OCT_UP, hi && (set.oct < 2 || ph));
            fm1_led_key_glow(BTN_OCT_DOWN, set.oct == -2);
            fm1_led_key_glow(BTN_OCT_UP, set.oct == 2);
        }
        fm1_led_key(BTN_HOME, page == PAGE_HOME);
        fm1_led_key(BTN_EDIT, page == PAGE_OP || focus == FOC_EDIT);
        fm1_led_key(BTN_ENV, page == PAGE_EG || focus == FOC_ENV);
        fm1_led_key(BTN_LFO, page == PAGE_LFO || focus == FOC_LFO);
        fm1_led_key(BTN_GLO, page == PAGE_GLO);
        fm1_led_key(BTN_FX, page == PAGE_FX || focus == FOC_FX);
        fm1_led_key(BTN_SEL, mi_editor ? mi_learn : opmute != 0u);
        fm1_led_key(BTN_ARP, arp_on);
        fm1_led_key_glow(BTN_ARP, ui[UI_LATCH]);           /* latched, whether or not it is running */
        fm1_led_key(BTN_SEQ, (focus == FOC_SEQ && page == PAGE_HOME) || page == PAGE_STEP);
        {   /* PLAY is the beat: a flash on each one, count-in and all */
            uint32_t q, cin = counting_in();
            fm1_led_key(BTN_PLAY, clock_at(&q) && (q & 0x3FFu) < 0x100u);
            fm1_led_key_glow(BTN_PLAY, sq_n != 0u);        /* there is a phrase to play */
            fm1_led_key(BTN_REC, rec && !(cin && (fm1_ms % 300u) >= 150u));   /* flickering: counting in */
        }
        fm1_led_key(BTN_SAVE, saving);                     /* lit while it is asking which slot */

        if (flash_msg && fm1_ms - flash_at > 1500u)
            flash_msg = 0;

        if (mi_editor) mi_draw();
        else {
        if (page != page_drawn || bank != bank_drawn2) {   /* a new screen: clear once, then only
                                                            * what changes */
            page_drawn = page;
            bank_drawn2 = bank;
            bank_drawn = op_drawn = 255;
            alg_drawn = alg_op_drawn = 255;
            eg_dirty = 1;
            diagnostics_fresh = 1;
            repaint_meter = 1;
            for (k = 0; k < 240u; k++)
                sp_pk[k] = 0;
            lcd_fill(0, 0, 240, 240, C_BLACK);  /* all of it: the meter is only on one page now */
            corner_sig = 0xFFFFFFFFu;           /* the clear took the corners with it */
            memset(cell_drawn, 0xFF, sizeof cell_drawn);
            foc_drawn = 255;
            repaint = 1;
        }
        if (page == PAGE_HOME) {                /* the banner, or what the knobs have become */
            uint8_t ft = (uint8_t)(focus == FOC_OFF ? (home_flt() ? 60u : bank == 2u ? 61u : 0u) :
                                   focus == FOC_FX ? 64u :
                                   focus == FOC_SEQ ? 65u :
                                   focus == FOC_ARP ? 66u + arp_pg :
                                   gsel == 2u && focus == FOC_ENV ? 68u + pit_pg :
                                   gsel && focus == FOC_ENV ? 63u :
                                   gsel == 1u && focus == FOC_LFO ? 62u :
                                   focus * 8u + opsel);
            if (ft != foc_drawn || repaint) {
                foc_drawn = ft;
                draw_home_title();
                repaint = 1;                    /* the banner's box reaches the voice line under it */
            }
        }
        {   /* the legend outlives a page, but not a change of mode: RATIO becomes FIXED HZ */
            uint8_t leg = (uint8_t)(eg_glo() ? 40u :
                                    eg_pit() ? 42u + (page == PAGE_HOME ? pit_pg : bank) :
                                    lfo_glo() ? 41u :
                                    home_flt() ? 45u :
                                    focus == FOC_FX && page == PAGE_HOME ? 50u :
                                    focus == FOC_SEQ && page == PAGE_HOME ? 51u :
                                    focus == FOC_ARP && page == PAGE_HOME ? 52u + arp_pg :
                                    focus != FOC_OFF && page == PAGE_HOME ?
                                        20u + focus * 2u +
                                            (*op_byte(opsel, 17) ? 1u : 0u) :
                                    page == PAGE_OP ? (*op_byte(opsel, 17) ? 2u : 1u) :
                                    page == PAGE_HOME ? 10u + homeset : 0u);
            if (leg != leg_drawn || repaint) {
                leg_drawn = leg;
                draw_legend();
            }
        }
        draw_corners();                         /* cheap unless the picture up there changed */
        if (page != PAGE_HOME && (bank != bank_drawn || opsel != op_drawn || opmute != mute_drawn
                                  || gsel != gsel_drawn || repaint)) {
            bank_drawn = bank;
            op_drawn = opsel;
            gsel_drawn = gsel;
            draw_head();
            shown[0] = shown[1] = shown[2] = shown[3] = 0xFFFFu;
            eg_dirty = 1;
        }
        {                                       /* the voice line: the name, or what it is asking / saying */
            uint64_t vkey = (uint64_t)voice | (uint64_t)modified << 9 | (uint64_t)ab << 10 |
                            (uint64_t)saving << 11 | (uint64_t)save_slot << 12 |
                            (flash_msg ? 1ull << 20 : 0u) | (uint64_t)(msg_gen & 7u) << 21 |
                            (uint64_t)save_cur << 24 | (uint64_t)name_sum() << 28;
            if (vkey != vkey_drawn || repaint) {
                vkey_drawn = vkey;
                draw_voice(voice, page == PAGE_HOME ? 44u : 188u);
                if (page != PAGE_HOME) { bank_drawn = 255; }   /* the header may name a changed algorithm */
            }
        }
        repaint = 0;

        if (page == PAGE_HOME) {
            /* the whole diagram only when the shape of it changes: the live brightness is six
             * small blits on the tick below, which is a great deal cheaper than 240 x 136.
             * The ring and the curve belong to an operator, and GLOBAL is not one -- so GLOBAL
             * gets the diagram's room instead, and the same picture the ENVELOPE page draws.
             * Adjusting a shape you cannot see is no way to learn what the shape does. */
            uint8_t asel = (uint8_t)(opsel | (eg_glo() ? 0x80u : 0u) | (lfo_glo() ? 0x40u : 0u) |
                                     (foc_op() ? 0x20u : 0u) |   /* the ring: only with an operator to pick */
                                     (eg_pit() ? 0x10u | pit_pg << 3 : 0u));
            /* the LFO's picture follows bytes the diagram never cared about, so it brings its own */
            uint32_t lsig = lfo_glo() ? (1u | (uint32_t)edit[137] << 1 | (uint32_t)edit[138] << 9
                                            | (uint32_t)edit[142] << 17) : 0u;
            if (bank == 0u && (edit[134] != alg_drawn || asel != alg_op_drawn
                               || opmute != mute_drawn || lsig != lfo_sig_drawn)) {
                alg_drawn = edit[134];
                alg_op_drawn = asel;
                lfo_sig_drawn = lsig;
                if (eg_glo())       { draw_geg(ALG_Y, ALG_H); eg_live = v_lit ? 1u : 0u; eg_dirty = 0; }
                else if (eg_pit())  { draw_peg(ALG_Y, ALG_H, pit_pg ? 0xCu : 0x3u); eg_live = v_lit ? 1u : 0u; eg_dirty = 0; }
                else if (lfo_glo()) { draw_lfo_plot(ALG_Y, ALG_H); eg_live = v_lit ? 1u : 0u; eg_dirty = 0; }
                else                { draw_alg(); }
            }
        } else if (page == PAGE_LFO || (page == PAGE_EG && (bank == 0u || eg_pit()))) {
            uint32_t ch = 0;                    /* the four cells are one blit, so one of them is all */
            for (e = 0; e < 4u; e++) {
                const param_t *p = e < bank_rows(bank) ? page_at(bank_first(bank) + e) : 0;
                uint16_t v = p ? (uint16_t)p_cache(p) : (uint16_t)0xFFFEu;
                if (shown[e] != v) { shown[e] = v; ch = 1; }
            }
            if (ch) {
                draw_eg_cells();
                eg_dirty = 1;
            }
        } else if (glo_diagnostics()) {
            if (diagnostics_fresh || fm1_ms-t_diagnostics >= 250u) {
                draw_diagnostics(nv); t_diagnostics = fm1_ms; diagnostics_fresh = 0;
            }
        } else if (page == PAGE_STEP) {
            draw_steps();                       /* every pass: it draws only what moved */
        } else {
            for (e = 0; e < bank_rows(bank); e++) {
                uint32_t pi = bank_first(bank) + e;
                const param_t *p = page_at(pi);
                uint16_t v = p ? (uint16_t)p_cache(p) : (uint16_t)0xFFFEu;  /* FFFE: this row is empty */
                if (shown[e] != v) {
                    shown[e] = v;
                    draw_row(e, p, p ? *p_ref(p) : 0u);
                    eg_dirty = 1;
                }
            }
        }
        mute_drawn = opmute;
        sp_want = (page == PAGE_HOME && bank != 0u);
        if (fm1_ms - t_live > 60u) {            /* the displays that follow the sound, ~16 times a second */
            t_live = fm1_ms;
            if (page == PAGE_HOME) {
                if (bank == 0u && eg_glo()) {   /* the global envelope's marker, same cadence */
                    uint32_t snd = v_lit ? 1u : 0u;
                    if (snd || snd != eg_live || eg_dirty) {
                        eg_live = snd;
                        eg_dirty = 0;
                        draw_geg(ALG_Y, ALG_H);
                    }
                } else if (bank == 0u && eg_pit()) {    /* ..and the pitch envelope's */
                    uint32_t snd = v_lit ? 1u : 0u;
                    if (snd || snd != eg_live || eg_dirty) {
                        eg_live = snd;
                        eg_dirty = 0;
                        draw_peg(ALG_Y, ALG_H, pit_pg ? 0xCu : 0x3u);
                    }
                } else if (bank == 0u && lfo_glo()) {   /* ..and the LFO's, which runs the fastest */
                    uint32_t snd = v_lit ? 1u : 0u;
                    if (snd || snd != eg_live || eg_dirty) {
                        eg_live = snd;
                        eg_dirty = 0;
                        draw_lfo_plot(ALG_Y, ALG_H);
                    }
                } else if (bank == 0u) {
                    draw_alg_levels();
                } else if (bank == 2u) {
                    draw_scope();
                } else {
                    sp_run();
                    draw_spectrum();
                }
            } else if (page == PAGE_EG) {
                uint32_t snd = v_lit ? 1u : 0u; /* when it goes quiet, one last pass clears the marker */
                if (snd || snd != eg_live || eg_dirty) {
                    eg_live = snd;
                    eg_dirty = 0;
                    draw_eg_now(bank);
                }
            } else if (page == PAGE_LFO) {
                uint32_t snd = v_lit ? 1u : 0u;
                if (snd || snd != eg_live || eg_dirty) {
                    eg_live = snd;
                    eg_dirty = 0;
                    draw_lfo_plot(EG_Y, EG_H_TALL);
                }
            }
        }

        if (page == PAGE_GLO && fm1_ms - t_meter > 250u) {   /* the meter moves every block: redraw it
                                                              * 4 times a second, and only where it is */
            uint32_t cpu = cpu_q8 * 100u / 256u, pk = cpu_peak;
            t_meter = fm1_ms;
            if (set.oct != oct_drawn || nv != nv_drawn || cpu != cpu_drawn || pk != pk_drawn
                || monomode != mono_drawn || repaint_meter) {
                oct_drawn = set.oct;
                nv_drawn = nv;
                cpu_drawn = cpu;
                pk_drawn = pk;
                mono_drawn = monomode;
                repaint_meter = 0;
                draw_status(set.oct, nv, cpu, pk);
            }
        }

        }
        if (dirty && midi_quiet_blocks >= 20u && !v_lit && !seq_run &&
            !(arp_on && chord_n) && fm1_ms - t_change > 2000u) {
            /* an erase stalls the sound: save when silent, and not in the gaps of a loop, which the
             * stall would put out of time */
            set.voice = (uint8_t)voice;
            set.voice_hi = (uint8_t)(voice >> 8);
            if (!fm_settings_write(&set)) {
                dirty = mi_dirty = 0; mi_drawn = ~0u;
            } else {
                say("SETTINGS SAVE FAILED"); t_change = fm1_ms; mi_feedback = 5; mi_drawn = ~0u;
            }
        }
    }
}
