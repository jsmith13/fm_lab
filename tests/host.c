/* SPDX-License-Identifier: GPL-3.0-only */
/* A fake FM-1 on a computer: the app (firmware/src/fmlab.c) runs unchanged on it. platform.c includes
 * this file instead of the hardware when DINGHY_HOST is set. A script presses keys, turns encoders and
 * sends MIDI over time; out come the screen (PPM, at the times SHOT asks, or the "T shot" lines of a
 * script) and the sound (WAV, as the
 * FM-1 sends it over USB).
 *
 *   tests/run_host.sh            (cc + sh)
 *   tests\run_host.ps1           (MSVC + PowerShell)
 * or by hand:
 *   mkdir -p build/host
 *   cc -O2 -Wall -DDINGHY_HOST -Ifirmware/src -Ifirmware/gen -o build/host/fmlab firmware/src/fmlab.c
 *   build/host/fmlab build/host/fmlab         (-> build/host/fmlab.wav, build/host/fmlab-<ms>.ppm)
 *   build/host/fmlab OUT script.txt [FPS]     (your own script; with FPS: OUT-NNNNN.ppm, a frame every 1/FPS s:
 *                                             a film of your firmware, e.g. ffmpeg -r 30 -i OUT-%05d.ppm ...)
 *
 * Change SCRIPT below to try your own app. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef APP_NAME
#define APP_NAME "FM LAB"
#endif
#ifndef APP_VER
#define APP_VER "DEV"
#endif
#define FS 44100u
#define HALF_FRAMES 256u

static volatile uint32_t fm1_ms;
static struct { uint32_t notes, buttons; } fm1_in;
/* A button is a thing you hold, not an instant: EDIT and ENV mean different things depending on
 * how long you hold them, so the script has to be able to say. `btn n` is a tap; `btn n 600` holds
 * it for 600 ms. The press edge goes out at once, the bit stays in fm1_in.buttons meanwhile, and
 * the release edge goes out when the time is up -- which is exactly what the hardware does. */
#define HOST_TAP 50u
static uint32_t host_edges, host_rel, host_btn_up[14];
static int32_t host_enc[7];
static uint32_t fm1_input_edges(uint32_t *released)
{
    uint32_t b = host_edges;
    host_edges = 0;
    *released = host_rel;
    host_rel = 0;
    return b;
}
static uint32_t fm1_input_note_edges(void) { return 0; }   /* (the script sets fm1_in.notes) */
static int32_t fm1_enc_take(uint32_t e) { int32_t d = e < 7u ? host_enc[e] : 0; if (e < 7u) host_enc[e] = 0; return d; }
static void fm1_led_key(uint32_t id, int on) { (void)id; (void)on; }
static void fm1_led_key_glow(uint32_t id, int on) { (void)id; (void)on; }
#define FM1_ADC_MASTER 0u
#define FM1_ADC_BATT 1u
static int32_t fm1_adc_read(uint32_t ch) { return ch == FM1_ADC_MASTER ? 1023 : 800; }
/* The battery and the USB host, which on the device live in platform.c. 800 counts is a full
 * battery and no host is there, so a run draws the three bars and never the charging animation. */
static int32_t batt_raw = 800;
static int fm1_usb_host(void) { return 0; }
#define FM1_TICKS_PER_US 24u
/* On the device this is TIMER4, free-running off the 24 MHz crystal. Here it is *this computer's*
 * clock, scaled to the same 24 MHz: it makes the load meter compile and move, but the number it shows
 * is this computer's speed, not the FM-1's. Only a flashed build measures the real thing. */
static uint32_t fm1_ticks(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint32_t)((uint64_t)ts.tv_sec * 24000000u + (uint64_t)ts.tv_nsec * 24u / 1000u);
}
static uint32_t fm1_micros(void) { return fm1_ms * 1000u; }
/* the host calls the "audio ISR" from platform_poll, on the one thread: nothing to order */
#define RING_PUBLISH() ((void)0)

/* the screen: 240 x 240 RGB565, as the ST7789 gets it (gfx.c sends the bytes swapped) */
static uint16_t fb[240 * 240];
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = c;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u) {
                uint16_t c = px[j * w + i];
                fb[(y + j) * 240u + x + i] = (uint16_t)(c >> 8 | c << 8);
            }
}
#define CV_MAX (240u * 136u)         /* must match platform.c, or the host silently clamps a canvas */
#include "gfx.c"

