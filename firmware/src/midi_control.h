/* SPDX-License-Identifier: GPL-3.0-only */
/* Shared protocol/learn implementation, mirrored in the independent FM/WT firmware copies.
 * The main loop publishes controls; only the audio owner advances the ramps. */
#ifndef MIDI_CONTROL_H
#define MIDI_CONTROL_H
#include <stdint.h>
#ifdef DINGHY_HOST
#include <string.h>
#endif

#define MI_SLOTS 16u
#define MI_PRESSURE 128u
#define MI_OFF 255u
#define MI_MAGIC 0x3149434Du
#define MI_BYTES 72u
#define MI_GROUP_COUNT (sizeof mi_groups/sizeof mi_groups[0])
enum { MI_NONE, MI_VIBRATO, MI_AMPLITUDE, MI_VOLUME, MI_EXPRESSION, MI_PAN,
       MI_CUTOFF, MI_RESONANCE, MI_BRIGHTNESS, MI_REVERB, MI_CHORUS, MI_DELAY, MI_PHASER };
enum { MI_IGNORED, MI_CONTROL, MI_PEDAL, MI_SOUND_OFF, MI_RESET, MI_NOTES_OFF,
       MI_LEARNED, MI_RESERVED };
typedef struct { uint8_t source, target, depth, negative; } mi_route_t;
typedef struct { uint32_t magic; uint8_t bend_range, reserved[3]; mi_route_t route[MI_SLOTS]; } mi_config_t;
typedef char mi_config_size[(sizeof(mi_config_t) == MI_BYTES) ? 1 : -1];
typedef struct {
    mi_config_t config;
    uint8_t value[129];
    uint32_t active[5];
    uint32_t suppressed;
    int32_t bend;
} mi_publication_t;
typedef struct { int32_t current, target, step, remainder, error; uint32_t left, divisor; } mi_ramp_t;
static mi_config_t mi_config;
static mi_publication_t mi_input, mi_published[2], mi_audio;
static mi_ramp_t mi_ramp[MI_SLOTS], mi_mix[MI_SLOTS], mi_bend;
static mi_ramp_t mi_left, mi_right;
static volatile uint32_t mi_index, mi_generation;
static uint32_t mi_seen, mi_slot, mi_editor, mi_learn, mi_drawn = ~0u;
static uint32_t mi_dirty, mi_feedback, mi_full_redraw;
static void mi_output_prepare(uint32_t n);
static int mi_range_turn(int32_t delta);

static int mi_reserved(uint32_t cc)
{
    return cc == 0u || cc == 32u || cc == 64u || cc >= 120u;
}

static void mi_defaults(mi_config_t *c)
{
    static const uint8_t cc[] = {1,7,11,10,74,71,91,93,MI_PRESSURE,MI_PRESSURE};
    static const uint8_t target[] = {MI_VIBRATO,MI_VOLUME,MI_EXPRESSION,MI_PAN,MI_CUTOFF,
                                    MI_RESONANCE,MI_REVERB,MI_CHORUS,MI_VIBRATO,MI_BRIGHTNESS};
    uint32_t i;
    memset(c, 0, sizeof *c);
    c->magic = MI_MAGIC; c->bend_range = 2;
    for (i = 0; i < MI_SLOTS; i++) { c->route[i].source = MI_OFF; c->route[i].depth = 100; }
    for (i = 0; i < sizeof cc; i++) {
        c->route[i].source = cc[i]; c->route[i].target = target[i];
        c->route[i].depth = i == 0u || i == 8u ? 12u : i == 9u ? 25u : 100u;
    }
}

static int mi_valid(const mi_config_t *c)
{
    uint32_t i;
    if (c->magic != MI_MAGIC || c->bend_range > 24u ||
        c->reserved[0] || c->reserved[1] || c->reserved[2]) return 0;
    for (i = 0; i < MI_SLOTS; i++) {
        const mi_route_t *r = &c->route[i];
        if (r->target >= MI_TARGET_COUNT || r->depth > 100u || r->negative > 1u ||
            (r->source != MI_OFF && (r->source > MI_PRESSURE ||
             (r->source < MI_PRESSURE && mi_reserved(r->source))))) return 0;
    }
    return 1;
}

static int mi_default_config(const mi_config_t *c)
{
    mi_config_t defaults;
    mi_defaults(&defaults);
    return !memcmp(c, &defaults, sizeof defaults);
}

static void mi_publish(void)
{
    uint32_t next = mi_index ^ 1u;
    mi_input.config = mi_config;
    mi_published[next] = mi_input;
    RING_PUBLISH(); mi_index = next;
    RING_PUBLISH(); mi_generation++;
}

