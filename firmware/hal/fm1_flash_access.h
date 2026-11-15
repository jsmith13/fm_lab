/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* Shared by the RAM flash driver and the CPU1 worker. Include in the same
 * compilation unit as both: like the rest of this BSP, the state is static.
 * Only CPU0 may enter a flash transaction, with its interrupts masked.
 * In a build with the second core (FM1_CPU1), a running CPU1 must have acknowledged
 * that it is in its RAM-only idle loop. Unknown workers are refused; they are never
 * forcibly stopped mid-job. */
#pragma once
#include <stdint.h>
#include "fm1_cc.h"

/* These primitives may be supplied by a host harness before inclusion. */
#ifndef FM1_FLASH_ICFG_READ
FM1_INLINE uint32_t fm1_flash_icfg_read(void)
{
    uint32_t v;
    __asm__ volatile("%0 = icfg" : "=r"(v) :: "memory");
    return v;
}
#define FM1_FLASH_ICFG_READ() fm1_flash_icfg_read()
#endif
#ifndef FM1_FLASH_ICFG_WRITE
FM1_INLINE void fm1_flash_icfg_write(uint32_t v)
{
    __asm__ volatile("icfg = %0" :: "r"(v) : "memory");
}
#define FM1_FLASH_ICFG_WRITE(v) fm1_flash_icfg_write(v)
#endif
#ifndef FM1_FLASH_SYNC
#define FM1_FLASH_SYNC() __asm__ volatile("csync" ::: "memory")
#endif
#ifndef FM1_FLASH_CORE
FM1_INLINE uint32_t fm1_flash_core(void)
{
    uint32_t v;
    __asm__ volatile("%0 = cnum" : "=r"(v));
    return v;
}
#define FM1_FLASH_CORE() fm1_flash_core()
#endif
#ifndef FM1_FLASH_C1_CON
#define FM1_FLASH_C1_CON (*(volatile uint32_t *)0x1EEE004u)
#endif

static volatile uint32_t fm1_flash_cpu1_idle;
#if FM1_CPU1                                    /* what the gate below was built with (fm1_cpu1.h checks it) */
#define FM1_FLASH_GATE_CPU1 1
#else
#define FM1_FLASH_GATE_CPU1 0
#endif

FM1_INLINE uint32_t irq_save(void)
{
    uint32_t f = FM1_FLASH_ICFG_READ();
    /* ICFG bit 8 is the global IRQ enable the AC79 SDK's local_irq_save clears; it leaves priority-7 sources
     * running (fm1_irq_attach takes 0..7; fm1-base gives a vendor library's interrupts up to 7), and their
     * handlers may run from XIP, which the flash driver switches off. Bit 9 is the one cli clears (what this
     * irq_save ran before): clear both, so everything is held off as before, and give the caller's ICFG back on
     * restore (it nests). Preserve all other bits, including the current interrupt context. */
    FM1_FLASH_ICFG_WRITE(f & ~0x300u);
    FM1_FLASH_SYNC();
    return f;
}

FM1_INLINE void irq_restore(uint32_t f)
{
    FM1_FLASH_SYNC();
    FM1_FLASH_ICFG_WRITE(f);
}

/* CPU0, with its interrupts off: ICFG bit 8 (irq_save, the SDK's local_irq_save) or bit 9 (cli) clear, as the SDK's
 * cpu_irq_disabled counts them ((icfg & 0x300) != 0x300). The second core is checked only in a build that has it
 * (FM1_CPU1, defined before the first BSP header): one without it never starts CPU1, so it neither holds CPU1 at its
 * start nor depends on that; a CPU1 running there is not this build's (as before this gate). */
FM1_INLINE int fm1_flash_access_ok(void)
{
    if (FM1_FLASH_CORE() != 0 || (FM1_FLASH_ICFG_READ() & 0x300u) == 0x300u)
        return 0;
#if FM1_FLASH_GATE_CPU1
    {
        uint32_t c;
        FM1_FLASH_SYNC();
        c = FM1_FLASH_C1_CON;
        return !(c & 8u) || (c & 2u) || fm1_flash_cpu1_idle == 1u;
    }
#else
    return 1;
#endif
}