static uint8_t host_store[256 + 4];
static int settings_load(void *p, uint32_t n)
{
    if (host_store[0] != 1 || n > 256u || host_store[1] == 0u || host_store[1] > n) return -1;
    memcpy(p, host_store + 4, host_store[1]);   /* a short store loads its prefix, as on the device */
    return 0;
}
static int settings_save(const void *p, uint32_t n)
{
    host_store[0] = 1;
    host_store[1] = (uint8_t)n;
    memcpy(host_store + 4, p, n);
    printf("%6u ms  settings saved (%u B)\n", fm1_ms, n);
    return 0;
}

static uint8_t mq[64][3];
static uint32_t mq_w, mq_r;
static int midi_get(uint8_t *st, uint8_t *d1, uint8_t *d2)
{
    if (mq_r == mq_w) return 0;
    *st = mq[mq_r % 64u][0]; *d1 = mq[mq_r % 64u][1]; *d2 = mq[mq_r % 64u][2];
    mq_r++;
    return 1;
}
static void midi_send(uint32_t st, uint32_t d1, uint32_t d2) { printf("%6u ms  MIDI out %02X %u %u\n", fm1_ms, st, d1, d2); }

static void app_audio(int32_t *out, uint32_t n);
static void app_main(void);

/* the script: at t ms, keys down (bit k = key k, 0 = F3), a button edge, an encoder's detents, a MIDI
 * message. A script file (the second argument) replaces it, a line each: "T keys MASK" (hex or decimal),
 * "T btn ID", "T enc ID STEPS", "T midi ST D1 D2", "T batt COUNTS", "T end" */