static void mi_reset_input(void)
{
    uint32_t i;
    /* Keep volume, pan and sound/effect levels; reset expressive controllers. */
    mi_input.value[1] = mi_input.value[MI_PRESSURE] = 0;
    mi_input.value[11] = 127; mi_input.bend = 0;
    for (i = 0; i < 129u; i++)
        if (i != 7u && i != 10u && !(i >= 70u && i <= 79u) && !(i >= 91u && i <= 95u)) {
            mi_input.value[i] = (uint8_t)(i == 11u ? 127u : 0u);
            mi_input.active[i >> 5] &= ~(1u << (i & 31u));
        }
    mi_input.active[0] |= (1u << 1) | (1u << 11);
    mi_input.active[4] |= 1u;
}

static void mi_init(void)
{
    mi_defaults(&mi_config);
    memset(&mi_input, 0, sizeof mi_input);
    memset(mi_published, 0, sizeof mi_published);
    memset(&mi_audio, 0, sizeof mi_audio);
    memset(mi_ramp, 0, sizeof mi_ramp); memset(&mi_bend, 0, sizeof mi_bend);
    memset(mi_mix,0,sizeof mi_mix);
    memset(&mi_left,0,sizeof mi_left); memset(&mi_right,0,sizeof mi_right);
    mi_left.current = mi_left.target = mi_right.current = mi_right.target = 32768;
    mi_input.value[7] = mi_input.value[11] = 127; mi_input.value[10] = 64;
    mi_input.active[0] = (1u << 1) | (1u << 7) | (1u << 10) | (1u << 11);
    mi_input.active[4] = 1u;
    mi_index = mi_generation = mi_seen = mi_slot = mi_editor = mi_learn = 0;
    mi_dirty = mi_feedback = 0; mi_full_redraw = 1;
    mi_drawn = ~0u;
    mi_publish();
}

static int mi_receive(uint32_t kind, uint32_t d1, uint32_t d2)
{
    uint32_t source;
    int learned = 0;
    if (kind == 0xE0u) {
        int32_t bend = (int32_t)(d1 + 128u*d2) - 8192;
        mi_input.bend = bend < 0 ? bend * 8 : bend * 65536 / 8191;
        mi_publish(); return MI_CONTROL;
    }
    if (kind == 0xB0u) {
        if (mi_learn && mi_reserved(d1)) { mi_feedback = 2; mi_drawn = ~0u; }
        if (d1 == 64u) return MI_PEDAL;
        if (d1 == 120u) return MI_SOUND_OFF;
        if (d1 == 121u) { mi_reset_input(); mi_publish(); return MI_RESET; }
        if (d1 == 123u) return MI_NOTES_OFF;
        if (mi_reserved(d1)) return MI_IGNORED;
        source = d1;
    } else if (kind == 0xD0u) source = MI_PRESSURE;
    else return MI_IGNORED;
    if (mi_learn) {
        mi_config.route[mi_slot].source = (uint8_t)source;
        mi_learn = 0; mi_drawn = ~0u; learned = 1; mi_dirty = mi_feedback = 1;
    }
    mi_input.value[source] = (uint8_t)(kind == 0xD0u ? d1 : d2);
    {
        uint32_t i;
        for (i = 0; i < MI_SLOTS; i++)
            if (mi_config.route[i].source == source) mi_input.suppressed &= ~(1u << i);
    }
    mi_input.active[source >> 5] |= 1u << (source & 31u);
    mi_publish();
    return learned ? MI_LEARNED : MI_CONTROL;
}

static void mi_ramp_target_n(mi_ramp_t *r, int32_t target, uint32_t n)
{
    int32_t delta;
    if (r->target == target) return;
    delta = target-r->current; r->target = target;
    r->left = r->divisor = n;
    r->step = delta/(int32_t)r->left;
    r->remainder = delta%(int32_t)r->left; r->error = 0;
}

static void mi_ramp_target(mi_ramp_t *r, int32_t target)
{
    mi_ramp_target_n(r,target,FS/50u);
}

static void mi_ramp_advance(mi_ramp_t *r, uint32_t n)
{
    while (n-- && r->left) {
        r->current += r->step; r->error += r->remainder;
        if (r->error >= (int32_t)r->divisor) { r->current++; r->error -= (int32_t)r->divisor; }
        if (r->error <= -(int32_t)r->divisor) { r->current--; r->error += (int32_t)r->divisor; }
        if (!--r->left) r->current = r->target;
    }
}

