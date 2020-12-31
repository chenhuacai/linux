/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2020 Loongson Technology Co., Ltd.
 */
#include <linux/errno.h>
#include <asm/asm.h>
#include <asm/asmmacro.h>
#include <asm/loongarch.h>
#include <asm/regdef.h>
#include <asm/stackframe.h>
#include <asm/asm-offsets.h>
#include <asm/thread_info.h>
#include <asm/unistd.h>
#include <asm/unwind_hints.h>

	.align	5
SYM_FUNC_START(handle_sys_lat_mips64)
	UNWIND_HINT_REGS
	li.d	t1, 0			#_TIF_WORK_SYSCALL_ENTRY
	LONG_L	t0, tp, TI_FLAGS	# syscall tracing enabled?
	and	t0, t1, t0
	bnez	t0, syscall_trace_entry

choose_abi:
	li.d	t0, TRANS_ARCH_MASK
	and	t1, t0, a7
	li.d	t0, SYS_NUM_MASK
	and	a7, a7, t0
	li.d	t0, TRANS_MIPS_N64
	bne	t0, t1, illegal_syscall

syscall_mips64:
	li.w	t0, __NR_MIPS64_Linux
	sub.d	t2, a7, t0
	sltui   t0, t2, __NR_MIPS64_Linux_syscalls + 1
	beqz    t0, illegal_syscall

	/* Syscall number held in a7 */
	slli.d	t0, t2, 3		# offset into table
	la	t2, mips64_syscall_table
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
	sub.d	a0, zero, a0
	bl	trans_mips_errno
1:	st.d	a0, sp, PT_R4		# result


trans_syscall_exit:
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

1:	b	trans_syscall_exit

	/*
	 * The system call does not exist in this kernel
	 */

illegal_syscall:
	li.w	a0, ENOSYS			# error
	st.d	a0, sp, PT_R4
	li.w	t0, 1				# set error flag
	st.d	t0, sp, PT_R7
	b	trans_syscall_exit
SYM_FUNC_END(handle_sys_lat_mips64)

	.align	3