typedef struct { uint32_t t, keys; int btn, enc, encd; uint8_t st, d1, d2; const char *syx; uint32_t hold; int batt; } host_ev_t;
static const host_ev_t SCRIPT0[] = {
    {200, 1u << 7, -1, -1, 0, 0, 0, 0},                                 /* C4 */
    {700, 1u << 7 | 1u << 11 | 1u << 14, -1, -1, 0, 0, 0, 0},           /* + E4 G4: a chord */
    {1500, 0, -1, -1, 0, 0, 0, 0},
    {1700, 0, -1, -1, 0, 0x90, 81, 120},                                /* MIDI in: A5 */
    {2300, 0, -1, -1, 0, 0x80, 81, 0},
    {2500, 0, -1, ENC_PRESETS, 9, 0, 0, 0},                             /* voice 10: BELLS */
    {2700, 1u << 7 | 1u << 11 | 1u << 14 | 1u << 19, -1, -1, 0, 0, 0, 0},
    {3600, 0, -1, -1, 0, 0, 0, 0},
    {3900, 0, -1, ENC_PRESETS, -4, 0, 0, 0},                            /* voice 6 */
    {4100, 1u << 0 | 1u << 4 | 1u << 7 | 1u << 11 | 1u << 14 | 1u << 18 | 1u << 21 | 1u << 26, -1, -1, 0, 0, 0, 0},
    {4600, 1u << 0 | 1u << 4 | 1u << 7 | 1u << 11 | 1u << 14 | 1u << 18 | 1u << 21 | 1u << 26 | 1u << 24, -1, -1, 0, 0, 0, 0},
    {5200, 0, -1, -1, 0, 0, 0, 0},                                      /* (9 keys on 8 voices: one is stolen) */
    /* the editor, under a held chord: the sound must change without a retrigger */
    {5600, 0, BTN_EDIT, -1, 0, 0, 0, 0},                                /* OPERATOR page */
    {5700, 1u << 7 | 1u << 11 | 1u << 14, -1, -1, 0, 0, 0, 0},          /* chord down, and held */
    {6000, 1u << 7 | 1u << 11 | 1u << 14, -1, ENC_SELECT, 2, 0, 0, 0},  /* pick OP3 */
    {6300, 1u << 7 | 1u << 11 | 1u << 14, -1, ENC_KNOB1, -40, 0, 0, 0}, /* its LEVEL down 40 */
    {6600, 1u << 7 | 1u << 11 | 1u << 14, -1, ENC_KNOB2, 3, 0, 0, 0},   /* COARSE 3: a new ratio */
    {6900, 1u << 7 | 1u << 11 | 1u << 14, -1, ENC_ALGORITHM, 5, 0, 0, 0},
    {7300, 1u << 7 | 1u << 11 | 1u << 14, BTN_SEL, -1, 0, 0, 0, 0},     /* second bank of four */
    {7500, 1u << 7 | 1u << 11 | 1u << 14, BTN_ENV, -1, 0, 0, 0, 0},     /* ENVELOPE page: graph + 2 rows */
    {7700, 1u << 7 | 1u << 11 | 1u << 14, BTN_SEL, -1, 0, 0, 0, 0},     /* segment 2: RATE 2 / LEVEL 2 */
    {7900, 1u << 7 | 1u << 11 | 1u << 14, -1, ENC_KNOB1, -60, 0, 0, 0}, /* RATE 2 down: a slow decay */
    {8100, 1u << 7 | 1u << 11 | 1u << 14, -1, ENC_KNOB2, -30, 0, 0, 0}, /* and it decays further */
    {8400, 1u << 7 | 1u << 11 | 1u << 14, BTN_HOME, -1, 0, 0, 0, 0},    /* the algorithm, lit by the sound */
    {8700, 1u << 7 | 1u << 11 | 1u << 14, -1, ENC_SELECT, 1, 0, 0, 0},  /* the box moves to OP4 */
    {9000, 0, -1, -1, 0, 0, 0, 0},
    {9200, 0, -1, ENC_ALGORITHM, 10, 0, 0, 0},                          /* a different diagram */
    {9400, 0, BTN_SAVE, -1, 0, 0, 0, 0},                                /* "SAVE TO U1": which slot? */
    {9600, 1u << 7 | 1u << 11 | 1u << 14, -1, -1, 0, 0, 0, 0},
    {10200, 0, -1, -1, 0, 0, 0, 0},
    {10400, 0, BTN_GLO, -1, 0, 0, 0, 0},
};
static const host_ev_t *script = SCRIPT0;
static uint32_t script_n = sizeof SCRIPT0 / sizeof SCRIPT0[0], end_ms = 11200, frame_fps;
static const uint32_t SHOT[] = {1000, 3000, 4500, 6700, 8000, 8300, 8900, 9300, 10600};
static const uint32_t *shots = SHOT;
static uint32_t shots_n = sizeof SHOT / sizeof SHOT[0];

static void read_script(const char *path)
{
    static host_ev_t ev[4096];
    static uint32_t shot_at[512];
    char line[256], what[16];
    FILE *f = fopen(path, "r");
    uint32_t n = 0, ns = 0;
    if (!f) { perror(path); exit(1); }
    while (fgets(line, sizeof line, f) && n < 4096u) {
        host_ev_t e = {0, 0, -1, -1, 0, 0, 0, 0, 0, 0, 0};
        unsigned a = 0, b = 0, c = 0;
        if (line[0] == '#' || sscanf(line, "%u %15s", &e.t, what) != 2) continue;
        if (!strcmp(what, "end")) { end_ms = e.t; continue; }
        if (!strcmp(what, "shot")) { if (ns < 512u) shot_at[ns++] = e.t; continue; }
        if (!strcmp(what, "keys")) { sscanf(line, "%*u %*s %i", (int *)&e.keys); }
        else if (!strcmp(what, "btn")) { sscanf(line, "%*u %*s %d %u", &e.btn, &e.hold); }
        else if (!strcmp(what, "enc")) { sscanf(line, "%*u %*s %d %d", &e.enc, &e.encd); }
        else if (!strcmp(what, "batt")) { sscanf(line, "%*u %*s %d", &e.batt); }   /* raw ADC counts */
        else if (!strcmp(what, "midi")) { sscanf(line, "%*u %*s %i %u %u", (int *)&a, &b, &c); e.st = (uint8_t)a; e.d1 = (uint8_t)b; e.d2 = (uint8_t)c; }
        else if (!strcmp(what, "syx")) {        /* a .syx file, as if the host had sent it */
            static char pool[32][256];
            static uint32_t np;
            if (np >= 32u || sscanf(line, "%*u %*s %255s", pool[np]) != 1) continue;
            e.syx = pool[np++];
        }
        else continue;
        ev[n++] = e;
    }
    fclose(f);
    script = ev;
    script_n = n;
    if (ns) { shots = shot_at; shots_n = ns; }  /* "T shot" lines replace the built-in list */
}

