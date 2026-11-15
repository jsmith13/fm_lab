/* SPDX-License-Identifier: GPL-3.0-only */
/* Extract this independent serial oracle from Git tag v1.3.3 before compiling. */
#include <stdint.h>
#define DX_LG_N 6
#include "dx7_voice.h"
#include "../build/cpu1-tests/serial-reference/dx7_core.c"

void fm_cpu1_reference_compute(void *state, int32_t *out, int32_t pitch, uint32_t os, uint32_t peg_off)
{
    dx_os = (uint8_t)os; dx_peg_off = (uint8_t)peg_off;
    dx_compute(state,out,pitch);
}