static void mi_audio_sync(void)
{
    uint32_t i;
    if (mi_seen == mi_generation) return;
    RING_PUBLISH();
    for (i = 0; i < MI_SLOTS; i++) {
        uint32_t source = mi_published[mi_index].config.route[i].source;
        uint32_t active = source != MI_OFF &&
            (mi_published[mi_index].active[source >> 5] & (1u << (source & 31u))) &&
            !(mi_published[mi_index].suppressed & (1u << i));
        uint32_t was_active = source != MI_OFF &&
            (mi_audio.active[source >> 5] & (1u << (source & 31u))) &&
            !(mi_audio.suppressed & (1u << i));
        int32_t value = 0;
        if (source != MI_OFF) {
            uint32_t raw = mi_published[mi_index].value[source];
            value = source == 10u ? (raw < 64u ? (int32_t)(raw*512u) :
                        32768+(int32_t)((raw-64u)*32768u/63u)) : (int32_t)(raw*65536u/127u);
        }
        if (mi_audio.config.magic != MI_MAGIC ||
            memcmp(&mi_audio.config.route[i], &mi_published[mi_index].config.route[i], sizeof(mi_route_t))) {
            mi_ramp[i].current = mi_ramp[i].target = value; mi_ramp[i].left = 0;
            mi_mix[i].current = mi_mix[i].target =
                mi_audio.config.magic == MI_MAGIC ? 0 : active ? 65536 : 0;
            mi_mix[i].left = 0;
        } else if (active && !was_active) {
            mi_ramp[i].current = mi_ramp[i].target = value; mi_ramp[i].left = 0;
        } else if (active) mi_ramp_target(&mi_ramp[i], value);
        mi_ramp_target(&mi_mix[i],active ? 65536 : 0);
    }
    mi_audio = mi_published[mi_index];
    mi_ramp_target(&mi_bend, mi_audio.bend);
    mi_seen = mi_generation;
}

static void mi_tick(uint32_t n)
{
    uint32_t i;
    mi_audio_sync();
    for (i = 0; i < MI_SLOTS; i++) {
        mi_ramp_advance(&mi_ramp[i], n); mi_ramp_advance(&mi_mix[i],n);
    }
    mi_ramp_advance(&mi_bend, n);
    if (n) mi_output_prepare(n);
}

static int32_t mi_value(uint32_t target, int32_t base, int32_t lo, int32_t hi)
{
    uint32_t i;
    int32_t value = base;
    for (i = 0; i < MI_SLOTS; i++) {
        const mi_route_t *r = &mi_audio.config.route[i];
        int32_t q, span = hi-lo, delta;
        if (r->target != target || r->source == MI_OFF || !r->depth ||
            !mi_mix[i].current) continue;
        q = mi_ramp[i].current;
        if (target == MI_VIBRATO || target == MI_BRIGHTNESS || target == MI_AMPLITUDE ||
            r->source == 1u || r->source == MI_PRESSURE) {
            delta = (int32_t)(((int64_t)q * span) >> 16);
            delta = (r->negative ? -delta : delta) * r->depth / 100;
        } else {
            int32_t position = lo+(int32_t)(((int64_t)(r->negative ? 65536-q : q)*span) >> 16);
            delta = (position-base)*r->depth/100;
        }
        value += (int32_t)(((int64_t)delta*mi_mix[i].current) >> 16);
    }
    return value < lo ? lo : value > hi ? hi : value;
}

static int32_t mi_pitch(void)
{
    return (int32_t)(((int64_t)mi_bend.current * mi_audio.config.bend_range * 256) >> 16);
}

static void mi_output_prepare(uint32_t n)
{
    int32_t gain = mi_value(MI_VOLUME,32768,0,32768);
    int32_t pan = mi_value(MI_PAN,32768,0,65536), l = 32768, r = 32768;
    gain = (int32_t)(((int64_t)gain*mi_value(MI_EXPRESSION,32768,0,32768)) >> 15);
    gain = (int32_t)(((int64_t)gain*mi_value(MI_AMPLITUDE,32768,0,65536)) >> 15);
    if (pan < 32768) r = pan; else l = 65536-pan;
    mi_ramp_target_n(&mi_left,(int32_t)(((int64_t)gain*l) >> 15),n);
    mi_ramp_target_n(&mi_right,(int32_t)(((int64_t)gain*r) >> 15),n);
}

static void mi_stereo(int32_t *left, int32_t *right)
{
    if (!mi_left.left && !mi_right.left && mi_left.current == 32768 && mi_right.current == 32768) return;
    mi_ramp_advance(&mi_left,1); mi_ramp_advance(&mi_right,1);
    *left = (int32_t)(((int64_t)*left*mi_left.current) >> 15);
    *right = (int32_t)(((int64_t)*right*mi_right.current) >> 15);
    if (*left > 8388607) *left = 8388607; else if (*left < -8388607) *left = -8388607;
    if (*right > 8388607) *right = 8388607; else if (*right < -8388607) *right = -8388607;
}

