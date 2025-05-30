/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 Intel Corporation
 */

#ifndef __ASSEMBLY__
#define __ASSEMBLY__
#endif

#include <asm/csr.h>

.macro save_context
	addi  sp, sp, (-8*36)
	sd    x1, 8(sp)
	sd    x2, 16(sp)
	sd    x3, 24(sp)
	sd    x4, 32(sp)
	sd    x5, 40(sp)
	sd    x6, 48(sp)
	sd    x7, 56(sp)
	sd    x8, 64(sp)
	sd    x9, 72(sp)
	sd    x10, 80(sp)
	sd    x11, 88(sp)
	sd    x12, 96(sp)
	sd    x13, 104(sp)
	sd    x14, 112(sp)
	sd    x15, 120(sp)
	sd    x16, 128(sp)
	sd    x17, 136(sp)
	sd    x18, 144(sp)
	sd    x19, 152(sp)
	sd    x20, 160(sp)
	sd    x21, 168(sp)
	sd    x22, 176(sp)
	sd    x23, 184(sp)
	sd    x24, 192(sp)
	sd    x25, 200(sp)
	sd    x26, 208(sp)
	sd    x27, 216(sp)
	sd    x28, 224(sp)
	sd    x29, 232(sp)
	sd    x30, 240(sp)
	sd    x31, 248(sp)
	csrr  s0, CSR_SEPC
	csrr  s1, CSR_SSTATUS
	csrr  s2, CSR_STVAL
	csrr  s3, CSR_SCAUSE
	sd    s0, 0(sp)
	sd    s1, 256(sp)
	sd    s2, 264(sp)
	sd    s3, 272(sp)
.endm

.macro restore_context
	ld    s3, 272(sp)
	ld    s2, 264(sp)
	ld    s1, 256(sp)
	ld    s0, 0(sp)
	csrw  CSR_SCAUSE, s3
	csrw  CSR_SSTATUS, s1
	csrw  CSR_SEPC, s0
	ld    x31, 248(sp)
	ld    x30, 240(sp)
	ld    x29, 232(sp)
	ld    x28, 224(sp)
	ld    x27, 216(sp)
	ld    x26, 208(sp)
	ld    x25, 200(sp)
	ld    x24, 192(sp)
	ld    x23, 184(sp)
	ld    x22, 176(sp)
	ld    x21, 168(sp)
	ld    x20, 160(sp)
	ld    x19, 152(sp)
	ld    x18, 144(sp)
	ld    x17, 136(sp)
	ld    x16, 128(sp)
	ld    x15, 120(sp)
	ld    x14, 112(sp)
	ld    x13, 104(sp)
	ld    x12, 96(sp)
	ld    x11, 88(sp)
	ld    x10, 80(sp)
	ld    x9, 72(sp)
	ld    x8, 64(sp)
	ld    x7, 56(sp)
	ld    x6, 48(sp)
	ld    x5, 40(sp)
	ld    x4, 32(sp)
	ld    x3, 24(sp)
	ld    x2, 16(sp)
	ld    x1, 8(sp)
	addi  sp, sp, (8*36)
.endm

.balign 4
.global exception_vectors
exception_vectors:
	save_context
	move  a0, sp
	call  route_exception
	restore_context
	sret