static const char *host_out = "build/host/fmlab";
static int16_t *wav;
static uint8_t *wav24;                          /* the same, as the DAC gets it: 24-bit, beside it as -24.wav */
static uint32_t wav_n;

/* the voice bank, as a file beside the WAV and the screenshots, so a run can see what the one
 * before it saved -- the same way the flash sector behaves on the FM-1 */
static int app_sysex(const uint8_t *p, uint32_t n);
static void host_path(char *out, uint32_t max, const char *suffix)
{
    snprintf(out, max, "%s%s", host_out, suffix);
}
static void bank_path(char *out, uint32_t max, uint32_t b)   /* bank 0 is -bank.bin, then -bank2.bin.. */
{
    char suf[16];
    if (b)
        snprintf(suf, sizeof suf, "-bank%u.bin", b + 1u);
    else
        snprintf(suf, sizeof suf, "-bank.bin");
    host_path(out, max, suf);
}
static int bank_read(uint32_t b, void *p, uint32_t n)
{
    char name[512];
    FILE *f;
    size_t got;
    if (b >= 8u) return -1;
    bank_path(name, sizeof name, b);
    if (!(f = fopen(name, "rb"))) return -1;
    got = fread(p, 1, n, f);
    fclose(f);
    return got == n ? 0 : -1;
}
static int bank_write(uint32_t b, const void *p, uint32_t n)
{
    char name[512];
    FILE *f;
    if (b >= 8u) return -1;
    bank_path(name, sizeof name, b);
    if (!(f = fopen(name, "wb"))) { perror(name); return -1; }
    fwrite(p, 1, n, f);
    fclose(f);
    printf("%6u ms  bank written (%u B) -> %s\n", fm1_ms, n, name);
    return 0;
}
static int sysex_send(const uint8_t *p, uint32_t n)      /* everything sent, in order, into one file */
{
    char name[512];
    FILE *f;
    host_path(name, sizeof name, "-out.syx");
    if (!(f = fopen(name, "ab"))) { perror(name); return -1; }
    fwrite(p, 1, n, f);
    fclose(f);
    printf("%6u ms  sysex out (%u B) -> %s\n", fm1_ms, n, name);
    return 0;
}

/* a .syx file from the script: every F0..F7 frame in it, offered to the app as the platform would */
static void host_syx(const char *path)
{
    static uint8_t buf[65536];
    FILE *f = fopen(path, "rb");
    uint32_t n, i = 0;
    if (!f) { perror(path); exit(1); }
    n = (uint32_t)fread(buf, 1, sizeof buf, f);
    fclose(f);
    while (i < n) {
        uint32_t j;
        if (buf[i] != 0xF0u) { i++; continue; }
        for (j = i + 1u; j < n && buf[j] != 0xF7u; j++) ;
        if (j >= n) break;
        printf("%6u ms  sysex in (%u B) from %s: %s\n", fm1_ms, j - i - 1u, path,
               app_sysex(buf + i + 1u, j - i - 1u) ? "taken" : "not ours");
        i = j + 1u;
    }
}

static void write_ppm_as(const char *name)
{
    FILE *f;
    uint32_t i;
    if (!(f = fopen(name, "wb"))) { perror(name); exit(1); }
    fprintf(f, "P6 240 240 255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t c = fb[i];
        fputc((c >> 11) * 255 / 31, f);
        fputc((c >> 5 & 63) * 255 / 63, f);
        fputc((c & 31) * 255 / 31, f);
    }
    fclose(f);
}
static void write_ppm(uint32_t t)
{
    char name[512];
    snprintf(name, sizeof name, "%s-%u.ppm", host_out, t);
    write_ppm_as(name);
    printf("%6u ms  screen -> %s\n", t, name);
}