static const char *mi_target_name(uint32_t target)
{
    static const char *const common[] = {"NONE","VIBRATO","AMPLITUDE","VOLUME","EXPRESSION","PAN",
        "CUTOFF","RESONANCE","BRIGHTNESS","REVERB","CHORUS","DELAY","PHASER"};
    return target < 13u ? common[target] : mi_extra_names[target-13u];
}

static uint32_t mi_wrap(int32_t value, uint32_t count)
{
    value %= (int32_t)count;
    return (uint32_t)(value < 0 ? value+(int32_t)count : value);
}

static void mi_slot_turn(int32_t delta)
{
    mi_slot = mi_wrap((int32_t)mi_slot+delta,MI_SLOTS);
    mi_learn = mi_feedback = 0; mi_drawn = ~0u;
}

static uint32_t mi_group_index(uint32_t target)
{
    uint32_t g, i;
    for (g = 0; g < MI_GROUP_COUNT; g++)
        for (i = 1; i <= mi_groups[g][0]; i++)
            if (mi_groups[g][i] == target) return g;
    return 0;
}

static int mi_target_set(uint32_t target)
{
    if (mi_config.route[mi_slot].target == target) return 0;
    mi_config.route[mi_slot].target = (uint8_t)target;
    mi_input.suppressed &= ~(1u << mi_slot);
    mi_learn = mi_feedback = 0; mi_dirty = 1; mi_drawn = ~0u; mi_publish(); return 1;
}

static int mi_group_turn(int32_t delta)
{
    uint32_t group;
    if (!delta) return 0;
    group = mi_wrap((int32_t)mi_group_index(mi_config.route[mi_slot].target)+delta,MI_GROUP_COUNT);
    return mi_target_set(mi_groups[group][1]);
}

static int mi_target_turn(int32_t delta)
{
    const uint8_t *group = mi_groups[mi_group_index(mi_config.route[mi_slot].target)];
    uint32_t i;
    if (!delta) return 0;
    for (i = 0; i < group[0]; i++)
        if (group[i+1u] == mi_config.route[mi_slot].target) break;
    return mi_target_set(group[1u+mi_wrap((int32_t)i+delta,group[0])]);
}

static int mi_knob(uint32_t knob, int32_t delta)
{
    mi_route_t *r = &mi_config.route[mi_slot];
    int32_t v;
    if (!delta) return 0;
    if (knob == 3u) return mi_range_turn(delta);
    if (knob == 0u) {
        uint32_t source = r->source == MI_OFF ? 118u : r->source == MI_PRESSURE ? 117u :
                          r->source-1u-(r->source > 32u)-(r->source > 64u);
        source = mi_wrap((int32_t)source+delta,119);
        r->source = (uint8_t)(source < 31u ? source+1u : source < 62u ? source+2u :
                             source < 117u ? source+3u : source == 117u ? MI_PRESSURE : MI_OFF);
    } else if (knob == 1u) {
        v = (int32_t)r->depth+delta; r->depth = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    } else r->negative = (uint8_t)(delta < 0);
    mi_input.suppressed &= ~(1u << mi_slot);
    mi_learn = mi_feedback = 0; mi_dirty = 1; mi_drawn = ~0u; mi_publish(); return 1;
}

static void mi_panel_target(uint32_t target)
{
    uint32_t i, before = mi_input.suppressed;
    if (target == MI_NONE) return;
    for (i = 0; i < MI_SLOTS; i++)
        if (mi_config.route[i].target == target && mi_config.route[i].source != MI_OFF)
            mi_input.suppressed |= 1u << i;
    if (before != mi_input.suppressed) mi_publish();
}

static int mi_range_turn(int32_t delta)
{
    int32_t v;
    if (!delta) return 0;
    v = (int32_t)mi_config.bend_range+delta;
    mi_config.bend_range = (uint8_t)(v < 0 ? 0 : v > 24 ? 24 : v);
    mi_dirty = 1; mi_feedback = 0; mi_drawn = ~0u; mi_publish(); return 1;
}

static void mi_open_editor(void)
{
    mi_editor = mi_full_redraw = 1; mi_learn = mi_feedback = 0; mi_drawn = ~0u;
}

static void mi_toggle_learn(void)
{
    mi_feedback = mi_config.route[mi_slot].target == MI_NONE ? 4u : 0u;
    mi_learn = mi_feedback ? 0u : mi_learn ^ 1u; mi_drawn = ~0u;
}

