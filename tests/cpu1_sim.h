/* SPDX-License-Identifier: GPL-3.0-only */
/* Ordered UI/audio simulation only; cpu1_host.c provides real threaded ownership tests. */
#define FM1_CPU1_TIMEOUT_US 600u
static uint32_t fm1_cpu1_on = 1, fm1_cpu1_gave_up;
static int fm1_cpu1_run(void (*fn)(uint32_t), uint32_t arg)
{
    if (!fm1_cpu1_on) return 0;
    fn(arg);
    return 1;
}
static int fm1_cpu1_wait_until(uint32_t deadline) { (void)deadline; return 0; }
static void fm1_cpu1_check(void) {}
