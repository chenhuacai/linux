/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Author: Lu Zeng <zenglu@loongson.cn>
 *         Pei Huang <huangpei@loongson.cn>
 *         Huacai Chen <chenhuacai@loongson.cn>
 *
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */
#include <asm/asm.h>
#include <asm/asmmacro.h>
#include <asm/asm-extable.h>
#include <asm/asm-offsets.h>
#include <asm/errno.h>
#include <asm/export.h>
#include <asm/fpregdef.h>
#include <asm/loongarch.h>
#include <asm/regdef.h>

#if defined(CONFIG_CPU_HAS_LBT)

#define SCR_REG_WIDTH 8

	.macro	EX insn, reg, src, offs
.ex\@:	\insn	\reg, \src, \offs
	_asm_extable .ex\@, lbt_fault
	.endm

	.macro	sc_save_scr base, tmp0
	movscr2gr	\tmp0, $scr0
	EX	st.d	\tmp0, \base, (0 * SCR_REG_WIDTH)
	movscr2gr	\tmp0, $scr1
	EX	st.d	\tmp0, \base, (1 * SCR_REG_WIDTH)
	movscr2gr	\tmp0, $scr2
	EX	st.d	\tmp0, \base, (2 * SCR_REG_WIDTH)
	movscr2gr	\tmp0, $scr3
	EX	st.d	\tmp0, \base, (3 * SCR_REG_WIDTH)
	.endm

	.macro	sc_restore_scr base, tmp0
	EX	ld.d	\tmp0, \base, (0 * SCR_REG_WIDTH)
	movgr2scr	$scr0, \tmp0
	EX	ld.d	\tmp0, \base, (1 * SCR_REG_WIDTH)
	movgr2scr	$scr1, \tmp0
	EX	ld.d	\tmp0, \base, (2 * SCR_REG_WIDTH)
	movgr2scr	$scr2, \tmp0
	EX	ld.d	\tmp0, \base, (3 * SCR_REG_WIDTH)
	movgr2scr	$scr3, \tmp0
	.endm

	.macro	sc_save_eflag base, tmp0
	x86mfflag	\tmp0, 0x3f
	EX st.w		\tmp0, \base, 0
	.endm

	.macro	sc_restore_eflag base, tmp0
	EX ld.w		\tmp0, \base, 0
	x86mtflag	\tmp0, 0x3f
	.endm

/*
 * Save a thread's lbt context.
 */
SYM_FUNC_START(_save_lbt)
	lbt_save_scr	a0 t1		# clobbers t1
	lbt_save_eflag	a0 t1		# clobbers t1
	jr				ra
SYM_FUNC_END(_save_lbt)
EXPORT_SYMBOL(_save_lbt)

/*
 * Restore a thread's lbt context.
 */
SYM_FUNC_START(_restore_lbt)
	lbt_restore_eflag	a0 t1	# clobbers t1
	lbt_restore_scr		a0 t1	# clobbers t1
	jr					ra
SYM_FUNC_END(_restore_lbt)
EXPORT_SYMBOL(_restore_lbt)

/*
 * a0: scr
 * a1: eflag
 */
SYM_FUNC_START(_save_lbt_context)
	sc_save_scr		a0 t1
	sc_save_eflag	a1 t1
	li.w			a0, 0		# success
	jr				ra
SYM_FUNC_END(_save_lbt_context)

/*
 * a0: scr
 * a1: eflag
 */
SYM_FUNC_START(_restore_lbt_context)
	sc_restore_scr		a0 t1
	sc_restore_eflag	a1 t1
	li.w				a0, 0	# success
	jr					ra
SYM_FUNC_END(_restore_lbt_context)

/*
 * a0: ftop
 */
SYM_FUNC_START(_save_ftop_context)
	x86mftop	t1
	st.b		t1, a0, 0
	li.w		a0, 0			# success
	jr			ra
SYM_FUNC_END(_save_ftop_context)

/*
 * a0: ftop
 */
SYM_FUNC_START(_restore_ftop_context)
	ld.b		t1, a0, 0
	andi		t1, t1, 0x7
	la.pcrel	a0, 2f
	alsl.d		a0, t1, a0, 3
	jr			a0
	2 :
	x86mttop	0
	b	1f
	x86mttop	1
	b	1f
	x86mttop	2
	b	1f
	x86mttop	3
	b	1f
	x86mttop	4
	b	1f
	x86mttop	5
	b	1f
	x86mttop	6
	b	1f
	x86mttop	7
	1 :
	li.w		a0, 0			# success
	jr			ra
SYM_FUNC_END(_restore_ftop_context)


SYM_FUNC_START(lbt_fault)
	li.w		a0, -EFAULT		# failure
	jr			ra
SYM_FUNC_END(lbt_fault)

#else

/*
 * Save a thread's lbt context.
 */
SYM_FUNC_START(_save_lbt)
	jr	ra
SYM_FUNC_END(_save_lbt)
EXPORT_SYMBOL(_save_lbt)

/*
 * Restore a thread's lbt context.
 */
SYM_FUNC_START(_restore_lbt)
	jr	ra
SYM_FUNC_END(_restore_lbt)
EXPORT_SYMBOL(_restore_lbt)

#endif
