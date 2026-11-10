# FM Lab platform API

Everything [platform.c](../firmware/src/platform.c) gives your app. One compilation unit: `fmlab.c` includes
`platform.c` first, so all of this is just there. Nothing here is a library you link against; read the source
when in doubt, it is short.

- [The contract](#the-contract)
- [Time](#time)
- [Keys, buttons, encoders](#keys-buttons-encoders)
- [LEDs](#leds)
- [Screen](#screen)
- [Sound](#sound)
- [MIDI](#midi)
- [Settings](#settings)
- [Memory and flash](#memory-and-flash)
- [Build options](#build-options)

## The contract

Your app supplies two functions:

```c
static void app_audio(int32_t *out, uint32_t n);   /* the audio interrupt: fill n stereo frames */
static void app_main(void);                        /* your main loop; never returns */
```

`app_main` must call **`platform_poll()`** often: every few milliseconds, and never less than once every
couple of seconds. It feeds the watchdog (8 s, then the FM-1 resets), runs USB (MIDI, the updater, a computer
asking for the update mode) and reads the MASTER knob. A long job (drawing a lot, saving) is fine; a loop that
waits for something must poll.

```c
static void app_main(void)
{
    /* set up */
    for (;;) {
        platform_poll();
        /* read the panel, MIDI; draw what changed */
    }
}
```

Names the platform defines that you can change from the build: `APP_NAME` (the rescue and crash screens,
default `"FM LAB"`), `APP_VER` (set by `./build.sh --release`), `FELUCCA_ID` (the package identity; see
[MAKE-IT-YOURS.md](MAKE-IT-YOURS.md)).

## Time

| | |
| --- | --- |
| `fm1_ms` | milliseconds since power-on (`volatile uint32_t`, counted by the 10 kHz timer) |
| `fm1_ticks()` | a free-running 24 MHz counter: `FM1_TICKS_PER_US` (24) per microsecond; wraps in ~179 s, so subtract: `fm1_ticks() - t0` |
| `fm1_micros()` | microseconds, from the same counter |
| `fm1_delay_ms(ms)`, `fm1_delay_us(us)` | busy waits; keep them short (they do not poll) |

To time your audio code: `t0 = fm1_ticks()` before, `(fm1_ticks() - t0) / FM1_TICKS_PER_US` after.

## Keys, buttons, encoders

The panel is scanned at 10 kHz in the background and debounced.

| | |
| --- | --- |
| `fm1_in.notes` | the 27 keys held now: bit 0 = F3 ... bit 26 = G5 |
| `fm1_in.buttons` | the 14 buttons held now: bit = `BTN_...` |
| `fm1_input_edges(&released)` | buttons **pressed** since the last call (bits), and those released into `released` |
| `fm1_input_note_edges()` | keys pressed since the last call (bits): a press shorter than a loop pass still counts |
| `fm1_enc_take(ENC_...)` | detents turned since the last call, + = clockwise |
| `fm1_adc_read(FM1_ADC_MASTER)` | the MASTER knob, 0..1023 (-1 = not ready). platform.c already makes it the volume |
| `fm1_adc_read(FM1_ADC_BATT)` | the battery voltage, raw |

Buttons (ids 0..13):

| id | | id | | id | |
| --- | --- | --- | --- | --- | --- |
| 0 | `BTN_OCT_DOWN` | 5 | `BTN_LFO` | 10 | `BTN_ARP` |
| 1 | `BTN_OCT_UP` | 6 | `BTN_EDIT` | 11 | `BTN_SEQ` |
| 2 | `BTN_FX` | 7 | `BTN_GLO` | 12 | `BTN_PLAY` |
| 3 | `BTN_SEL` | 8 | `BTN_HOME` | 13 | `BTN_REC` |
| 4 | `BTN_ENV` | 9 | `BTN_SAVE` | | |

`BTN_OCT_DOWN` held at power-on is the USB rescue; your app sees it like any button otherwise.

Encoders: `ENC_SELECT` (0), `ENC_ALGORITHM` (1), `ENC_KNOB1` .. `ENC_KNOB4` (2..5), `ENC_PRESETS` (6).

A key down and up, the usual way:

```c
uint32_t keys = fm1_in.notes, down = keys & ~prev, up = prev & ~keys;
prev = keys;
```

## LEDs

| | |
| --- | --- |
| `fm1_led_key(id, on)` | a button (`BTN_...`) or a key (`KEY_LED(k)`, k = 0..26) lit or not |
| `fm1_led_key_glow(id, on)` | the same id in the glow layer; an LED in several layers takes the brightest, so a glow under a lit LED shows only once the lit layer goes out — which is how a key pulses without going dark |
| `fm1_led[col]`, `fm1_led_dim[col]` | the raw layers, for many LEDs at once (see `hal/fm1_input.h`); `_dim` glows at ~1/24 |

## Screen

240 x 240 pixels, 16-bit colour (`RGB(r, g, b)` with 0..255 each; `C_BLACK`, `C_WHITE`). The screen keeps
what you draw: draw only what changed.

Straight to the screen:

| | |
| --- | --- |
| `lcd_fill(x, y, w, h, colour)` | a rectangle |
| `draw_text_box(x, y, w, font, "text", colour, align)` | text in a box of width `w`, cleared to black first; align 0 left, 1 centre, 2 right |

Fonts: `FONT_S` (16 px, ASCII 32..126) and `FONT_L` (32 px, ASCII 32..95: capitals, digits, signs; small
letters show as `?`).

For anything more, a canvas: draw into a buffer, then send it in one go (no flicker):

```c
cv_begin(240, 56, C_BLACK);                /* w x h, cleared; w * h <= CV_MAX (240 x 136) */
cv_rect(x, y, w, h, colour);
cv_line(x0, y0, x1, y1, colour);
cv_pset(x, y, colour);
cv_text(x, y, &FONT_S, "text", colour);     /* returns x after the text; text_w(&FONT_S, "text") measures */
cv_blit(0, 140);                            /* to the screen at (0, 140) */
```

A bigger canvas: raise `CV_MAX` in platform.c **and in tests/host.c** (2 bytes a pixel, in the pool: 240 x 240
is 113 KB of 336). `cv_begin` clamps a canvas that will not fit rather than failing, so a mismatch between the
two shows up as a stale strip on the screen instead of an error; fmlab.c carries a compile-time check for it.

## Sound

```c
static void app_audio(int32_t *out, uint32_t n)
```

- Runs in the audio interrupt, every 5.8 ms, with `n` = 256 frames at **44.1 kHz** (`FS`).
- `out` is `L R L R ...`, **already zero**: add your sound to it. Full scale is **±8388607** (Q23: 24 bits,
  Q15 << 8); more is clipped. Keep the low bits: a soft note sits far under full scale, and its tail lives in them.
- After you: the MASTER knob, the clip, the DAC (all 24 bits), and the USB audio stream (16-bit, dithered:
  the computer records what the speaker plays). The DAC gets it 6 dB under its full scale, so the FM-1's
  output stage does not distort.
- Keep it fast and simple: no flash, no screen, no waiting. Talk to the main loop through `volatile` variables
  (one writer each). If a render runs late the sound breaks up; see [RULES.md](RULES.md).

Pitch: a 32-bit phase that wraps is an oscillator with no division at run time: `inc = f * 2^32 / 44100`,
`ph += inc` each frame. `dx7_core.c` does this per operator, from the tables in `dx7_tables.h`.

## MIDI

USB-MIDI, class compliant, cable 0.

| | |
| --- | --- |
| `midi_get(&status, &d1, &d2)` | 1 and the next message received, or 0. Call it from the main loop until 0 |
| `midi_send(status, d1, d2)` | send one message (from the main loop only) |

`status` includes the channel: `0x90` note on channel 1, `0x93` channel 4; a note on with velocity 0 is a
note off.

```c
static int  app_sysex(const uint8_t *p, uint32_t n);   /* non-zero = it was mine */
static void sysex_send(const uint8_t *p, uint32_t n);  /* F0..F7, sent whole */
```

A SysEx frame is offered to `app_sysex` first, with `p`/`n` covering the bytes between `F0` and `F7`. Return
non-zero and it is consumed; return zero and it goes to the updater exactly as it did before, so a firmware
update still works while the app is listening. Frames longer than `sx_frame` (4160 bytes) are dropped, and
only one frame is held at a time: a second arriving before the main loop takes the first is lost.

## Settings

```c
static int settings_load(void *p, uint32_t n);         /* 0 = loaded; else *p is left alone */
static int settings_save(const void *p, uint32_t n);   /* 0 = saved */
```

Up to 256 bytes, kept with their length and a CRC32 in one 4 KiB flash sector (`SETTINGS_AT`, 0xFC000). A
broken save reads as "nothing saved": start from your defaults then.

**Fields may only be appended.** A store shorter than what you ask for is an older build of the same
settings, so it loads what it has and leaves the rest of your struct alone — adding a setting costs nothing
and keeps the octave and the current voice. A store *longer* than you ask for is a newer build, and is
refused. Reordering or removing a field needs a new `SETTINGS_MAGIC`.

**Save when nothing sounds.** Erasing the sector holds the interrupts off for tens of milliseconds: the sound
drops out. `fmlab.c` saves 2 s after the last change, once all voices are silent. Flash wears out after about
100,000 erases of a sector: never save in a loop.

## Memory and flash

| | | |
| --- | --- | --- |
| Code + constants | 568 KB | runs straight from flash (`const` tables cost no RAM) |
| RAM `.data` + `.bss` | 96 KB | your globals and statics |
| Pool | 336 KB | big buffers, zeroed at boot: `static int16_t delay[65536] __attribute__((section(".pool")));` |
| Stack | 24 KB | keep big arrays off it: `static` |

The build prints what you use and fails above the limits.

Flash (1 MiB) as FM Lab uses it: the app, the update staging area (0xE0000, the platform's), the settings
sector (0xFC000) and eight voice banks (0xFD000 for bank 0, then 0xF6000, 0xF8000 and 0xFA000, then
0xEE000, 0xF0000, 0xF2000 and 0xF4000, two sectors each, via `bank_read(b, p, n)`/`bank_write(b, p, n)` — 4096 packed bytes with a length-and-CRC header, too
big for one sector). The other firmwares of the family keep their data in 0x97000..0xFBFFF; FM Lab leaves
all of it below 0xEE000 alone, and the stock firmware's main store ends at 0xE0000, so putting it back
still finds its samples. A new firmware simply finds its own magic missing and starts fresh. To store more,
widen `FL_RANGE_OK` in platform.c to the range you pick: it is the guard that keeps a bug from erasing
anything else.

## Build options

In platform.c, before `#include "usb.c"`:

| | | |
| --- | --- | --- |
| `FELUCCA_UAC` | 1 | USB audio input (the output to the computer) |
| `FELUCCA_CDC` | 0 | a USB serial console for debugging (some hosts then hide the audio; see usb.c) |
| `FELUCCA_OTA` | 1 | the updater. **Leave it on**: without it nothing can install over your firmware but the rescue |
