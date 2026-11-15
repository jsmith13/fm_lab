/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio): from Melodee (github.com/keremimo/melodee): the start that works
 *   on the device, core 1's guards, the worker with its check word and its retirement, the bar, the fault path
 *   (fm1_multicore.h, audio_worker.h, fm1_core1_bar.h, fm1_irq.h), under MPL-2.0 by agreement
 *   (https://github.com/hugelton/felucca-bsp/issues/5)
 * Copyright (C) 2026 Charles Vestal (the PC limits around the start, the stack mark, the bar per build, the API)
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (the first start, from the SDK)
 * Written against the public JieLi AC79 SDK (Apache-2.0); see NOTICE. */
/* FM-1 second core (CPU1): a worker that runs one job at a time for CPU0, usually a part of the audio render.
 *
 * Start (the SDK's EnableOtherCpu, as Melodee runs it): CPU1 held (fm1_cpu1_stop), its interrupt bank off
 * (0x1EEF300..), its entry address into the word at 0x01C7FFF8 (the protected top of RAM: unlocked for it), bit 3
 * of SYS_DIV (0x10008) set while it starts, C1_CON (0x1EEE004) bit 3 set and bit 1 cleared. It boots through the
 * chip's ROM, which the PC limits refuse (DBG bit 10, c1_pc_limit_err), so they are open until it has reported in.
 * Start it at power-on, before the audio and the timers run (Melodee's and X0X's finding: that is the start that
 * works), with interrupts off.
 *
 * Work: CPU0 hands a job over (fm1_cpu1_run: a function and an argument), does its own share, then waits for it
 * (fm1_cpu1_wait). One job at a time, a single writer for each word, csync around each (the cores share the
 * caches): CPU0's words, then CPU1's on a cache line of their own. CPU1's loop is in RAM (.c1_text, copied at
 * boot), so an idle CPU1 never fetches from the flash; the job itself runs from the flash, so CPU0 turns the flash
 * off only with no job outstanding. The RAM loop acknowledges idle to the flash driver; dispatch revokes
 * that permission before publishing a job. Flash entry points refuse a busy or unknown CPU1.
 *
 * Units that cannot run it (keremimo/melodee#17, charlesvestal/fm1-x0x#10; measured by Jangada,
 * github.com/zednaked/jangada): on some FM-1s CPU1 now and then reads shared RAM wrong (0x00200000 where it holds
 * 0), more often under load. So CPU1 runs only the next job, and only when its check word (the function, the
 * argument and the job's number, written before the job) matches; the jobs it finished stay in a register. A
 * misread, a fault on CPU1, a job not back in FM1_CPU1_TIMEOUT_US, or a start it does not answer is a failure:
 *   FM1_CPU1_FALLBACK 0 (the default; Melodee's releases): no fallback to one core. CPU1 is held, the failure goes
 *     in the crash record (fm1_crash: vec 0x100 + FM1_CPU1_*, pc the faulting pc or the misread job, dbg DBG_MSG,
 *     sp the misreads), and the FM-1 restarts; the boot guard (two failed starts: safe mode) covers a unit that cannot run it
 *   FM1_CPU1_FALLBACK 1: CPU1 is retired: held, and CPU0 does the work alone (the job is dropped, never run twice).
 *     Retired, or CPU0 crashed while CPU1 ran (its data can go bad too, where no check reaches): barred, one core
 *     at every start until power-off (fm1_cpu1_bar, .noinit; nothing written to the flash on a unit that may be
 *     unstable). fm1_cpu1_bar_build gives a new build two cores again.
 *
 *   fm1_cpu1_start()        0 = it runs and answered; -1 = held (barred, or it did not report in: FALLBACK only)
 *   fm1_cpu1_run(fn, arg)   1 = it has fn(arg); 0 = do it yourself (off, or a job outstanding)
 *   fm1_cpu1_wait()         0 = done; -1 = retired, the job dropped (do it yourself; FM1_CPU1_FALLBACK only)
 *   fm1_cpu1_busy()         1 = the job handed over is not done yet (0 after a misread or a fault: wait)
 *   fm1_cpu1_check()        retire it if it misread or faulted while idle (run does it too)
 *   fm1_cpu1_stop()         hold it (fm1_cpu1_stop.h: registers only)
 *   fm1_cpu1_sync()         a barrier between the cores' reads and writes (a job's own sharing)
 *   fm1_cpu1_on, fm1_cpu1_gave_up (FM1_CPU1_*), fm1_cpu1_stack_used(), fm1_cpu1_barred(), fm1_cpu1_bar_build()
 *
 * The build defines FM1_CPU1=1 (fm1_irq.h then hands CPU1's faults here), assembles fm1_cpu1.S (the entry and the
 * stacks), and links .c1_text into the RAM code (.ram_text, between _c1_start and _c1_end). */
#pragma once
#include <stdint.h>
#include "fm1_cc.h"
#if defined(FM1_FLASH_GATE_CPU1) && !FM1_FLASH_GATE_CPU1
#error "fm1_cpu1.h: the flash gate (fm1_flash_access.h) was built without FM1_CPU1: define FM1_CPU1 1 before the first BSP header"
#endif
#ifndef FM1_CPU1
#define FM1_CPU1 1                                /* (fm1_irq.h and the flash gate check it) */
#endif
#include "fm1_time.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_cpu1_stop.h"
#include "fm1_flash_access.h"
#include "fm1_sys.h"

#ifndef FM1_CPU1_FALLBACK
#define FM1_CPU1_FALLBACK 0                     /* 1: one core after a failure, instead of a restart */
#endif
#ifndef FM1_CPU1_TIMEOUT_US
#define FM1_CPU1_TIMEOUT_US 8000u               /* a job not back by then: far past any audio block, or it faulted */
#endif
#define FM1_CPU1_SYNC() __asm__ volatile("csync" ::: "memory")
#define FM1_C1_CON (*(volatile uint32_t *)0x1EEE004u)
#define FM1_C1_SYS_DIV (*(volatile uint32_t *)0x10008u)
#define FM1_C1_ENTRY_WORD (*(volatile uint32_t *)0x01C7FFF8u)
#define FM1_C1_ICFG(i) (*(volatile uint32_t *)(0x1EEF300u + 4u * (i)))   /* CPU1's interrupt bank (CPU0's: 0x1EEF100) */
#define FM1_C1_EMU_CON (*(volatile uint32_t *)0x1EEF2D0u)                 /* CPU1's EMU bank (CPU0's: 0x1EEF0D0) */
#define FM1_C1_EMU_MSG (*(volatile uint32_t *)0x1EEF2D4u)

enum { FM1_CPU1_MISREAD = 1, FM1_CPU1_LATE = 2, FM1_CPU1_BARRED = 3, FM1_CPU1_FAULT = 4,
       FM1_CPU1_SILENT = 5, FM1_CPU1_DEADLINE = 7 };

extern void fm1_cpu1_entry(void);                       /* fm1_cpu1.S */
extern uint32_t fm1_cpu1_ustack[], fm1_cpu1_sstack_top[];
#ifndef FM1_CPU1_USTACK_BYTES
#define FM1_CPU1_USTACK_BYTES 2816                      /* the jobs' stack, bytes (fm1_cpu1.S reads the same macro): a build may
                                                         * set more for deeper jobs, a multiple of 8, the lowest 256 B a guard */
#endif
#define FM1_CPU1_USTACK_WORDS (FM1_CPU1_USTACK_BYTES / 4u)   /* (fm1_cpu1.S) */
_Static_assert(FM1_CPU1_USTACK_BYTES % 8 == 0 && FM1_CPU1_USTACK_BYTES > 256,
               "FM1_CPU1_USTACK_BYTES: a multiple of 8, above the 256 B guard");
#define FM1_CPU1_MARK 0x43314D4Bu                       /* "C1MK": how deep its stack has gone */

typedef void (*fm1_cpu1_fn)(uint32_t);
/* CPU0's words, then CPU1's on a cache line of their own */
static struct {
    volatile uint32_t request;                  /* CPU0: the job's number (request + 1 hands fn(arg) over) */
    fm1_cpu1_fn volatile fn;
    volatile uint32_t arg, check;
    volatile uint32_t ready __attribute__((aligned(32)));   /* CPU1: it started */
    volatile uint32_t complete, rejected;       /* the last job it finished; the jobs it would not run */
    volatile uint32_t misread, jobs;            /* the last refused, as it read it; how many it ran */
} fm1_cpu1_mb __attribute__((aligned(32)));
#define FM1_CPU1_CHECK(fn, arg, request) ((uint32_t)(uintptr_t)(fn) ^ (arg) ^ (request) * 0x9E3779B1u)

static uint8_t fm1_cpu1_on;                     /* started and answering: CPU0 may hand it work */
static inline void fm1_cpu1_sync(void) { FM1_CPU1_SYNC(); }   /* a barrier between the cores (a job's own sharing) */
static uint8_t fm1_cpu1_gave_up;                /* why it is off (FM1_CPU1_*), 0 = it is not */
static uint32_t fm1_cpu1_requested_at;
static uint32_t fm1_cpu1_late_age;
static int32_t fm1_cpu1_late_left;

/* its faults (fm1_cpu1_fault_c): CPU0 reads count */
static struct { volatile uint32_t count; uint32_t echoes, cpu, dbg, emu, pc, rets; } fm1_cpu1_fault;

/* one core until power-off. Not static, as fm1_crash: the compiler must not assume a zero start */
#define FM1_CPU1_BAR_MAGIC 0x52414231u          /* "1BAR" */
struct { uint32_t magic, barred, build; } fm1_cpu1_bar __attribute__((section(".noinit")));
static inline int fm1_cpu1_barred(void)
{
    return FM1_CPU1_FALLBACK && fm1_cpu1_bar.magic == FM1_CPU1_BAR_MAGIC && fm1_cpu1_bar.barred == 1u;
}
static inline void fm1_cpu1_bar_set(void)
{
    fm1_cpu1_bar.magic = FM1_CPU1_BAR_MAGIC;
    fm1_cpu1_bar.barred = 1;
}
/* at boot: a bar set by another build (another id) is lifted, as it was for that build's sake */
static void fm1_cpu1_bar_build(uint32_t build)
{
    if (fm1_cpu1_bar.magic != FM1_CPU1_BAR_MAGIC || fm1_cpu1_bar.build != build)
        fm1_cpu1_bar.barred = 0;
    fm1_cpu1_bar.magic = FM1_CPU1_BAR_MAGIC;
    fm1_cpu1_bar.build = build;
}

/* CPU1's loop (in RAM): its guards, then one look at the mailbox after another. Only request, fn, arg and check
 * come from the RAM; a misread of any of them fails the check */
void fm1_cpu1_main(void);
void __attribute__((section(".c1_text"), noreturn, used)) fm1_cpu1_main(void)
{
    /* the stack limit at CPU1's EMU bank, over both its stacks less the lowest 256 bytes (a stack that runs
     * down traps before it reaches what lies below), and EMU_CON bit 2 and bits 16..20 off, bit 3 on */
    uint32_t lo = (uint32_t)(uintptr_t)fm1_cpu1_ustack + 256u, hi = (uint32_t)(uintptr_t)fm1_cpu1_sstack_top - 1u;
    uint32_t done;
    *(volatile uint32_t *)0x1EEF2D8u = hi;
    *(volatile uint32_t *)0x1EEF2DCu = lo;
    *(volatile uint32_t *)0x1EEF2E0u = hi;
    *(volatile uint32_t *)0x1EEF2E4u = lo;
    FM1_C1_EMU_CON = (FM1_C1_EMU_CON & ~((1u << 2) | (0x1Fu << 16))) | (1u << 3);
    done = fm1_cpu1_mb.complete;
    FM1_CPU1_SYNC();
    fm1_flash_cpu1_idle = 1;
    FM1_CPU1_SYNC();
    fm1_cpu1_mb.ready = 1;
    FM1_CPU1_SYNC();
    for (;;) {
        uint32_t request = fm1_cpu1_mb.request;
        FM1_CPU1_SYNC();
        if (request == done)
            continue;
        if (request == done + 1u) {
            fm1_cpu1_fn fn = fm1_cpu1_mb.fn;
            uint32_t arg = fm1_cpu1_mb.arg;
            if (fm1_cpu1_mb.check == FM1_CPU1_CHECK(fn, arg, request)) {
                fn(arg);                        /* the work itself runs from the flash */
                fm1_cpu1_mb.jobs++;
                done = request;
                FM1_CPU1_SYNC();
                fm1_flash_cpu1_idle = 1;
                FM1_CPU1_SYNC();
                fm1_cpu1_mb.complete = request;
                FM1_CPU1_SYNC();
                continue;
            }
        }
        fm1_cpu1_mb.misread = request;
        FM1_CPU1_SYNC();
        fm1_cpu1_mb.rejected++;
        FM1_CPU1_SYNC();
    }
}

/* CPU0, no fallback: hold it, the failure into the crash record (as fm1_fault_c writes it), and restart */
static void __attribute__((noreturn)) fm1_cpu1_failure(uint32_t why, uint32_t pc, uint32_t dbg)
{
    uint32_t count = fm1_crash.magic == FM1_CRASH_MAGIC ? fm1_crash.count : 0;
    __asm__ volatile("cli" ::: "memory");
    fm1_cpu1_stop();
    fm1_crash.magic = FM1_CRASH_MAGIC;
    fm1_crash.count = count + 1u;
    fm1_crash.vec = 0x100u + why;
    fm1_crash.pc = pc;
    fm1_crash.rets = fm1_cpu1_fault.rets;
    fm1_crash.emu = fm1_cpu1_fault.emu;
    fm1_crash.dbg = dbg;
    fm1_crash.sp = fm1_cpu1_mb.rejected;
    fm1_crash.uptime_ms = fm1_uptime_ms();
    fm1_reboot();
    for (;;)
        ;
}

/* CPU0: a failure. Without the fallback, a restart; with it, held for good (until power-off) and CPU0 works alone
 * (a job it may have been running is dropped) */
static void fm1_cpu1_retire(uint32_t why)
{
    if (!FM1_CPU1_FALLBACK)
        fm1_cpu1_failure(why, why == FM1_CPU1_FAULT ? fm1_cpu1_fault.pc : fm1_cpu1_mb.misread,
                         why == FM1_CPU1_FAULT ? fm1_cpu1_fault.dbg : 0u);
    fm1_cpu1_stop();
    fm1_cpu1_bar_set();
    fm1_cpu1_on = 0;
    fm1_cpu1_gave_up = (uint8_t)why;
    fm1_cpu1_mb.complete = fm1_cpu1_mb.request;
    FM1_CPU1_SYNC();
}

static int fm1_cpu1_start(void)
{
    uint32_t div, t0, i, pc[4], top;
    if (fm1_cpu1_on)
        return 0;
    fm1_cpu1_stop();
    if (fm1_cpu1_barred()) {
        fm1_cpu1_gave_up = FM1_CPU1_BARRED;
        return -1;
    }
    for (i = 0; i < FM1_CPU1_USTACK_WORDS; i++)
        fm1_cpu1_ustack[i] = FM1_CPU1_MARK;
    fm1_flash_cpu1_idle = 0;
    fm1_cpu1_mb.ready = 0;
    fm1_cpu1_mb.request = 0;
    fm1_cpu1_mb.complete = 0;
    fm1_cpu1_mb.rejected = 0;
    fm1_cpu1_mb.fn = 0;
    fm1_cpu1_fault.count = fm1_cpu1_fault.echoes = 0;
    FM1_CPU1_SYNC();
    for (i = 0; i < 32u; i++)                   /* its interrupt bank: all off (it runs polled) */
        FM1_C1_ICFG(i) = 0;
    fm1__dbg_unlock();                          /* the PC limits open while it boots through the ROM, and the */
    pc[0] = FM1_PC_LIMIT0_L, pc[1] = FM1_PC_LIMIT0_H;     /* protected top of RAM for its entry word */
    pc[2] = FM1_PC_LIMIT1_L, pc[3] = FM1_PC_LIMIT1_H;
    FM1_PC_LIMIT0_L = 0u;
    FM1_PC_LIMIT0_H = 0xFFFFFFFFu;
    FM1_PC_LIMIT1_L = 0u;
    FM1_PC_LIMIT1_H = 0xFFFFFFFFu;
    top = FM1_C0_WR_LIMIT_EN & 4u;
    FM1_C0_WR_LIMIT_EN &= ~4u;
    fm1__dbg_lock();
    FM1_C1_ENTRY_WORD = (uint32_t)(uintptr_t)fm1_cpu1_entry;
    div = FM1_C1_SYS_DIV;
    FM1_C1_SYS_DIV = div | 8u;
    FM1_CPU1_SYNC();
    FM1_C1_CON |= 8u;
    FM1_C1_CON &= ~2u;
    t0 = fm1_ticks();
    while (!fm1_cpu1_mb.ready && (uint32_t)(fm1_ticks() - t0) < 20000u * FM1_TICKS_PER_US)   /* 20 ms */
        FM1_CPU1_SYNC();
    if (!fm1_cpu1_mb.ready)
        fm1_cpu1_stop();
    FM1_C1_SYS_DIV = div;
    fm1__dbg_unlock();
    FM1_DBG_MSG_CLR = 0xFFFFFFFFu;
    FM1_PC_LIMIT0_L = pc[0], FM1_PC_LIMIT0_H = pc[1];
    FM1_PC_LIMIT1_L = pc[2], FM1_PC_LIMIT1_H = pc[3];
    FM1_C0_WR_LIMIT_EN |= top;
    fm1__dbg_lock();
    if (!fm1_cpu1_mb.ready) {
        if (!FM1_CPU1_FALLBACK)
            fm1_cpu1_failure(FM1_CPU1_SILENT, 0u, 0u);
        fm1_cpu1_gave_up = FM1_CPU1_SILENT;
        fm1_cpu1_bar_set();
        return -1;
    }
    fm1_cpu1_on = 1;
    fm1_cpu1_gave_up = 0;
    return 0;
}

static void fm1_cpu1_check(void)
{
    FM1_CPU1_SYNC();
    if (!fm1_cpu1_on)
        return;
    if (fm1_cpu1_fault.count)
        fm1_cpu1_retire(FM1_CPU1_FAULT);
    else if (fm1_cpu1_mb.rejected)
        fm1_cpu1_retire(FM1_CPU1_MISREAD);
}

static inline int fm1_cpu1_run(fm1_cpu1_fn fn, uint32_t arg)
{
    uint32_t request;
    fm1_cpu1_check();
    if (!fm1_cpu1_on || (request = fm1_cpu1_mb.request) != fm1_cpu1_mb.complete)
        return 0;
    request++;
    fm1_flash_cpu1_idle = 0;
    FM1_CPU1_SYNC();
    fm1_cpu1_mb.fn = fn;
    fm1_cpu1_mb.arg = arg;
    fm1_cpu1_mb.check = FM1_CPU1_CHECK(fn, arg, request);
    fm1_cpu1_requested_at = fm1_ticks();
    FM1_CPU1_SYNC();
    fm1_cpu1_mb.request = request;
    FM1_CPU1_SYNC();
    return 1;
}

static inline int fm1_cpu1_busy(void)            /* (a misread or a fault: not busy; fm1_cpu1_wait retires it) */
{
    FM1_CPU1_SYNC();
    return fm1_cpu1_on && fm1_cpu1_mb.complete != fm1_cpu1_mb.request && !fm1_cpu1_mb.rejected && !fm1_cpu1_fault.count;
}

static int fm1_cpu1_wait_until(uint32_t deadline)
{
    for (;;) {
        FM1_CPU1_SYNC();
        if (!fm1_cpu1_on)
            return -1;
        if (fm1_cpu1_fault.count) {
            fm1_cpu1_retire(FM1_CPU1_FAULT);
            return -1;
        }
        if (fm1_cpu1_mb.rejected) {
            fm1_cpu1_retire(FM1_CPU1_MISREAD);
            return -1;
        }
        if (fm1_cpu1_mb.complete == fm1_cpu1_mb.request)
            return 0;
        uint32_t now = fm1_ticks();
        uint32_t age = now - fm1_cpu1_requested_at;
        int32_t left = (int32_t)(deadline - now);
        if (age >= FM1_CPU1_TIMEOUT_US * FM1_TICKS_PER_US || left <= 0) {
            fm1_cpu1_late_age = age;
            fm1_cpu1_late_left = left;
            fm1_cpu1_retire(age >= FM1_CPU1_TIMEOUT_US * FM1_TICKS_PER_US ?
                            FM1_CPU1_LATE : FM1_CPU1_DEADLINE);
            return -1;
        }
    }
}

static int fm1_cpu1_wait(void)
{
    return fm1_cpu1_wait_until(fm1_cpu1_requested_at + FM1_CPU1_TIMEOUT_US * FM1_TICKS_PER_US);
}

/* how deep its stack has gone, in bytes (the mark left untouched since the start) */
static uint32_t fm1_cpu1_stack_used(void)
{
    uint32_t i = 0;
    while (i < FM1_CPU1_USTACK_WORDS && fm1_cpu1_ustack[i] == FM1_CPU1_MARK)
        i++;
    return (FM1_CPU1_USTACK_WORDS - i) * 4u;
}

/* fm1_fault_c (fm1_irq.h), first: 1 = a fault of CPU1's, handled, CPU0 resumes; 0 = CPU0's own (the crash report).
 * CPU1 enables no interrupt, so the debug unit's errors about it (a fetch outside the PC limits, a bus error) raise
 * the exception on CPU0, whose own state is intact: CPU1 is held, the fault recorded, and CPU0 retires it at its
 * next look (fm1_cpu1_wait, _check) and goes on. CPU1 can also take the exception itself (a misread pointer it
 * called): it records it and parks until CPU0 holds it (the crash screen and fm1_reboot on CPU1 would hold CPU1
 * itself: a frozen FM-1). Only CPU0 can finish restoring the debug guards after a hold. Without the fallback, CPU0
 * restarts there (fm1_cpu1_failure). With it, CPU0's own crash beside a running CPU1 bars it */
static int fm1_cpu1_fault_c(const uint32_t *f)
{
    uint32_t cpu = fm1_cnum(), dbg = FM1_DBG_MSG, was;
    if (!cpu && (dbg & FM1_DBG_C0))
        goto own;
    /* CPU1 took it too and cleared DBG_MSG first. Once per CPU1 fault, so a genuine CPU0 exception with DBG_MSG
     * clear that lands in that window is resumed once; it faults again and is reported then */
    if (!cpu && !(dbg & FM1_DBG_C1)) {
        if (fm1_cpu1_fault.echoes >= fm1_cpu1_fault.count)
            goto own;
        fm1_cpu1_fault.echoes++;
        return 1;
    }
    if (!cpu && fm1_cpu1_fault.count >= 16u)
        goto own;                               /* a hold that did not take */
    if (!cpu)
        fm1_cpu1_stop();
    fm1_cpu1_fault.cpu = cpu;
    fm1_cpu1_fault.dbg = dbg;
    fm1_cpu1_fault.emu = cpu ? FM1_C1_EMU_MSG : FM1_EMU_MSG;     /* the faulting core's EMU_MSG */
    fm1_cpu1_fault.pc = f[16];
    fm1_cpu1_fault.rets = f[25];
    fm1_cpu1_fault.count++;                     /* (before DBG_MSG clears: see the echo above) */
    was = FM1_DBG_WR_EN & 1u;                   /* (writing 0xE7 toggles the unlock) */
    if (!was)
        FM1_DBG_WR_EN = 0xE7u;
    FM1_DBG_MSG_CLR = dbg;
    if (!was)
        FM1_DBG_WR_EN = 0xE7u;
    if (cpu) {
        FM1_CPU1_SYNC();                        /* publish the fault; CPU0 sees it and holds us */
        for (;;)
            ;
    }
    if (!FM1_CPU1_FALLBACK)
        fm1_cpu1_failure(FM1_CPU1_FAULT, f[16], dbg);
    return 1;
own:
    if (FM1_CPU1_FALLBACK && fm1_cpu1_on)
        fm1_cpu1_bar_set();
    return 0;
}