SYM_DATA_START(mips64_syscall_table)
	PTR	sys_read			/* 5000 */
	PTR	sys_write
	PTR	sys_ni_syscall
	PTR	sys_close
	PTR	sys_ni_syscall			/* stat */
	PTR	sys_ni_syscall			/* 5005 */
	PTR	sys_ni_syscall			/* lstat */
	PTR	sys_ni_syscall
	PTR	sys_lseek
	PTR	sys_ni_syscall
	PTR	sys_mprotect			/* 5010 */
	PTR	sys_munmap
	PTR	sys_brk
	PTR	sys_ni_syscall
	PTR	sys_rt_sigprocmask
	PTR	sys_ioctl			/* 5015 */
	PTR	sys_pread64
	PTR	sys_pwrite64
	PTR	sys_readv
	PTR	sys_writev
	PTR	sys_access			/* 5020 */
	PTR	sys_pipe
	PTR	sys_ni_syscall
	PTR	sys_sched_yield
	PTR	sys_mremap
	PTR	sys_msync			/* 5025 */
	PTR	sys_mincore
	PTR	sys_madvise
	PTR	sys_shmget
	PTR	sys_shmat
	PTR	sys_shmctl			/* 5030 */
	PTR	sys_dup
	PTR	sys_dup2
	PTR	sys_ni_syscall			/* pause */
	PTR	sys_nanosleep
	PTR	sys_getitimer			/* 5035 */
	PTR	sys_setitimer
	PTR	sys_alarm
	PTR	sys_getpid
	PTR	sys_sendfile64
	PTR	sys_ni_syscall			/* 5040 */
	PTR	sys_connect
	PTR	sys_accept
	PTR	sys_sendto
	PTR	sys_recvfrom
	PTR	sys_sendmsg			/* 5045 */
	PTR	sys_recvmsg
	PTR	sys_shutdown
	PTR	sys_bind
	PTR	sys_listen
	PTR	sys_getsockname			/* 5050 */
	PTR	sys_getpeername
	PTR	sys_socketpair
	PTR	sys_setsockopt
	PTR	sys_getsockopt
	PTR	sys_ni_syscall			/* 5055 */
	PTR	sys_ni_syscall
	PTR	sys_execve
	PTR	sys_exit
	PTR	sys_wait4
	PTR	sys_kill			/* 5060 */
	PTR	sys_newuname
	PTR	sys_semget
	PTR	sys_semop
	PTR	sys_semctl
	PTR	sys_shmdt			/* 5065 */
	PTR	sys_msgget
	PTR	sys_msgsnd
	PTR	sys_msgrcv
	PTR	sys_msgctl
	PTR	sys_fcntl			/* 5070 */
	PTR	sys_flock
	PTR	sys_fsync
	PTR	sys_fdatasync
	PTR	sys_truncate
	PTR	sys_ftruncate			/* 5075 */
	PTR	sys_ni_syscall
	PTR	sys_getcwd
	PTR	sys_chdir
	PTR	sys_fchdir
	PTR	sys_rename			/* 5080 */
	PTR	sys_mkdir
	PTR	sys_rmdir
	PTR	sys_ni_syscall
	PTR	sys_link
	PTR	sys_unlink			/* 5085 */
	PTR	sys_symlink
	PTR	sys_readlink
	PTR	sys_chmod
	PTR	sys_fchmod
	PTR	sys_chown			/* 5090 */
	PTR	sys_fchown
	PTR	sys_lchown
	PTR	sys_umask
	PTR	sys_gettimeofday
	PTR	sys_getrlimit			/* 5095 */
	PTR	sys_getrusage
	PTR	sys_sysinfo
	PTR	sys_times
	PTR	sys_ptrace
	PTR	sys_getuid			/* 5100 */
	PTR	sys_syslog
	PTR	sys_getgid
	PTR	sys_setuid
	PTR	sys_setgid
	PTR	sys_geteuid			/* 5105 */
	PTR	sys_getegid
	PTR	sys_setpgid
	PTR	sys_getppid
	PTR	sys_getpgrp
	PTR	sys_setsid			/* 5110 */
	PTR	sys_setreuid
	PTR	sys_setregid
	PTR	sys_getgroups
	PTR	sys_setgroups
	PTR	sys_setresuid			/* 5115 */
	PTR	sys_getresuid
	PTR	sys_setresgid
	PTR	sys_getresgid
	PTR	sys_getpgid
	PTR	sys_setfsuid			/* 5120 */
	PTR	sys_setfsgid
	PTR	sys_getsid
	PTR	sys_capget
	PTR	sys_capset
	PTR	sys_rt_sigpending		/* 5125 */
	PTR	sys_rt_sigtimedwait
	PTR	sys_rt_sigqueueinfo
	PTR	sys_rt_sigsuspend
	PTR	sys_sigaltstack
	PTR	sys_ni_syscall			/* 5130 */
	PTR	sys_mknod
	PTR	sys_personality
	PTR	sys_ni_syscall
	PTR	sys_ni_syscall			/* statfs */
	PTR	sys_ni_syscall			/* 5135 */
	PTR	sys_ni_syscall
	PTR	sys_getpriority
	PTR	sys_setpriority
	PTR	sys_sched_setparam
	PTR	sys_sched_getparam		/* 5140 */
	PTR	sys_sched_setscheduler
	PTR	sys_sched_getscheduler
	PTR	sys_sched_get_priority_max
	PTR	sys_sched_get_priority_min
	PTR	sys_sched_rr_get_interval	/* 5145 */
	PTR	sys_mlock
	PTR	sys_munlock
	PTR	sys_mlockall
	PTR	sys_munlockall
	PTR	sys_vhangup			/* 5150 */
	PTR	sys_pivot_root
	PTR	sys_ni_syscall
	PTR	sys_prctl
	PTR	sys_adjtimex
	PTR	sys_setrlimit			/* 5155 */
	PTR	sys_chroot
	PTR	sys_sync
	PTR	sys_acct
	PTR	sys_settimeofday
	PTR	sys_mount			/* 5160 */
	PTR	sys_umount
	PTR	sys_swapon
	PTR	sys_swapoff
	PTR	sys_reboot
	PTR	sys_sethostname			/* 5165 */
	PTR	sys_setdomainname
	PTR	sys_ni_syscall			/* was create_module */
	PTR	sys_init_module
	PTR	sys_delete_module
	PTR	sys_ni_syscall			/* 5170, was get_kernel_syms */
	PTR	sys_ni_syscall			/* was query_module */
	PTR	sys_quotactl
	PTR	sys_ni_syscall			/* was nfsservctl */
	PTR	sys_ni_syscall			/* res. for getpmsg */
	PTR	sys_ni_syscall			/* 5175	 for putpmsg */
	PTR	sys_ni_syscall			/* res. for afs_syscall */
	PTR	sys_ni_syscall			/* res. for security */
	PTR	sys_gettid
	PTR	sys_readahead
	PTR	sys_setxattr			/* 5180 */
	PTR	sys_lsetxattr
	PTR	sys_fsetxattr
	PTR	sys_getxattr
	PTR	sys_lgetxattr
	PTR	sys_fgetxattr			/* 5185 */
	PTR	sys_listxattr
	PTR	sys_llistxattr
	PTR	sys_flistxattr
	PTR	sys_removexattr
	PTR	sys_lremovexattr		/* 5190 */
	PTR	sys_fremovexattr
	PTR	sys_tkill
	PTR	sys_ni_syscall
	PTR	sys_futex
	PTR	sys_sched_setaffinity		/* 5195 */
	PTR	sys_sched_getaffinity
	PTR	sys_ni_syscall
	PTR	sys_ni_syscall
	PTR	sys_ni_syscall
	PTR	sys_io_setup			/* 5200 */
	PTR	sys_io_destroy
	PTR	sys_io_getevents
	PTR	sys_io_submit
	PTR	sys_io_cancel
	PTR	sys_exit_group			/* 5205 */
	PTR	sys_ni_syscall
	PTR	sys_epoll_create
	PTR	sys_epoll_ctl
	PTR	sys_epoll_wait
	PTR	sys_remap_file_pages		/* 5210 */
	PTR	sys_rt_sigreturn
	PTR	sys_set_tid_address
	PTR	sys_restart_syscall
	PTR	sys_semtimedop
	PTR	sys_fadvise64_64		/* 5215 */
	PTR	sys_timer_create
	PTR	sys_timer_settime
	PTR	sys_timer_gettime
	PTR	sys_timer_getoverrun
	PTR	sys_timer_delete		/* 5220 */
	PTR	sys_clock_settime
	PTR	sys_clock_gettime
	PTR	sys_clock_getres
	PTR	sys_clock_nanosleep
	PTR	sys_tgkill			/* 5225 */
	PTR	sys_ni_syscall			/* utimes */
	PTR	sys_mbind
	PTR	sys_get_mempolicy
	PTR	sys_set_mempolicy
	PTR	sys_mq_open			/* 5230 */
	PTR	sys_mq_unlink
	PTR	sys_mq_timedsend
	PTR	sys_mq_timedreceive
	PTR	sys_mq_notify
	PTR	sys_mq_getsetattr		/* 5235 */
	PTR	sys_ni_syscall			/* sys_vserver */
	PTR	sys_waitid
	PTR	sys_ni_syscall			/* available, was setaltroot */
	PTR	sys_add_key
	PTR	sys_request_key			/* 5240 */
	PTR	sys_keyctl
	PTR	sys_ni_syscall			/* set_thread_area */
	PTR	sys_inotify_init
	PTR	sys_inotify_add_watch
	PTR	sys_inotify_rm_watch		/* 5245 */
	PTR	sys_migrate_pages
	PTR	sys_ni_syscall
	PTR	sys_mkdirat
	PTR	sys_mknodat
	PTR	sys_fchownat			/* 5250 */
	PTR	sys_ni_syscall			/* futimesat */
	PTR	sys_ni_syscall			/* newfstatat */
	PTR	sys_unlinkat
	PTR	sys_renameat
	PTR	sys_linkat			/* 5255 */
	PTR	sys_symlinkat
	PTR	sys_readlinkat
	PTR	sys_fchmodat
	PTR	sys_faccessat
	PTR	sys_pselect6			/* 5260 */
	PTR	sys_ppoll
	PTR	sys_unshare
	PTR	sys_splice
	PTR	sys_sync_file_range
	PTR	sys_tee				/* 5265 */
	PTR	sys_vmsplice
	PTR	sys_move_pages
	PTR	sys_set_robust_list
	PTR	sys_get_robust_list
	PTR	sys_kexec_load			/* 5270 */
	PTR	sys_getcpu
	PTR	sys_epoll_pwait
	PTR	sys_ioprio_set
	PTR	sys_ioprio_get
	PTR	sys_utimensat			/* 5275 */
	PTR	sys_ni_syscall			/* signalfd */
	PTR	sys_ni_syscall			/* was timerfd */
	PTR	sys_ni_syscall			/* eventfd */
	PTR	sys_fallocate
	PTR	sys_timerfd_create		/* 5280 */
	PTR	sys_timerfd_gettime
	PTR	sys_timerfd_settime
	PTR	sys_signalfd4
	PTR	sys_eventfd2
	PTR	sys_epoll_create1		/* 5285 */
	PTR	sys_dup3
	PTR	sys_pipe2
	PTR	sys_inotify_init1
	PTR	sys_preadv
	PTR	sys_pwritev			/* 5290 */
	PTR	sys_rt_tgsigqueueinfo
	PTR	sys_perf_event_open
	PTR	sys_accept4
	PTR	sys_recvmmsg
	PTR	sys_fanotify_init		/* 5295 */
	PTR	sys_fanotify_mark
	PTR	sys_ni_syscall
	PTR	sys_name_to_handle_at
	PTR	sys_open_by_handle_at
	PTR	sys_clock_adjtime		/* 5300 */
	PTR	sys_syncfs
	PTR	sys_sendmmsg
	PTR	sys_setns
	PTR	sys_process_vm_readv
	PTR	sys_process_vm_writev		/* 5305 */
	PTR	sys_kcmp
	PTR	sys_finit_module
	PTR	sys_getdents64
	PTR	sys_sched_setattr
	PTR	sys_sched_getattr		/* 5310 */
	PTR	sys_renameat2
	PTR	sys_seccomp
	PTR	sys_getrandom
	PTR	sys_memfd_create
	PTR	sys_bpf				/* 5315 */
	PTR	sys_execveat
	PTR	sys_userfaultfd
	PTR	sys_membarrier
	PTR	sys_mlock2
	PTR	sys_copy_file_range		/* 5320 */
	PTR	sys_preadv2
	PTR	sys_pwritev2
	PTR	sys_pkey_mprotect
	PTR	sys_pkey_alloc
	PTR	sys_pkey_free			/* 5325 */
	PTR	sys_statx
	PTR	sys_rseq
	PTR	sys_io_pgetevents
	PTR	sys_set_user_tp
SYM_DATA_END(mips64_syscall_table)
