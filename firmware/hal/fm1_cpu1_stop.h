/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio): from Melodee (github.com/keremimo/melodee), fm1_core1_stop.h,
 *   under MPL-2.0 by agreement (https://github.com/hugelton/felucca-bsp/issues/5)
 * Portions Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments: Felucca 1.6.1's fm1_cpu1_halt
 *   (a53ecf4; https://github.com/hugelton/Felucca/pull/247) */
/* Hold the second core (CPU1), safely. Holding it can abort a bus access it has outstanding, and that raises a
 * delayed bus-invalid flag on CPU0: the bus traps are masked, CPU1 held (C1_CON 0x1EEE004: bit 1 set, then bit 3
 * cleared), the flag given time to arrive and cleared, then the debug enables and the write lock put back.
 * Registers, and the flash gate's idle word (fm1_flash_access.h: a held CPU1 is no longer idle in its RAM loop; a
 * word in .bss, so harmless before the RAM is set up): also first thing at a start (a reset that left CPU1
 * running), and on the way to a reset or an update. Harmless when CPU1 never ran. */
#pragma once
#include <stdint.h>
#include "fm1_cc.h"
#include "fm1_flash_access.h"

FM1_INLINE void fm1_cpu1_stop(void)
{
    uint32_t locked = !(*(volatile uint32_t *)0x1EEE240u & 1u), en, bus = 0x003F0030u;
    volatile uint32_t n;
    __asm__("" : "+r"(bus));                    /* (not folded into an address that looks like the chip ROM's) */
    if (locked)
        *(volatile uint32_t *)0x1EEE240u = 0xE7u;          /* DBG_WR_EN: writing 0xE7 toggles the unlock */
    en = *(volatile uint32_t *)0x1EEE340u;                 /* DBG_EN */
    *(volatile uint32_t *)0x1EEE340u = en & ~bus;
    __asm__ volatile("csync" ::: "memory");
    *(volatile uint32_t *)0x1EEE004u |= 2u;
    *(volatile uint32_t *)0x1EEE004u &= ~8u;
    __asm__ volatile("csync" ::: "memory");
    for (n = 0; n < 256u; n++)                  /* let the delayed bus flag arrive */
        ;
    *(volatile uint32_t *)0x1EEE248u = bus;                /* DBG_MSG_CLR */
    __asm__ volatile("csync" ::: "memory");
    *(volatile uint32_t *)0x1EEE340u = en;
    if (locked && (*(volatile uint32_t *)0x1EEE240u & 1u))
        *(volatile uint32_t *)0x1EEE240u = 0xE7u;
    fm1_flash_cpu1_idle = 0;
    __asm__ volatile("csync" ::: "memory");
}