static void write_wav_as(const char *suffix, const void *data, uint32_t bps)
{
    char name[512];
    FILE *f;
    uint32_t fb = 2u * bps / 8u, bytes = wav_n * fb;
    uint32_t v[] = {16, 0x00020001u, FS, FS * fb, fb | bps << 16};
    snprintf(name, sizeof name, "%s%s.wav", host_out, suffix);
    if (!(f = fopen(name, "wb"))) { perror(name); exit(1); }
    fwrite("RIFF", 1, 4, f); bytes += 36; fwrite(&bytes, 4, 1, f); bytes -= 36;
    fwrite("WAVEfmt ", 1, 8, f); fwrite(v, 4, 5, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    fwrite(data, fb, wav_n, f);
    fclose(f);
    printf("sound  -> %s (%u frames)\n", name, wav_n);
}

static void write_wav(void)
{
    write_wav_as("", wav, 16u);
    write_wav_as("-24", wav24, 24u);
}

static void platform_poll(void)                 /* one call = one millisecond */
{
    static uint32_t step, shot, owed, frame;
    static int32_t blk[2u * HALF_FRAMES];
    fm1_ms++;
    {   /* anything held long enough has been let go of */
        uint32_t n;
        for (n = 0; n < 14u; n++)
            if (host_btn_up[n] && fm1_ms >= host_btn_up[n]) {
                host_btn_up[n] = 0;
                fm1_in.buttons &= ~(1u << n);
                host_rel |= 1u << n;
            }
    }
    while (step < script_n && script[step].t <= fm1_ms) {
        if (script[step].syx)
            host_syx(script[step].syx);
        else if (script[step].st) {
            mq[mq_w % 64u][0] = script[step].st; mq[mq_w % 64u][1] = script[step].d1; mq[mq_w % 64u][2] = script[step].d2;
            mq_w++;
        } else if (script[step].batt) {
            batt_raw = script[step].batt;       /* the divider, said in counts: see draw_corners */
        } else if (script[step].btn >= 0 && script[step].btn < 14) {
            uint32_t n = (uint32_t)script[step].btn;
            host_edges |= 1u << n;
            fm1_in.buttons |= 1u << n;
            host_btn_up[n] = fm1_ms + (script[step].hold ? script[step].hold : HOST_TAP);
        }
        else if (script[step].enc >= 0 && script[step].enc < 7)
            host_enc[script[step].enc] += script[step].encd;
        else
            fm1_in.notes = script[step].keys;
        step++;
    }
    owed += FS;                                 /* the frames this millisecond is worth, in blocks as the ISR renders */
    while (owed >= 1000u * HALF_FRAMES) {
        uint32_t i;
        owed -= 1000u * HALF_FRAMES;
        memset(blk, 0, sizeof blk);
        app_audio(blk, HALF_FRAMES);
        wav = realloc(wav, (wav_n + HALF_FRAMES) * 4u);
        wav24 = realloc(wav24, (wav_n + HALF_FRAMES) * 6u);
        for (i = 0; i < 2u * HALF_FRAMES; i++) {
            int32_t v = blk[i];                 /* (the MASTER knob all the way up) Q23 */
            uint8_t *p = &wav24[6u * wav_n + 3u * i];
            v = v > 8388607 ? 8388607 : v < -8388607 ? -8388607 : v;
            p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16);
            v = (v + 128) >> 8;                 /* rounded, not dithered: a run is the same every time */
            wav[2u * wav_n + i] = (int16_t)(v > 32767 ? 32767 : v);
        }
        wav_n += HALF_FRAMES;
    }
    if (frame_fps) {                            /* a film: a frame every 1/fps s, as the screen is then */
        if ((uint64_t)frame * 1000u <= (uint64_t)fm1_ms * frame_fps) {
            char name[512];
            snprintf(name, sizeof name, "%s-%05u.ppm", host_out, frame++);
            write_ppm_as(name);
        }
    } else if (shot < shots_n && fm1_ms == shots[shot])
        write_ppm(shots[shot++]);
    if (fm1_ms >= end_ms) {
        write_wav();
        exit(0);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) host_out = argv[1];
    if (argc > 2) read_script(argv[2]);
    if (argc > 3) frame_fps = (uint32_t)atoi(argv[3]);
    app_main();
    return 0;
}
