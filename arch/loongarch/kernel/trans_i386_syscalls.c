// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Loongson Technology Corporation Limited
 * Authors: Hanlu Li <lihanlu@loongson.cn>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */
#include <linux/syscalls.h>

typedef struct {
	uint32_t sig[2];
} latx_sigset_t;

typedef uint32_t latx_size_t;

static inline void latx_sig_setmask(sigset_t *blocked, old_sigset_t set)
{
	memcpy(blocked->sig, &set, sizeof(set));
}

/* This is for latx-i386 */
SYSCALL_DEFINE4(latx_rt_sigprocmask, int, how, latx_sigset_t __user *, nset,
	latx_sigset_t __user *, oset, latx_size_t, sigsetsize)
{
	old_sigset_t old_set, new_set;
	sigset_t new_blocked;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(latx_sigset_t))
		return -EINVAL;

	old_set = current->blocked.sig[0];

	if (nset) {
		if (copy_from_user(&new_set, nset, sizeof(latx_sigset_t)))
			return -EFAULT;
		new_set &= ~(sigmask(SIGKILL) | sigmask(SIGSTOP));

		new_blocked = current->blocked;

		switch (how) {
		case SIG_BLOCK:
			sigaddsetmask(&new_blocked, new_set);
			break;
		case SIG_UNBLOCK:
			sigdelsetmask(&new_blocked, new_set);
			break;
		case SIG_SETMASK:
			latx_sig_setmask(&new_blocked, new_set);
			break;
		default:
			return -EINVAL;
		}
		set_current_blocked(&new_blocked);

		if (copy_to_user(nset, &(current->blocked),
				sizeof(latx_sigset_t)))
			return -EFAULT;
	}

	if (oset) {
		if (copy_to_user(oset, &old_set, sizeof(latx_sigset_t)))
			return -EFAULT;
	}

	return 0;
}
