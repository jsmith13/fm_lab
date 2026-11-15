# Licensing

FM Lab's CPU1 support adapts the MPL-2.0 worker, entry/stop, flash gate and exception-frame
restoration from [felucca-bsp](https://github.com/hugelton/felucca-bsp), pinned revision
`18eadadb8c364abd026accd2b1ea1c14ca347f06`. It reuses the guarded adaptations tested in WT Lab.
Their attribution headers and file-level MPL obligations are retained; `LICENSE-MPL-2.0`
contains the complete license. The new FM staging layer and tests are GPL-3.0-only.

FM Lab is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them
its complete source under the same licence too.

One file carries a second licence as well: `firmware/src/dx7_core.c` is
`SPDX-License-Identifier: Apache-2.0 AND GPL-3.0-only`. Apache-2.0 is compatible with GPL-3.0, so the firmware
as a whole is GPL-3.0-only.

## Who made what

| | |
| --- | --- |
| The platform: `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `midi_uart.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| `firmware/src/platform.c`, the host mode `tests/host.c`, `docs/` | zvenson ([Dinghy](https://github.com/zvenson/dinghy)) |
| The FM engine `firmware/src/dx7_core.c` and its tables `dx7_tables.h` | see below; carried here from [dxsloop](https://github.com/zvenson/dxsloop) (zvenson) |
| The 20 factory voices, `tools/gen_voices.py` and the `firmware/src/dx7_voices.h` it generates | Copyright (C) 2026 Sven Trogus, designed for sloopDX ([dxsloop](https://github.com/zvenson/dxsloop) `tools/gen_dx7_bank.py`); FM Lab keeps the voices as they are and drops the drum kit |
| The instrument `firmware/src/fmlab.c`, `dx7_voice.h`, `tools/load_test.py`, `tools/docshots.py`, `assets/logo.svg`, the test scripts and checks in `tests/`, this documentation | FM Lab |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| DX7 core: the msfa engine of Dexed, Copyright 2012 Google Inc., 2016-2025 Pascal Gauthier, ported to integer C by Sven Trogus (the port is GPL-3.0-only, Copyright (C) 2026 Sven Trogus); the tables are computed on the host from msfa's expressions | Apache-2.0 (msfa) AND GPL-3.0-only (the port) | `firmware/src/dx7_core.c`, `firmware/src/dx7_tables.h`; <https://github.com/asb2m10/dexed> |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## Voice data

**No Yamaha and no M-VAVE voice data is in this tree, and none should be added.** The factory voices are
Sven Trogus's, designed from scratch for sloopDX and written out in `tools/gen_voices.py`, which is the readable
source for `firmware/src/dx7_voices.h`; both are GPL-3.0-only like the rest of the code. `.syx` banks you import
stay on your device and are yours, not part of this repository.

This rule is inherited from dxsloop and is not a formality: a DX7 voice is 156 bytes of someone's authorship,
and the factory banks are Yamaha's.

## The logo

`assets/logo.svg` / `logo.png` are FM Lab's own: a modulator feeding a carrier, over a plot of
`sin(4t + 1.2 sin t)`. They are GPL-3.0-only like the rest of the tree.

Dinghy's logo carries the designburg signet, which is *not* under the GPL and marks Dinghy itself. It is not
in this repository, and must not be reintroduced. If you fork FM Lab, the same courtesy applies in reverse:
make your own.

## Trademarks

"DX7" is a trademark of Yamaha Corporation. FM Lab is an independent implementation of the DX7's synthesis (by
way of Dexed) and contains no Yamaha voice data, firmware or ROM content. It is not affiliated with, endorsed
by or supported by Yamaha.

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE" and "FM-1" belong to their
owners. FM Lab is not made or endorsed by any of them.
