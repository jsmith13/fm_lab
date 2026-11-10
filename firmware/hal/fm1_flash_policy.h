/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

#define FL_ERASE_TIMEOUT_US 2000000u
#define FL_PROGRAM_TIMEOUT_US 50000u

RAMINL int fl_jedec_ok(uint32_t id)
{
    return id == 0x856014u || id == 0xB36014u;    /* Puya P25Q80H / UCUN UC25HQ80, both 1 MiB */
}

/* The driver and host model supply RAMINL, fl_rdsr, T4_CNT (24 MHz) and the
 * non-returning fl_reset_busy. issued includes command and cache-flush time. */
RAMINL int fl_wait_ready(uint32_t issued, uint32_t max_us, uint32_t *took_us)
{
    for (;;) {
        uint8_t busy = fl_rdsr(0x05) & 1u;
        *took_us = (T4_CNT - issued) / 24u;
        if (!busy)
            return 0;
        if (*took_us >= max_us)
            fl_reset_busy();                   /* never resume XIP on a busy part */
    }
}
