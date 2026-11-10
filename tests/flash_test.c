/* SPDX-License-Identifier: GPL-3.0-only */
/* cc -std=c99 -O2 -Wall -Wextra -Werror tests/flash_test.c -o build/flash_test */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t ticks, issued, busy_us, poll_us, reads, resets;
static int stuck;
static jmp_buf reset_target;

static uint8_t fl_rdsr(uint8_t cmd)
{
    assert(cmd == 0x05);
    ticks += poll_us * 24u;
    reads++;
    return stuck || (uint32_t)(ticks - issued) / 24u < busy_us ? 1u : 0u;
}

static void fl_reset_busy(void)
{
    resets++;
    longjmp(reset_target, 1);
}

#define RAMINL static inline
#define T4_CNT ticks
#include "../firmware/hal/fm1_flash_policy.h"

static void model(uint32_t start, uint32_t busy, uint32_t poll)
{
    ticks = issued = start;
    busy_us = busy;
    poll_us = poll;
    reads = resets = 0;
    stuck = 0;
}

static void completes(uint32_t busy, uint32_t limit, uint32_t start, uint32_t before_wait)
{
    uint32_t took = 0;
    model(start, busy, 100u);
    ticks += before_wait * 24u;
    assert(fl_wait_ready(issued, limit, &took) == 0);
    assert(took == busy);
    assert(reads && !resets);
}

static void times_out(uint32_t limit, uint32_t start)
{
    uint32_t took = 0;
    model(start, 0, 100u);
    stuck = 1;
    if (setjmp(reset_target) == 0) {
        fl_wait_ready(issued, limit, &took);
        assert(!"busy flash returned to its caller");
    }
    assert(resets == 1u);
    assert((uint32_t)(ticks - issued) / 24u == limit);
}

int main(void)
{
    assert(fl_jedec_ok(0x856014u));
    assert(fl_jedec_ok(0xB36014u));
    assert(!fl_jedec_ok(0u));
    assert(!fl_jedec_ok(0xFFFFFFu));
    assert(!fl_jedec_ok(0x856015u));
    assert(!fl_jedec_ok(0xB36015u));
    assert(!fl_jedec_ok(0xEF4014u));

    completes(18000u, FL_ERASE_TIMEOUT_US, 0u, 0u);
    completes(1000u, FL_PROGRAM_TIMEOUT_US, 0u, 0u);
    completes(750000u, FL_ERASE_TIMEOUT_US, 0u, 0u);
    completes(20000u, FL_PROGRAM_TIMEOUT_US, 0u, 0u);
    completes(FL_ERASE_TIMEOUT_US, FL_ERASE_TIMEOUT_US, 0u, 0u);
    completes(FL_PROGRAM_TIMEOUT_US, FL_PROGRAM_TIMEOUT_US, 0u, 0u);
    completes(750000u, FL_ERASE_TIMEOUT_US, UINT32_MAX - 1000u, 600000u);
    completes(20000u, FL_PROGRAM_TIMEOUT_US, UINT32_MAX - 1000u, 15000u);
    times_out(FL_ERASE_TIMEOUT_US, 0u);
    times_out(FL_PROGRAM_TIMEOUT_US, UINT32_MAX - 1000u);
    puts("flash: known chips only; normal, slow, boundary and wrapped waits; stuck operations reset");
    return 0;
}
