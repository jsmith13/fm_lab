/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdint.h>
#include <stdio.h>
#define FM1_CPU1 1
static uint32_t cfg = 0x300u, core, c1;
#define FM1_FLASH_ICFG_READ() cfg
#define FM1_FLASH_ICFG_WRITE(v) (cfg = (v))
#define FM1_FLASH_SYNC() ((void)0)
#define FM1_FLASH_CORE() core
#define FM1_FLASH_C1_CON c1
#include "fm1_flash_access.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"flash gate line %d\n",__LINE__); return 1; } } while (0)
int main(void)
{
    uint32_t old, nested;
    CHECK(!fm1_flash_access_ok());
    old = irq_save(); CHECK(cfg == 0 && fm1_flash_access_ok());
    nested = irq_save(); irq_restore(nested); CHECK(cfg == 0);
    c1 = 8; CHECK(!fm1_flash_access_ok());
    fm1_flash_cpu1_idle = 1; CHECK(fm1_flash_access_ok());
    fm1_flash_cpu1_idle = 0; CHECK(!fm1_flash_access_ok());
    c1 = 10; CHECK(fm1_flash_access_ok());
    core = 1; CHECK(!fm1_flash_access_ok());
    irq_restore(old); CHECK(cfg == 0x300u);
    puts("WT CPU1 flash gate: busy/idle/held/core/IRQ/nesting passed");
    return 0;
}
