/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1995, 96, 97, 98, 99, 2000, 01, 02 by Ralf Baechle
 * Copyright (C) 1999, 2000 Silicon Graphics, Inc.
 * Copyright (C) 2001 MIPS Technologies, Inc.
 * Copyright (C) 2020 Loongson Technology Co., Ltd.
 */
#include <linux/errno.h>
#include <asm/asm.h>
#include <asm/asmmacro.h>
#include <asm/irqflags.h>
#include <asm/loongarch.h>
#include <asm/regdef.h>
#include <asm/stackframe.h>
#include <asm/asm-offsets.h>
#include <asm/thread_info.h>
#include <asm/unistd.h>
#include <asm/unwind_hints.h>

	.align	5
SYM_FUNC_START(handle_sys_lat_i386)
	UNWIND_HINT_REGS
	li.d	t1, 0			#_TIF_WORK_SYSCALL_ENTRY
	LONG_L	t0, tp, TI_FLAGS	# syscall tracing enabled?
	and	t0, t1, t0
	bnez	t0, syscall_trace_entry

choose_abi:
	li.d	t0, TRANS_ARCH_MASK
	and	t1, t0, a7		# pick the ARCH specfied tag
	li.d	t0, SYS_NUM_MASK
	and	a7, a7, t0		# pick the syscall num
	li.d	t0, TRANS_I386
	bne	t0, t1, illegal_syscall

syscall_i386:

	sltui   t0, a7, __NR_i386_Linux_syscalls + 1
	beqz    t0, illegal_syscall

	/* Syscall number held in a7 */
	slli.d	t0, a7, 3		# offset into table
	la	t2, i386_syscall_table
	add.d	t0, t2, t0
	ld.d	t2, t0, 0		# syscall routine
	beqz    t2, illegal_syscall

	jirl	ra, t2, 0		# Do The Real Thing (TM)

	li.w	t0, -EMAXERRNO - 1	# error?
	sltu	t0, t0, a0
	st.d	t0, sp, PT_R7		# set error flag
	beqz	t0, 1f

	ld.d	t1, sp, PT_R11		# syscall number
	addi.d	t1, t1, 1		# +1 for handle_signal
	st.d	t1, sp, PT_R0		# save it for syscall restarting
1:	st.d	a0, sp, PT_R4		# result


trans_i386_syscall_exit:
	RESTORE_TEMP
	RESTORE_STATIC
	RESTORE_SOME
	RESTORE_SP_AND_RET

/* ------------------------------------------------------------------------ */

syscall_trace_entry:
	SAVE_STATIC
	move	a0, sp
	move	a1, a7
	move	a0, zero		#bl	syscall_trace_enter

	blt	a0, zero, 1f			# seccomp failed? Skip syscall

	RESTORE_STATIC
	ld.d	a0, sp, PT_R4		# Restore argument registers
	ld.d	a1, sp, PT_R5
	ld.d	a2, sp, PT_R6
	ld.d	a3, sp, PT_R7
	ld.d	a4, sp, PT_R8
	ld.d	a5, sp, PT_R9
	ld.d	a6, sp, PT_R10
	ld.d	a7, sp, PT_R11		# Restore syscall (maybe modified)
	b	choose_abi

1:	b	trans_i386_syscall_exit

	/*
	 * The system call does not exist in this kernel
	 */

illegal_syscall:
	li.w	a0, ENOSYS			# error
	st.d	a0, sp, PT_R4
	li.w	t0, 1				# set error flag
	st.d	t0, sp, PT_R7
	b	trans_i386_syscall_exit
SYM_FUNC_END(handle_sys_lat_i386)

	.align	3