static void mi_clear(void)
{
    mi_config.route[mi_slot].source = MI_OFF;
    mi_learn = 0; mi_dirty = 1; mi_feedback = 3; mi_drawn = ~0u; mi_publish();
}

static void mi_invalid(void)
{
    if (mi_editor) { mi_feedback = 6; mi_drawn = ~0u; }
}

static char *mi_text(char *out, const char *s)
{
    while (*s) *out++ = *s++;
    *out = 0; return out;
}

static char *mi_number(char *out, uint32_t n)
{
    char digits[10]; uint32_t i = 0;
    do { digits[i++] = (char)('0'+n%10u); n /= 10u; } while (n);
    while (i) *out++ = digits[--i];
    *out = 0; return out;
}

static void mi_draw(void)
{
    char text[48], *p;
    const mi_route_t *r = &mi_config.route[mi_slot];
    uint32_t g = mi_group_index(r->target), index, i, neighbors;
    const uint8_t *group = mi_groups[g];
    static const char *const labels[] = {"SOURCE","DEPTH","POLAR","BEND"};
    const char *feedback[] = {"SEL LEARN / SAV CLEAR","CC LEARNED","RESERVED SOURCE",
                             "ASSIGNMENT CLEARED","SELECT DESTINATION","SETTINGS SAVE FAILED","INVALID MIDI"};
    if (mi_drawn != ~0u) return;
    mi_drawn = 0;
    if (mi_full_redraw) { lcd_fill(0,0,240,240,C_BLACK); mi_full_redraw = 0; }
    p = mi_number(mi_text(text,"CC ASSIGN "),mi_slot+1u); p = mi_text(p,"/16");
    if (mi_dirty) mi_text(p," *");
    draw_text_box(0,18,240,&FONT_S,text,RGB(180,140,255),1);
    mi_text(text,mi_group_names[g]);
    draw_text_box(0,38,240,&FONT_S,text,RGB(150,150,150),1);
    for (index = 0; index < group[0]; index++) if (group[index+1u] == r->target) break;
    cv_begin(240,98,C_BLACK);
    cv_rect(3,38,234,20,RGB(35,22,50));
    neighbors = group[0] >= 5u ? 2u : 1u;
    for (i = 0; i < 5u; i++) {
        uint32_t at;
        const char *name;
        if (i < 2u-neighbors || i > 2u+neighbors) continue;
        at = mi_wrap((int32_t)index+(int32_t)i-2,group[0]);
        name = mi_target_name(group[1u+at]);
        cv_text((240-text_w(&FONT_S,name))/2,4+(int32_t)i*18,&FONT_S,name,
                i == 2u ? C_WHITE : RGB(100,100,100));
    }
    cv_text(6,40,&FONT_S,">",RGB(180,140,255));
    cv_text(226,40,&FONT_S,"<",RGB(180,140,255));
    cv_blit(0,54);
    cv_begin(240,32,C_BLACK);
    for (i = 0; i < 4u; i++) {
        uint32_t amount;
        if (i == 0u) {
            if (r->source == MI_OFF) mi_text(text,"OFF");
            else if (r->source == MI_PRESSURE) mi_text(text,"PRESS");
            else mi_number(mi_text(text,"CC"),r->source);
            amount = r->source == MI_OFF ? 0u : 54u;
        } else if (i == 1u) {
            mi_text(mi_number(text,r->depth),"%"); amount = r->depth*54u/100u;
        } else if (i == 2u) {
            mi_text(text,r->negative ? "NEG" : "POS"); amount = r->negative ? 0u : 54u;
        } else {
            mi_number(mi_text(text,"+/-"),mi_config.bend_range); amount = mi_config.bend_range*54u/24u;
        }
        cv_text((int32_t)i*60+(60-text_w(&FONT_S,text))/2,4,&FONT_S,text,C_WHITE);
        cv_rect((int32_t)i*60+3,26,54,4,RGB(40,40,40));
        cv_rect((int32_t)i*60+3,26,(int32_t)amount,4,RGB(180,140,255));
    }
    cv_blit(0,176);
    cv_begin(240,16,C_BLACK);
    for (i = 0; i < 4u; i++)
        cv_text((int32_t)i*60+(60-text_w(&FONT_S,labels[i]))/2,0,&FONT_S,labels[i],RGB(150,150,150));
    cv_blit(0,208);
    draw_text_box(0,224,240,&FONT_S,mi_learn && mi_feedback != 2u && mi_feedback != 6u ?
                  "MOVE CC / PRESSURE" : feedback[mi_feedback],RGB(180,140,255),1);
}
#endif
