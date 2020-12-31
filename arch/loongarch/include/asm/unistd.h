/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Author: Hanlu Li <lihanlu@loongson.cn>
 *         Huacai Chen <chenhuacai@loongson.cn>
 *
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */

#include <uapi/asm/unistd.h>

#define NR_syscalls (__NR_syscalls)

/* for binary translation */
#if defined(CONFIG_CPU_HAS_LBT)
#define __ARCH_WANT_SYS_ALARM
#define __ARCH_WANT_SYS_GETPGRP

#define __NR_MIPS64_Linux		5000
#define __NR_MIPS64_Linux_syscalls	329
#define __NR_i386_Linux_syscalls	386

#define TRANS_MIPS_N64			0x010000
#define TRANS_I386			0x100000
#define TRANS_X64			0x110000
#define TRANS_ARCH_MASK			0xffff0000
#define SYS_NUM_MASK			0xffff
#endif