SYM_DATA_START(i386_syscall_table)
	PTR	sys_restart_syscall		/* 0 */
	PTR	sys_exit			/* 1 */
	PTR	sys_ni_syscall			/* 2 sys_fork */
	PTR	sys_read			/* 3 */
	PTR	sys_write			/* 4 */
	PTR	sys_open			/* 5 */
	PTR	sys_close			/* 6 */
	PTR	sys_ni_syscall			/* 7 sys_waitpid */
	PTR	sys_creat			/* 8 */
	PTR	sys_link			/* 9 */
	PTR	sys_unlink			/* 10 */
	PTR	sys_execve			/* 11 */
	PTR	sys_chdir			/* 12 */
	PTR	sys_ni_syscall			/* 13 sys_time */
	PTR	sys_mknod			/* 14 */
	PTR	sys_chmod			/* 15 */
	PTR	sys_lchown16			/* 16 */
	PTR	sys_ni_syscall                  /* 17 break */
	PTR	sys_ni_syscall			/* 18 sys_stat */
	PTR	sys_lseek			/* 19 */
	PTR	sys_getpid			/* 20 */
	PTR	sys_mount			/* 21 */
	PTR	sys_ni_syscall			/* 22 sys_oldumount */
	PTR	sys_setuid16			/* 23 */
	PTR	sys_getuid16			/* 24 */
	PTR	sys_ni_syscall			/* 25 sys_stime */
	PTR	sys_ptrace			/* 26 */
	PTR	sys_alarm			/* 27 */
	PTR	sys_ni_syscall			/* 28 sys_fstat */
	PTR	sys_ni_syscall			/* 29 sys_pause */
	PTR	sys_ni_syscall			/* 30 sys_utime */
	PTR	sys_ni_syscall                  /* 31 stty */
	PTR	sys_ni_syscall                  /* 32 gtty */
	PTR	sys_access			/* 33 */
	PTR	sys_ni_syscall			/* 34 sys_nice */
	PTR	sys_ni_syscall                  /* 35 ftime */
	PTR	sys_sync			/* 36 */
	PTR	sys_kill			/* 37 */
	PTR	sys_rename			/* 38 */
	PTR	sys_mkdir			/* 39 */
	PTR	sys_rmdir			/* 40 */
	PTR	sys_dup				/* 41 */
	PTR	sys_pipe			/* 42 */
	PTR	sys_times			/* 43 */
	PTR	sys_ni_syscall                  /* 44 prof */
	PTR	sys_brk				/* 45 */
	PTR	sys_setgid16			/* 46 */
	PTR	sys_getgid16			/* 47 */
	PTR	sys_ni_syscall			/* 48 sys_signal */
	PTR	sys_geteuid16			/* 49 */
	PTR	sys_getegid16			/* 50 */
	PTR	sys_acct			/* 51 */
	PTR	sys_umount			/* 52 */
	PTR	sys_ni_syscall                  /* 53 lock */
	PTR	sys_ioctl			/* 54 */
	PTR	sys_fcntl			/* 55 */
	PTR	sys_ni_syscall                  /* 56 mxp */
	PTR	sys_setpgid			/* 57 */
	PTR	sys_ni_syscall                  /* 58 ulimit */
	PTR	sys_ni_syscall			/* 59 sys_olduname */
	PTR	sys_umask			/* 60 */
	PTR	sys_chroot			/* 61 */
	PTR	sys_ustat			/* 62 */
	PTR	sys_dup2			/* 63 */
	PTR	sys_getppid			/* 64 */
	PTR	sys_getpgrp			/* 65 */
	PTR	sys_setsid			/* 66 */
	PTR	sys_ni_syscall			/* 67 sys_sigaction */
	PTR	sys_sgetmask			/* 68 */
	PTR	sys_ssetmask			/* 69 */
	PTR	sys_setreuid16			/* 70 */
	PTR	sys_setregid16			/* 71 */
	PTR	sys_ni_syscall			/* 72 sys_sigsuspend */
	PTR	sys_ni_syscall			/* 73 sys_sigpending */
	PTR	sys_sethostname			/* 74 */
	PTR	sys_setrlimit			/* 75 */
	PTR	sys_ni_syscall			/* 76 sys_old_getrlimit */
	PTR	sys_getrusage			/* 77 */
	PTR	sys_gettimeofday		/* 78 */
	PTR	sys_settimeofday		/* 79 */
	PTR	sys_getgroups16			/* 80 */
	PTR	sys_setgroups16			/* 81 */
	PTR	sys_ni_syscall			/* 82 sys_old_select */
	PTR	sys_symlink			/* 83 */
	PTR	sys_ni_syscall			/* 84 sys_lstat */
	PTR	sys_readlink			/* 85 */
	PTR	sys_uselib			/* 86 */
	PTR	sys_swapon			/* 87 */
	PTR	sys_reboot			/* 88 */
	PTR	sys_ni_syscall			/* 89 sys_old_readdir */
	PTR	sys_ni_syscall			/* 90 sys_old_mmap */
	PTR	sys_munmap			/* 91 */
	PTR	sys_truncate			/* 92 */
	PTR	sys_ftruncate			/* 93 */
	PTR	sys_fchmod			/* 94 */
	PTR	sys_fchown16			/* 95 */
	PTR	sys_getpriority			/* 96 */
	PTR	sys_setpriority			/* 97 */
	PTR	sys_ni_syscall                  /* 98 profil */
	PTR	sys_statfs			/* 99 */
	PTR	sys_fstatfs			/* 100 */
	PTR	sys_ni_syscall			/* 101 sys_ioperm */
	PTR	sys_socketcall			/* 102 */
	PTR	sys_syslog			/* 103 */
	PTR	sys_setitimer			/* 104 */
	PTR	sys_getitimer			/* 105 */
	PTR	sys_newstat			/* 106 */
	PTR	sys_newlstat			/* 107 */
	PTR	sys_newfstat			/* 108 */
	PTR	sys_ni_syscall			/* 109 sys_uname */
	PTR	sys_ni_syscall			/* 110 sys_iopl */
	PTR	sys_vhangup			/* 111 */
	PTR	sys_ni_syscall                  /* 112 idel */
	PTR	sys_vm86old			/* 113 */
	PTR	sys_wait4			/* 114 */
	PTR	sys_swapoff			/* 115 */
	PTR	sys_sysinfo			/* 116 */
	PTR	sys_ipc				/* 117 */
	PTR	sys_fsync			/* 118 */
	PTR	sys_ni_syscall			/* 119 sys_sigreturn */
	PTR	sys_clone			/* 120 */
	PTR	sys_setdomainname		/* 121 */
	PTR	sys_newuname			/* 122 */
	PTR	sys_modify_ldt			/* 123 */
	PTR	sys_adjtimex			/* 124 */
	PTR	sys_mprotect			/* 125 */
	PTR	sys_ni_syscall			/* 126 sys_sigprocmask */
	PTR	sys_ni_syscall                  /* 127 create_module */
	PTR	sys_init_module			/* 128 */
	PTR	sys_delete_module		/* 129 */
	PTR	sys_ni_syscall                  /* 130 get_kernel_syms */
	PTR	sys_quotactl			/* 131 */
	PTR	sys_getpgid			/* 132 */
	PTR	sys_fchdir			/* 133 */
	PTR	sys_ni_syscall	//sys_bdflush			/* 134 */
	PTR	sys_sysfs			/* 135 */
	PTR	sys_personality			/* 136 */
	PTR	sys_ni_syscall                  /* 137 afs_syscall */
	PTR	sys_setfsuid16			/* 138 */
	PTR	sys_setfsgid16			/* 139 */
	PTR	sys_ni_syscall			/* 140 sys_llseek */
	PTR	sys_getdents			/* 141 */
	PTR	sys_select			/* 142 */
	PTR	sys_flock			/* 143 */
	PTR	sys_msync			/* 144 */
	PTR	sys_readv			/* 145 */
	PTR	sys_writev			/* 146 */
	PTR	sys_getsid			/* 147 */
	PTR	sys_fdatasync			/* 148 */
	PTR	sys_ni_syscall	//sys_sysctl			/* 149 */
	PTR	sys_mlock			/* 150 */
	PTR	sys_munlock			/* 151 */
	PTR	sys_mlockall			/* 152 */
	PTR	sys_munlockall			/* 153 */
	PTR	sys_sched_setparam		/* 154 */
	PTR	sys_sched_getparam		/* 155 */
	PTR	sys_sched_setscheduler		/* 156 */
	PTR	sys_sched_getscheduler		/* 157 */
	PTR	sys_sched_yield			/* 158 */
	PTR	sys_sched_get_priority_max	/* 159 */
	PTR	sys_sched_get_priority_min	/* 160 */
	PTR	sys_sched_rr_get_interval	/* 161 */
	PTR	sys_nanosleep			/* 162 */
	PTR	sys_mremap			/* 163 */
	PTR	sys_setresuid16			/* 164 */
	PTR	sys_getresuid16			/* 165 */
	PTR	sys_vm86			/* 166 */
	PTR	sys_ni_syscall                  /* 167 query_module */
	PTR	sys_poll			/* 168 */
	PTR	sys_ni_syscall                  /* 169 nfsservctl */
	PTR	sys_setresgid16			/* 170 */
	PTR	sys_getresgid16			/* 171 */
	PTR	sys_prctl			/* 172 */
	PTR	sys_rt_sigreturn		/* 173 */
	PTR	sys_rt_sigaction		/* 174 */
	PTR	sys_latx_rt_sigprocmask		/* 175 */
	PTR	sys_rt_sigpending		/* 176 */
	PTR	sys_rt_sigtimedwait		/* 177 */
	PTR	sys_rt_sigqueueinfo		/* 178 */
	PTR	sys_rt_sigsuspend		/* 179 */
	PTR	sys_pread64			/* 180 */
	PTR	sys_pwrite64			/* 181 */
	PTR	sys_chown16			/* 182 */
	PTR	sys_getcwd			/* 183 */
	PTR	sys_capget			/* 184 */
	PTR	sys_capset			/* 185 */
	PTR	sys_sigaltstack			/* 186 */
	PTR	sys_sendfile			/* 187 */
	PTR	sys_ni_syscall                  /* 188 getpmsg */
	PTR	sys_ni_syscall                  /* 189 putpmsg */
	PTR	sys_ni_syscall			/* 190 sys_vfork */
	PTR	sys_getrlimit			/* 191 */
	PTR	sys_mmap_pgoff			/* 192 */
	PTR	sys_ni_syscall			/* 193 sys_truncate64 */
	PTR	sys_ni_syscall			/* 194 sys_ftruncate64 */
	PTR	sys_ni_syscall			/* 195 sys_stat64 */
	PTR	sys_ni_syscall			/* 196 sys_lstat64 */
	PTR	sys_ni_syscall			/* 197 sys_fstat64 */
	PTR	sys_lchown			/* 198 */
	PTR	sys_getuid			/* 199 */
	PTR	sys_getgid			/* 200 */
	PTR	sys_geteuid			/* 201 */
	PTR	sys_getegid			/* 202 */
	PTR	sys_setreuid			/* 203 */
	PTR	sys_setregid			/* 204 */
	PTR	sys_getgroups			/* 205 */
	PTR	sys_setgroups			/* 206 */
	PTR	sys_fchown			/* 207 */
	PTR	sys_setresuid			/* 208 */
	PTR	sys_getresuid			/* 209 */
	PTR	sys_setresgid			/* 210 */
	PTR	sys_getresgid			/* 211 */
	PTR	sys_chown			/* 212 */
	PTR	sys_setuid			/* 213 */
	PTR	sys_setgid			/* 214 */
	PTR	sys_setfsuid			/* 215 */
	PTR	sys_setfsgid			/* 216 */
	PTR	sys_pivot_root			/* 217 */
	PTR	sys_mincore			/* 218 */
	PTR	sys_madvise			/* 219 */
	PTR	sys_getdents64			/* 220 */
	PTR	sys_ni_syscall			/* 221 sys_fcntl64 */
	PTR	sys_ni_syscall                  /* 222 is unused */
	PTR	sys_ni_syscall                  /* 223 is unused */
	PTR	sys_gettid			/* 224 */
	PTR	sys_readahead			/* 225 */
	PTR	sys_setxattr			/* 226 */
	PTR	sys_lsetxattr			/* 227 */
	PTR	sys_fsetxattr			/* 228 */
	PTR	sys_getxattr			/* 229 */
	PTR	sys_lgetxattr			/* 230 */
	PTR	sys_fgetxattr			/* 231 */
	PTR	sys_listxattr			/* 232 */
	PTR	sys_llistxattr			/* 233 */
	PTR	sys_flistxattr			/* 234 */
	PTR	sys_removexattr			/* 235 */
	PTR	sys_lremovexattr		/* 236 */
	PTR	sys_fremovexattr		/* 237 */
	PTR	sys_tkill			/* 238 */
	PTR	sys_sendfile64			/* 239 */
	PTR	sys_futex			/* 240 */
	PTR	sys_sched_setaffinity		/* 241 */
	PTR	sys_sched_getaffinity		/* 242 */
	PTR	sys_ni_syscall			/* 243 sys_set_thread_area */
	PTR	sys_ni_syscall			/* 244 sys_get_thread_area */
	PTR	sys_io_setup			/* 245 */
	PTR	sys_io_destroy			/* 246 */
	PTR	sys_io_getevents		/* 247 */
	PTR	sys_io_submit			/* 248 */
	PTR	sys_io_cancel			/* 249 */
	PTR	sys_fadvise64			/* 250 */
	PTR	sys_ni_syscall                  /* 251 is available for reuse*/
	PTR	sys_exit_group			/* 252 */
	PTR	sys_ni_syscall			/* 253 */
	PTR	sys_epoll_create		/* 254 */
	PTR	sys_epoll_ctl			/* 255 */
	PTR	sys_epoll_wait			/* 256 */
	PTR	sys_remap_file_pages		/* 257 */
	PTR	sys_set_tid_address		/* 258 */
	PTR	sys_timer_create		/* 259 */
	PTR	sys_timer_settime		/* 260 */
	PTR	sys_timer_gettime		/* 261 */
	PTR	sys_timer_getoverrun		/* 262 */
	PTR	sys_timer_delete		/* 263 */
	PTR	sys_clock_settime		/* 264 */
	PTR	sys_clock_gettime		/* 265 */
	PTR	sys_clock_getres		/* 266 */
	PTR	sys_clock_nanosleep		/* 267 */
	PTR	sys_statfs64			/* 268 */
	PTR	sys_fstatfs64			/* 269 */
	PTR	sys_tgkill			/* 270 */
	PTR	sys_ni_syscall	//sys_utimes			/* 271 */
	PTR	sys_fadvise64_64		/* 272 */
	PTR	sys_ni_syscall                  /* 273 vserver */
	PTR	sys_mbind			/* 274 */
	PTR	sys_get_mempolicy		/* 275 */
	PTR	sys_set_mempolicy		/* 276 */
	PTR	sys_mq_open			/* 277 */
	PTR	sys_mq_unlink			/* 278 */
	PTR	sys_mq_timedsend		/* 279 */
	PTR	sys_mq_timedreceive		/* 280 */
	PTR	sys_mq_notify			/* 281 */
	PTR	sys_mq_getsetattr		/* 282 */
	PTR	sys_kexec_load			/* 283 */
	PTR	sys_waitid			/* 284 */
	PTR	sys_ni_syscall                  /* 285 sys_setaltroot */
	PTR	sys_add_key			/* 286 */
	PTR	sys_request_key			/* 287 */
	PTR	sys_keyctl			/* 288 */
	PTR	sys_ioprio_set			/* 289 */
	PTR	sys_ioprio_get			/* 290 */
	PTR	sys_inotify_init		/* 291 */
	PTR	sys_inotify_add_watch		/* 292 */
	PTR	sys_inotify_rm_watch		/* 293 */
	PTR	sys_migrate_pages		/* 294 */
	PTR	sys_openat			/* 295 */
	PTR	sys_mkdirat			/* 296 */
	PTR	sys_mknodat			/* 297 */
	PTR	sys_fchownat			/* 298 */
	PTR	sys_ni_syscall	//sys_futimesat			/* 299 */
	PTR	sys_ni_syscall			/* 300 sys_fstatat64 */
	PTR	sys_unlinkat			/* 301 */
	PTR	sys_renameat			/* 302 */
	PTR	sys_linkat			/* 303 */
	PTR	sys_symlinkat			/* 304 */
	PTR	sys_readlinkat			/* 305 */
	PTR	sys_fchmodat			/* 306 */
	PTR	sys_faccessat			/* 307 */
	PTR	sys_pselect6			/* 308 */
	PTR	sys_ppoll			/* 309 */
	PTR	sys_unshare			/* 310 */
	PTR	sys_set_robust_list		/* 311 */
	PTR	sys_get_robust_list		/* 312 */
	PTR	sys_splice			/* 313 */
	PTR	sys_sync_file_range		/* 314 */
	PTR	sys_tee				/* 315 */
	PTR	sys_vmsplice			/* 316 */
	PTR	sys_move_pages			/* 317 */
	PTR	sys_getcpu			/* 318 */
	PTR	sys_epoll_pwait			/* 319 */
	PTR	sys_utimensat			/* 320 */
	PTR	sys_signalfd			/* 321 */
	PTR	sys_timerfd_create		/* 322 */
	PTR	sys_eventfd			/* 323 */
	PTR	sys_fallocate			/* 324 */
	PTR	sys_timerfd_settime		/* 325 */
	PTR	sys_timerfd_gettime		/* 326 */
	PTR	sys_signalfd4			/* 327 */
	PTR	sys_eventfd2			/* 328 */
	PTR	sys_epoll_create1		/* 329 */
	PTR	sys_dup3			/* 330 */
	PTR	sys_pipe2			/* 331 */
	PTR	sys_inotify_init1		/* 332 */
	PTR	sys_preadv			/* 333 */
	PTR	sys_pwritev			/* 334 */
	PTR	sys_rt_tgsigqueueinfo		/* 335 */
	PTR	sys_perf_event_open		/* 336 */
	PTR	sys_recvmmsg			/* 337 */
	PTR	sys_fanotify_init		/* 338 */
	PTR	sys_fanotify_mark		/* 339 */
	PTR	sys_prlimit64			/* 340 */
	PTR	sys_name_to_handle_at		/* 341 */
	PTR	sys_open_by_handle_at		/* 342 */
	PTR	sys_clock_adjtime		/* 343 */
	PTR	sys_syncfs			/* 344 */
	PTR	sys_sendmmsg			/* 345 */
	PTR	sys_setns			/* 346 */
	PTR	sys_process_vm_readv		/* 347 */
	PTR	sys_process_vm_writev		/* 348 */
	PTR	sys_kcmp			/* 349 */
	PTR	sys_finit_module		/* 350 */
	PTR	sys_sched_setattr		/* 351 */
	PTR	sys_sched_getattr		/* 352 */
	PTR	sys_renameat2			/* 353 */
	PTR	sys_seccomp			/* 354 */
	PTR	sys_getrandom			/* 355 */
	PTR	sys_memfd_create		/* 356 */
	PTR	sys_bpf				/* 357 */
	PTR	sys_execveat			/* 358 */
	PTR	sys_socket			/* 359 */
	PTR	sys_socketpair			/* 360 */
	PTR	sys_bind			/* 361 */
	PTR	sys_connect			/* 362 */
	PTR	sys_listen			/* 363 */
	PTR	sys_accept4			/* 364 */
	PTR	sys_getsockopt			/* 365 */
	PTR	sys_setsockopt			/* 366 */
	PTR	sys_getsockname			/* 367 */
	PTR	sys_getpeername			/* 368 */
	PTR	sys_sendto			/* 369 */
	PTR	sys_sendmsg			/* 370 */
	PTR	sys_recvfrom			/* 371 */
	PTR	sys_recvmsg			/* 372 */
	PTR	sys_shutdown			/* 373 */
	PTR	sys_userfaultfd			/* 374 */
	PTR	sys_membarrier			/* 375 */
	PTR	sys_mlock2			/* 376 */
	PTR	sys_copy_file_range		/* 377 */
	PTR	sys_preadv2			/* 378 */
	PTR	sys_pwritev2			/* 379 */
	PTR	sys_pkey_mprotect		/* 380 */
	PTR	sys_pkey_alloc			/* 381 */
	PTR	sys_pkey_free			/* 382 */
	PTR	sys_statx			/* 383 */
	PTR	sys_ni_syscall			/* 384 sys_arch_prctl */
	PTR	sys_io_pgetevents		/* 385 */
	PTR	sys_rseq			/* 386 */
SYM_DATA_END(i386_syscall_table)
