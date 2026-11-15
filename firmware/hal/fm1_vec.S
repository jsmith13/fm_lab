/* SPDX-License-Identifier: GPL-3.0-only AND MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* 128 fatal-vector stubs, 6 bytes each: VEC[n] = fm1_fatal_stubs + 6*n.
 * The call leaves rets = stub + 6, so common code recovers n. */
	.section .text.fm1vec,"ax",@progbits
	.globl fm1_fatal_stubs
	.balign 2
fm1_fatal_stubs:
	.rept 128
	[--sp] = {rets}
	call fm1_fatal_common
	.endr

	.section .text.fm1vec_common,"ax",@progbits
	.globl fm1_fatal_common
fm1_fatal_common:
	trigger
	[--sp] = {sp, ssp, usp, icfg, psr, rets, retx, rete, reti}
	[--sp] = {r15-r0}
	r0 = sp				/* frame: r0..r15, reti, rete, retx, rets(stub+6), psr, icfg, usp, ssp, sp, rets(orig) */
	call fm1_fault_c
#if FM1_CPU1
	/* Resume CPU0 only after the CPU1 fault handler has held the worker.
	 * Frame restoration follows Melodee/felucca-bsp (Kerem Kilic, MPL-2.0). */
	r0 = [sp+64]
	r1 = [sp+100]
	r2 = [sp+80]
	[sp+92] = r0
	[sp+96] = r1
	[sp+100] = r2
	{r15-r0} = [sp++]
	sp += 28
	{psr, rets, reti} = [sp++]
	csync
	rti
#else
1:	goto 1b
#endif
