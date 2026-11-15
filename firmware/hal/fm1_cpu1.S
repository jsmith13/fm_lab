/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio): from Melodee (github.com/keremimo/melodee), crt0.S's CPU1 entry,
 *   under MPL-2.0 by agreement (https://github.com/hugelton/felucca-bsp/issues/5)
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Written against the public JieLi AC79 SDK (Apache-2.0); see NOTICE. */
/* Entry of the second core (CPU1), as the SDK's cpu.a/startup.S.o: interrupts off, no interrupt configuration, its
 * own user and supervisor stacks, then out of the start-up interrupt into fm1_cpu1_main (in RAM, fm1_cpu1.h), which
 * never returns. fm1_cpu1_start puts this address in the word the chip starts CPU1 from (0x01C7FFF8). */
#ifndef FM1_CPU1_USTACK_BYTES
#define FM1_CPU1_USTACK_BYTES 2816	/* the jobs' stack (fm1_cpu1.h reads the same macro); a multiple of 8 */
#endif
	.section .text.fm1_cpu1,"ax",@progbits
	.balign 4
	.globl fm1_cpu1_entry
fm1_cpu1_entry:
	cli
	icfg = 0
	usp = fm1_cpu1_ustack_top
	sp = fm1_cpu1_sstack_top
	ssp = fm1_cpu1_sstack_top
	reti = fm1_cpu1_main
	rti

	.section .bss.fm1_cpu1_stack,"aw",@nobits
	.balign 8
	.globl fm1_cpu1_ustack
fm1_cpu1_ustack:		/* after rti CPU1 runs on this one: the jobs' (FM1_CPU1_USTACK_WORDS; marked at start) */
	.space FM1_CPU1_USTACK_BYTES
fm1_cpu1_ustack_top:
	.space 256		/* exceptions only (no interrupts on this core) */
	.globl fm1_cpu1_sstack_top
fm1_cpu1_sstack_top:
