/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Copyright 2014-2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef KCD_PRIV_H_INCLUDED
#define KCD_PRIV_H_INCLUDED

#include <linux/hashtable.h>
#include <linux/mmu_notifier.h>
#include <linux/memremap.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/idr.h>
#include <linux/kfifo.h>
#include <linux/seq_file.h>
#include <linux/kref.h>
#include <linux/sysfs.h>
#include <linux/device_cgroup.h>
#include <drm/drm_file.h>
#include <drm/drm_drv.h>
#include <drm/drm_device.h>
#include <drm/drm_ioctl.h>
#include <kgd_kcd_interface.h>
#include <linux/swap.h>
#include <linux/interval_tree.h>

#include "loonggpu.h"
#include "kcd_ioctl.h"

#define IP_VERSION(mj, mn, rv) (((mj) << 16) | ((mn) << 8) | (rv))

#define KCD_MAX_RING_ENTRY_SIZE	4

#define KCD_SYSFS_FILE_MODE 0444

/* GPU ID hash width in bits */
#define KCD_GPU_ID_HASH_WIDTH 16

/* Use upper bits of mmap offset to store KCD driver specific information.
 * BITS[62:61] - Encode MMAP type
 * BITS[60:45] - Encode gpu_id. To identify to which GPU the offset belongs to
 * BITS[44:0]  - MMAP offset value
 *
 * NOTE: struct vm_area_struct.vm_pgoff uses offset in pages. Hence, these
 *  defines are w.r.t to PAGE_SIZE
 */
#define KCD_MMAP_TYPE_SHIFT	61
#define KCD_MMAP_TYPE_MASK	(0x3ULL << KCD_MMAP_TYPE_SHIFT)
#define KCD_MMAP_TYPE_DOORBELL	(0x3ULL << KCD_MMAP_TYPE_SHIFT)
#define KCD_MMAP_TYPE_EVENTS	(0x2ULL << KCD_MMAP_TYPE_SHIFT)

#define KCD_MMAP_GPU_ID_SHIFT 45
#define KCD_MMAP_GPU_ID_MASK (((1ULL << KCD_GPU_ID_HASH_WIDTH) - 1) \
				<< KCD_MMAP_GPU_ID_SHIFT)
#define KCD_MMAP_GPU_ID(gpu_id) ((((uint64_t)gpu_id) << KCD_MMAP_GPU_ID_SHIFT)\
				& KCD_MMAP_GPU_ID_MASK)
#define KCD_MMAP_GET_GPU_ID(offset)    ((offset & KCD_MMAP_GPU_ID_MASK) \
				>> KCD_MMAP_GPU_ID_SHIFT)

/*
 * When working with cp scheduler we should assign the HIQ manually or via
 * the loonggpu driver to a fixed hqd slot, here are the fixed HIQ hqd slot
 * definitions for Kaveri. In Kaveri only the first ME queues participates
 * in the cp scheduling taking that in mind we set the HIQ slot in the
 * second ME.
 */
#define KCD_CIK_HIQ_PIPE 4
#define KCD_CIK_HIQ_QUEUE 0

/* Macro for allocating structures */
#define kcd_alloc_struct(ptr_to_struct)	\
	((typeof(ptr_to_struct)) kzalloc(sizeof(*ptr_to_struct), GFP_KERNEL))

#define KCD_MAX_NUM_OF_PROCESSES 512
#define KCD_MAX_NUM_OF_QUEUES_PER_PROCESS 1024

#define KCD_CWSR_TBA_TMA_SIZE (PAGE_SIZE * 2)
#define KCD_CWSR_TMA_OFFSET PAGE_SIZE

#define KCD_MAX_NUM_OF_QUEUES_PER_DEVICE		\
	(KCD_MAX_NUM_OF_PROCESSES *			\
			KCD_MAX_NUM_OF_QUEUES_PER_PROCESS)

#define KCD_KERNEL_QUEUE_SIZE 2048

#define KCD_UNMAP_LATENCY_MS	(4000)

#define KCD_MAX_SDMA_QUEUES	128

/*
 * 512 = 0x200
 * The doorbell index distance between SDMA RLC (2*i) and (2*i+1) in the
 * same SDMA engine on SOC15, which has 8-byte doorbells for SDMA.
 * 512 8-byte doorbell distance (i.e. one page away) ensures that SDMA RLC
 * (2*i+1) doorbells (in terms of the lower 12 bit address) lie exactly in
 * the OFFSET and SIZE set in registers like BIF_SDMA0_DOORBELL_RANGE.
 */
#define KCD_QUEUE_DOORBELL_MIRROR_OFFSET 512

/*
 * Kernel module parameter to specify maximum number of supported queues per
 * device
 */
extern int max_num_of_queues_per_device;


/* Kernel module parameter to specify the scheduling policy */
extern int sched_policy;

/*
 * Kernel module parameter to specify the maximum process
 * number per HW scheduler
 */
extern int hws_max_conc_proc;

/*
 * Kernel module parameter to specify whether to send sigterm to VDD process on
 * unhandled exception
 */
extern int send_sigterm;

/*
 * This kernel module is used to simulate large bar machine on non-large bar
 * enabled machines.
 */
extern int debug_largebar;

/* Halt if HWS hang is detected */
extern int halt_if_hws_hang;

/* Queue preemption timeout in ms */
extern int queue_preemption_timeout_ms;

/*
 * Don't evict process queues on vm fault
 */
extern int loonggpu_no_queue_eviction_on_vm_fault;

/* Enable eviction debug messages */
extern bool debug_evictions;

extern struct mutex kcd_processes_mutex;

extern int loonggpu_cwsr_enable;

struct kcd_node;

struct kcd_event_interrupt_class {
	bool (*interrupt_isr)(struct kcd_node *dev,
			const uint32_t *ih_ring_entry, uint32_t *patched_ihre,
			bool *patched_flag);
	void (*interrupt_wq)(struct kcd_node *dev,
			const uint32_t *ih_ring_entry);
};

struct kcd_device_info {
	uint32_t gfx_target_version;
	const struct kcd_event_interrupt_class *event_interrupt_class;
	unsigned int max_pasid_bits;
	unsigned int max_no_of_hqd;
	unsigned int doorbell_size;
	size_t ih_ring_entry_size;
	uint16_t mqd_size_aligned;
	bool supports_cwsr;
	bool needs_pci_atomics;
	uint32_t no_atomic_fw_version;
	unsigned int num_sdma_queues_per_engine;
	unsigned int num_reserved_sdma_queues_per_engine;
	DECLARE_BITMAP(reserved_sdma_queues_bitmap, KCD_MAX_SDMA_QUEUES);
};

unsigned int kcd_get_num_sdma_engines(struct kcd_node *kdev);

struct kcd_mem_obj {
	uint32_t range_start;
	uint32_t range_end;
	uint64_t gpu_addr;
	uint32_t *cpu_ptr;
	void *gtt_mem;
};

struct kcd_vmid_info {
	uint32_t first_vmid_kcd;
	uint32_t last_vmid_kcd;
	uint32_t vmid_num_kcd;
};

#define MAX_KCD_NODES	8

struct kcd_dev;

struct kcd_node {
	unsigned int node_id;
	struct loonggpu_device *adev;     /* Duplicated here along with keeping
					 * a copy in kcd_dev to save a hop
					 */
	const struct kcd2kgd_calls *kcd2kgd; /* Duplicated here along with
					      * keeping a copy in kcd_dev to
					      * save a hop
					      */
	struct kcd_vmid_info vm_info;
	unsigned int id;                /* topology stub index */

	/* Interrupts */
	struct kfifo ih_fifo;
	struct workqueue_struct *ih_wq;
	struct work_struct interrupt_work;
	spinlock_t interrupt_lock;

	/*
	 * Interrupts of interest to KCD are copied
	 * from the HW ring into a SW ring.
	 */
	bool interrupts_active;
	uint32_t interrupt_bitmap; /* Only used for GFX 9.4.3 */

	/* QCM Device instance */
	struct device_queue_manager *dqm;

	/* Clients watching SMI events */
	struct list_head smi_clients;
	spinlock_t smi_lock;
	uint32_t reset_seq_num;

	/* Maximum process number mapped to HW scheduler */
	unsigned int max_proc_per_quantum;

	unsigned int compute_vmid_bitmap;

	struct kcd_local_mem_info local_mem_info;

	struct kcd_dev *kcd;
};

struct kcd_dev {
	struct loonggpu_device *adev;

	struct kcd_device_info device_info;

	u64 __iomem *doorbell_kernel_ptr; /* This is a pointer for a doorbells
					   * page used by kernel queue
					   */

	struct kgd2kcd_shared_resources shared_resources;

	const struct kcd2kgd_calls *kcd2kgd;
	struct mutex doorbell_mutex;

	void *gtt_mem;
	uint64_t gtt_start_gpu_addr;
	void *gtt_start_cpu_ptr;
	void *gtt_sa_bitmap;
	struct mutex gtt_sa_lock;
	unsigned int gtt_sa_chunk_size;
	unsigned int gtt_sa_num_of_chunks;

	bool init_complete;

	/* Firmware versions */
	uint16_t fw_version;
	uint16_t sdma_fw_version;

	/* CWSR */
	bool cwsr_enabled;
	const void *cwsr_isa;
	unsigned int cwsr_isa_size;

	bool pci_atomic_requested;

	/* Compute Profile ref. count */
	atomic_t compute_profile;
	struct ida doorbell_ida;
	unsigned int max_doorbell_slices;

	int noretry;

	struct kcd_node *nodes[MAX_KCD_NODES];
	unsigned int num_nodes;

	/* Kernel doorbells for KCD device */
	bool has_doorbells;
	struct loonggpu_bo *doorbells;

	/* bitmap for dynamic doorbell allocation from doorbell object */
	unsigned long *doorbell_bitmap;
};

struct kcd_ipc_obj;

struct kcd_bo {
	void *mem;
	struct interval_tree_node it;
	struct kcd_node *dev;
	/* page-aligned VA address */
	uint64_t cpuva;
	unsigned int mem_type;
};

/* Character device interface */
int kcd_chardev_init(void);
void kcd_chardev_exit(void);

/**
 * enum kcd_unmap_queues_filter - Enum for queue filters.
 *
 * @KCD_UNMAP_QUEUES_FILTER_ALL_QUEUES: Preempts all queues in the
 *						running queues list.
 *
 * @KCD_UNMAP_QUEUES_FILTER_DYNAMIC_QUEUES: Preempts all non-static queues
 *						in the run list.
 *
 * @KCD_UNMAP_QUEUES_FILTER_BY_PASID: Preempts queues that belongs to
 *						specific process.
 *
 */
enum kcd_unmap_queues_filter {
	KCD_UNMAP_QUEUES_FILTER_ALL_QUEUES = 1,
	KCD_UNMAP_QUEUES_FILTER_DYNAMIC_QUEUES = 2,
	KCD_UNMAP_QUEUES_FILTER_BY_PASID = 3
};

/**
 * enum kcd_queue_type - Enum for various queue types.
 *
 * @KCD_QUEUE_TYPE_COMPUTE: Regular user mode queue type.
 *
 * @KCD_QUEUE_TYPE_SDMA: SDMA user mode queue type.
 *
 * @KCD_QUEUE_TYPE_HIQ: HIQ queue type.
 */
enum kcd_queue_type  {
	KCD_QUEUE_TYPE_COMPUTE,
	KCD_QUEUE_TYPE_SDMA,
	KCD_QUEUE_TYPE_HIQ
};

enum kcd_queue_format {
	KCD_QUEUE_FORMAT_PM4
};

enum KCD_QUEUE_PRIORITY {
	KCD_QUEUE_PRIORITY_MINIMUM = 0,
	KCD_QUEUE_PRIORITY_MAXIMUM = 15
};

/**
 * struct queue_properties
 *
 * @type: The queue type.
 *
 * @queue_id: Queue identifier.
 *
 * @queue_address: Queue ring buffer address.
 *
 * @queue_size: Queue ring buffer size.
 *
 * @priority: Defines the queue priority relative to other queues in the
 * process.
 * This is just an indication and HW scheduling may override the priority as
 * necessary while keeping the relative prioritization.
 * the priority granularity is from 0 to f which f is the highest priority.
 * currently all queues are initialized with the highest priority.
 *
 * @queue_percent: This field is partially implemented and currently a zero in
 * this field defines that the queue is non active.
 *
 * @read_ptr: User space address which points to the number of dwords the
 * cp read from the ring buffer. This field updates automatically by the H/W.
 *
 * @write_ptr: Defines the number of dwords written to the ring buffer.
 *
 * @is_interop: Defines if this is a interop queue. Interop queue means that
 * the queue can access both graphics and compute resources.
 *
 * @is_evicted: Defines if the queue is evicted. Only active queues
 * are evicted, rendering them inactive.
 *
 * @is_active: Defines if the queue is active or not. @is_active and
 * @is_evicted are protected by the DQM lock.
 *
 * @vmid: If the scheduling mode is no cp scheduling the field defines the vmid
 * of the queue.
 *
 * This structure represents the queue properties for each queue no matter if
 * it's user mode or kernel mode queue.
 *
 */

struct queue_properties {
	enum kcd_queue_type type;
	enum kcd_queue_format format;
	unsigned int queue_id;
	uint64_t queue_address;
	uint64_t  queue_size;
	uint32_t priority;
	uint32_t queue_percent;
	uint32_t *read_ptr;
	uint32_t *write_ptr;
	void __iomem *doorbell_ptr;
	uint32_t doorbell_off;
	bool is_interop;
	bool is_evicted;
	bool is_suspended;
	bool is_being_destroyed;
	bool is_active;
	/* Not relevant for user mode queues in cp scheduling */
	unsigned int vmid;
	/* Relevant only for sdma queues*/
	uint32_t sdma_engine_id;
	uint32_t sdma_queue_id;
	uint32_t sdma_vm_addr;

	uint64_t ctx_save_restore_area_address;
	uint32_t ctx_save_restore_area_size;

	uint64_t exception_status;
};

#define QUEUE_IS_ACTIVE(q) ((q).queue_size > 0 &&	\
			    (q).queue_address != 0 &&	\
			    (q).queue_percent > 0 &&	\
			    !(q).is_evicted &&		\
			    !(q).is_suspended)

/**
 * struct queue
 *
 * @list: Queue linked list.
 *
 * @mqd: The queue MQD (memory queue descriptor).
 *
 * @mqd_mem_obj: The MQD local gpu memory object.
 *
 * @gart_mqd_addr: The MQD gart mc address.
 *
 * @properties: The queue properties.
 *
 * @mec: Used only in no cp scheduling mode and identifies to micro engine id
 *	 that the queue should be executed on.
 *
 * @pipe: Used only in no cp scheduling mode and identifies the queue's pipe
 *	  id.
 *
 * @queue: Used only in no cp scheduliong mode and identifies the queue's slot.
 *
 * @process: The kcd process that created this queue.
 *
 * @device: The kcd device that created this queue.
 *
 * This structure represents user mode compute queues.
 * It contains all the necessary data to handle such queues.
 *
 */

struct queue {
	struct list_head list;
	void *mqd;
	struct kcd_mem_obj *mqd_mem_obj;
	uint64_t gart_mqd_addr;
	struct queue_properties properties;

	uint32_t mec;
	uint32_t pipe;
	uint32_t queue;

	unsigned int sdma_id;
	unsigned int doorbell_id;

	struct kcd_process	*process;
	struct kcd_node		*device;

	/* procfs */
	struct kobject kobj;
};

enum KCD_MQD_TYPE {
	KCD_MQD_TYPE_CP = 0,		/* for cp queues */
	KCD_MQD_TYPE_SDMA,		/* for sdma queues */
	KCD_MQD_TYPE_HIQ,		/* for hiq */
	KCD_MQD_TYPE_MAX
};

enum KCD_PIPE_PRIORITY {
	KCD_PIPE_PRIORITY_CS_LOW = 0,
	KCD_PIPE_PRIORITY_CS_MEDIUM,
	KCD_PIPE_PRIORITY_CS_HIGH
};

struct process_queue_manager {
	/* data */
	struct kcd_process	*process;
	struct list_head	queues;
	unsigned long		*queue_slot_bitmap;
};

struct qcm_process_device {
	/* The Device Queue Manager that owns this data */
	struct device_queue_manager *dqm;
	struct process_queue_manager *pqm;
	/* Queues list */
	struct list_head queues_list;
	struct list_head priv_queue_list;

	unsigned int queue_count;
	unsigned int vmid;
	bool is_debug;
	unsigned int evicted; /* eviction counter, 0=active */

	/* This flag tells if we should reset all wavefronts on
	 * process termination
	 */
	bool reset_wavefronts;

	/* Contains page table flags such as LOONGGPU_PTE_VALID since gfx9 */
	uint64_t page_table_base;

	/* CWSR memory */
	struct kgd_mem *cwsr_mem;
	void *cwsr_kaddr;
	uint64_t cwsr_base;
	uint64_t tba_addr;
	uint64_t tma_addr;

	/* doorbells for kcd process */
	struct loonggpu_bo *proc_doorbells;

	/* bitmap for dynamic doorbell allocation from the bo */
	unsigned long *doorbell_bitmap;
};

/* KCD Memory Eviction */

/* Approx. wait time before attempting to restore evicted BOs */
#define PROCESS_RESTORE_TIME_MS 100
/* Approx. back off time if restore fails due to lack of memory */
#define PROCESS_BACK_OFF_TIME_MS 100
/* Approx. time before evicting the process again */
#define PROCESS_ACTIVE_TIME_MS 10

/* 8 byte handle containing GPU ID in the most significant 4 bytes and
 * idr_handle in the least significant 4 bytes
 */
#define MAKE_HANDLE(gpu_id, idr_handle) \
	(((uint64_t)(gpu_id) << 32) + idr_handle)
#define GET_GPU_ID(handle) (handle >> 32)
#define GET_IDR_HANDLE(handle) (handle & 0xFFFFFFFF)

enum kcd_pdd_bound {
	PDD_UNBOUND = 0,
	PDD_BOUND,
	PDD_BOUND_SUSPENDED,
};

#define MAX_SYSFS_FILENAME_LEN 15

/*
 * SDMA counter runs at 100MHz frequency.
 * We display SDMA activity in microsecond granularity in sysfs.
 * As a result, the divisor is 100.
 */
#define SDMA_ACTIVITY_DIVISOR  100

/* Data that is per-process-per device. */
struct kcd_process_device {
	/* The device that owns this data. */
	struct kcd_node *dev;

	/* The process that owns this kcd_process_device. */
	struct kcd_process *process;

	/* per-process-per device QCM data structure */
	struct qcm_process_device qpd;

	/*Apertures*/
	uint64_t lds_base;
	uint64_t lds_limit;
	uint64_t gpuvm_base;
	uint64_t gpuvm_limit;
	uint64_t scratch_base;
	uint64_t scratch_limit;

	/* VM context for GPUVM allocations */
	struct file *drm_file;
	void *drm_priv;

	/* GPUVM allocations storage */
	struct idr alloc_idr;

	/* Flag used to tell the pdd has dequeued from the dqm.
	 * This is used to prevent dev->dqm->ops.process_termination() from
	 * being called twice when it is already called in IOMMU callback
	 * function.
	 */
	bool already_dequeued;
	bool runtime_inuse;

	/* Is this process/pasid bound to this device? (loongson_iommu_bind_pasid) */
	enum kcd_pdd_bound bound;

	/* VRAM usage */
	uint64_t vram_usage;
	struct attribute attr_vram;
	char vram_filename[MAX_SYSFS_FILENAME_LEN];

	/* SDMA activity tracking */
	uint64_t sdma_past_activity_counter;
	struct attribute attr_sdma;
	char sdma_filename[MAX_SYSFS_FILENAME_LEN];

	/* Eviction activity tracking */
	uint64_t last_evict_timestamp;
	atomic64_t evict_duration_counter;
	struct attribute attr_evict;

	struct kobject *kobj_stats;

	/*
	 * @cu_occupancy: Reports occupancy of Compute Units (CU) of a process
	 * that is associated with device encoded by "this" struct instance. The
	 * value reflects CU usage by all of the waves launched by this process
	 * on this device. A very important property of occupancy parameter is
	 * that its value is a snapshot of current use.
	 *
	 * Following is to be noted regarding how this parameter is reported:
	 *
	 *  The number of waves that a CU can launch is limited by couple of
	 *  parameters. These are encoded by struct loonggpu_cu_info instance
	 *  that is part of every device definition. For GFX9 devices this
	 *  translates to 40 waves (simd_per_cu * max_waves_per_simd) when waves
	 *  do not use scratch memory and 32 waves (max_scratch_slots_per_cu)
	 *  when they do use scratch memory. This could change for future
	 *  devices and therefore this example should be considered as a guide.
	 *
	 *  All CU's of a device are available for the process. This may not be true
	 *  under certain conditions - e.g. CU masking.
	 *
	 *  Finally number of CU's that are occupied by a process is affected by both
	 *  number of CU's a device has along with number of other competing processes
	 */
	struct attribute attr_cu_occupancy;

	/* sysfs counters for GPU retry fault and page migration tracking */
	struct kobject *kobj_counters;
	struct attribute attr_faults;
	struct attribute attr_page_in;
	struct attribute attr_page_out;
	uint64_t faults;
	uint64_t page_in;
	uint64_t page_out;

	/* Exception code status*/
	uint64_t exception_status;

	/*
	 * If this process has been checkpointed before, then the user
	 * application will use the original gpu_id on the
	 * checkpointed node to refer to this device.
	 */
	uint32_t user_gpu_id;
};

#define qpd_to_pdd(x) container_of(x, struct kcd_process_device, qpd)
#define MAX_GPU_INSTANCE	64

struct svm_range_list {
	struct mutex			lock;
	struct rb_root_cached		objects;
	struct list_head		list;
	struct work_struct		deferred_list_work;
	struct list_head		deferred_range_list;
	struct list_head                criu_svm_metadata_list;
	spinlock_t			deferred_list_lock;
	atomic_t			evicted_ranges;
	atomic_t			drain_pagefaults;
	struct delayed_work		restore_work;
	DECLARE_BITMAP(bitmap_supported, MAX_GPU_INSTANCE);
	struct task_struct		*faulting_task;
};

/* Process data */
struct kcd_process {
	/*
	 * kcd_process are stored in an mm_struct*->kcd_process*
	 * hash table (kcd_processes in kcd_process.c)
	 */
	struct hlist_node kcd_processes;

	/*
	 * Opaque pointer to mm_struct. We don't hold a reference to
	 * it so it should never be dereferenced from here. This is
	 * only used for looking up processes by their mm.
	 */
	void *mm;

	struct kref ref;
	struct work_struct release_work;

	struct mutex mutex;

	/*
	 * In any process, the thread that started main() is the lead
	 * thread and outlives the rest.
	 * It is here because loongson_iommu_bind_pasid wants a task_struct.
	 * It can also be used for safely getting a reference to the
	 * mm_struct of the process.
	 */
	struct task_struct *lead_thread;

	/* We want to receive a notification when the mm_struct is destroyed */
	struct mmu_notifier mmu_notifier;

	u32 pasid;

	/*
	 * Array of kcd_process_device pointers,
	 * one for each device the process is using.
	 */
	struct kcd_process_device *pdds[MAX_GPU_INSTANCE];
	uint32_t n_pdds;

	struct process_queue_manager pqm;

	/*Is the user space process 32 bit?*/
	bool is_32bit_user_mode;

	/* Event-related data */
	struct mutex event_mutex;
	/* Event ID allocator and lookup */
	struct idr event_idr;
	/* Event page */
	u64 signal_handle;
	struct kcd_signal_page *signal_page;
	size_t signal_mapped_size;
	size_t signal_event_count;
	bool signal_event_limit_reached;

	struct rb_root_cached bo_interval_tree;

	/* Information used for memory eviction */
	void *kgd_process_info;
	/* Eviction fence that is attached to all the BOs of this process. The
	 * fence will be triggered during eviction and new one will be created
	 * during restore
	 */
	struct dma_fence *ef;

	/* Work items for evicting and restoring BOs */
	struct delayed_work eviction_work;
	struct delayed_work restore_work;
	/* seqno of the last scheduled eviction */
	unsigned int last_eviction_seqno;
	/* Approx. the last timestamp (in jiffies) when the process was
	 * restored after an eviction
	 */
	unsigned long last_restore_timestamp;

	/* Indicates device process is debug attached with reserved vmid. */
	bool debug_trap_enabled;

	/* per-process-per device debug event fd file */
	struct file *dbg_ev_file;

	/* If the process is a kcd debugger, we need to know so we can clean
	 * up at exit time.  If a process enables debugging on itself, it does
	 * its own clean-up, so we don't set the flag here.  We track this by
	 * counting the number of processes this process is debugging.
	 */
	atomic_t debugged_process_count;

	/* If the process is a debugged, this is the debugger process */
	struct kcd_process *debugger_process;

	/* Kobj for our procfs */
	struct kobject *kobj;
	struct kobject *kobj_queues;
	struct attribute attr_pasid;

	/* Exception code enable mask and status */
	uint64_t exception_enable_mask;
	uint64_t exception_status;

	/* Used to drain stale interrupts */
	wait_queue_head_t wait_irq_drain;
	bool irq_drain_is_open;

	/* shared virtual memory registered by this process */
	struct svm_range_list svms;

	bool xnack_enabled;

	/* Work area for debugger event writer worker. */
	struct work_struct debug_event_workarea;

	/* Tracks debug per-vmid request for debug flags */
	bool dbg_flags;

	atomic_t poison;

	/* Tracks runtime enable status */
	struct semaphore runtime_enable_sema;
	bool is_runtime_retry;
	struct kcd_runtime_info runtime_info;
	/* Use for delayed freeing of kcd_process structure */
	struct rcu_head	rcu;
};

#define KCD_PROCESS_TABLE_SIZE 5 /* bits: 32 entries */
extern DECLARE_HASHTABLE(kcd_processes_table, KCD_PROCESS_TABLE_SIZE);
extern struct srcu_struct kcd_processes_srcu;

/**
 * typedef lgkcd_ioctl_t - typedef for ioctl function pointer.
 *
 * @filep: pointer to file structure.
 * @p: lgkcd process pointer.
 * @data: pointer to arg that was copied from user.
 *
 * Return: returns ioctl completion code.
 */
typedef int lgkcd_ioctl_t(struct file *filep, struct kcd_process *p,
				void *data);

struct lgkcd_ioctl_desc {
	unsigned int cmd;
	int flags;
	lgkcd_ioctl_t *func;
	unsigned int cmd_drv;
	const char *name;
};
bool kcd_dev_is_large_bar(struct kcd_node *dev);

int kcd_process_create_wq(void);
void kcd_process_destroy_wq(void);
void kcd_cleanup_processes(void);
struct kcd_process *kcd_create_process(struct task_struct *thread);
struct kcd_process *kcd_get_process(const struct task_struct *task);
struct kcd_process *kcd_lookup_process_by_pasid(u32 pasid);
struct kcd_process *kcd_lookup_process_by_mm(const struct mm_struct *mm);

int kcd_process_gpuidx_from_gpuid(struct kcd_process *p, uint32_t gpu_id);
int kcd_process_gpuid_from_node(struct kcd_process *p, struct kcd_node *node,
				uint32_t *gpuid, uint32_t *gpuidx);
static inline int kcd_process_gpuid_from_gpuidx(struct kcd_process *p,
				uint32_t gpuidx, uint32_t *gpuid) {
	return gpuidx < p->n_pdds ? p->pdds[gpuidx]->dev->id : -EINVAL;
}
static inline struct kcd_process_device *kcd_process_device_from_gpuidx(
				struct kcd_process *p, uint32_t gpuidx) {
	return gpuidx < p->n_pdds ? p->pdds[gpuidx] : NULL;
}

void kcd_unref_process(struct kcd_process *p);
int kcd_process_evict_queues(struct kcd_process *p, uint32_t trigger);
int kcd_process_restore_queues(struct kcd_process *p);
void kcd_suspend_all_processes(void);
int kcd_resume_all_processes(void);

struct kcd_process_device *kcd_process_device_data_by_id(struct kcd_process *process,
							 uint32_t gpu_id);

int kcd_process_get_user_gpu_id(struct kcd_process *p, uint32_t actual_gpu_id);

int kcd_process_device_init_vm(struct kcd_process_device *pdd,
			       struct file *drm_file);
struct kcd_process_device *kcd_bind_process_to_device(struct kcd_node *dev,
						struct kcd_process *p);
struct kcd_process_device *kcd_get_process_device_data(struct kcd_node *dev,
							struct kcd_process *p);
struct kcd_process_device *kcd_create_process_device_data(struct kcd_node *dev,
							struct kcd_process *p);

bool kcd_process_xnack_mode(struct kcd_process *p, bool supported);

/* KCD process API for creating and translating handles */
int kcd_process_device_create_obj_handle(struct kcd_process_device *pdd,
					void *mem, uint64_t start,
					uint64_t length, uint64_t cpuva,
					unsigned int mem_type,
					int preferred_id);
void *kcd_process_device_translate_handle(struct kcd_process_device *p,
					int handle);
struct kcd_bo *kcd_process_device_find_bo(struct kcd_process_device *pdd,
					int handle);
void kcd_process_device_remove_obj_handle(struct kcd_process_device *pdd,
					int handle);
struct kcd_process *kcd_lookup_process_by_pid(struct pid *pid);

/* PASIDs */
int kcd_pasid_init(void);
void kcd_pasid_exit(void);
bool kcd_set_pasid_limit(unsigned int new_limit);
unsigned int kcd_get_pasid_limit(void);
u32 kcd_pasid_alloc(void);
void kcd_pasid_free(u32 pasid);

/* Doorbells */
size_t kcd_doorbell_process_slice(struct kcd_dev *kcd);
int kcd_doorbell_init(struct kcd_dev *kcd);
void kcd_doorbell_fini(struct kcd_dev *kcd);
int kcd_doorbell_mmap(struct kcd_node *dev, struct kcd_process *process,
		      struct vm_area_struct *vma);
void __iomem *kcd_get_kernel_doorbell(struct kcd_dev *kcd,
					unsigned int *doorbell_off);
void kcd_release_kernel_doorbell(struct kcd_dev *kcd, u64 __iomem *db_addr);
void write_kernel_doorbell64(void __iomem *db, u64 value);
unsigned int kcd_get_doorbell_dw_offset_in_bar(struct kcd_dev *kcd,
					struct kcd_process_device *pdd,
					unsigned int doorbell_id);
phys_addr_t kcd_get_process_doorbells(struct kcd_process_device *pdd);
int kcd_alloc_process_doorbells(struct kcd_dev *kcd,
				struct kcd_process_device *pdd);
void kcd_free_process_doorbells(struct kcd_dev *kcd,
				struct kcd_process_device *pdd);

/* GTT Sub-Allocator */

int kcd_gtt_sa_allocate(struct kcd_node *node, unsigned int size,
			struct kcd_mem_obj **mem_obj);

int kcd_gtt_sa_free(struct kcd_node *node, struct kcd_mem_obj *mem_obj);

extern struct device *kcd_device;

/* KCD's procfs */
void kcd_procfs_init(void);
void kcd_procfs_shutdown(void);
int kcd_procfs_add_queue(struct queue *q);
void kcd_procfs_del_queue(struct queue *q);

/* Topology */
int kcd_topology_init(void);
void kcd_topology_shutdown(void);
int kcd_topology_add_device(struct kcd_node *gpu);
int kcd_topology_remove_device(struct kcd_node *gpu);
struct kcd_topology_device *kcd_topology_device_by_proximity_domain(
						uint32_t proximity_domain);
struct kcd_topology_device *kcd_topology_device_by_proximity_domain_no_lock(
						uint32_t proximity_domain);
struct kcd_topology_device *kcd_topology_device_by_id(uint32_t gpu_id);
struct kcd_node *kcd_device_by_id(uint32_t gpu_id);
struct kcd_node *kcd_device_by_pci_dev(const struct pci_dev *pdev);
static inline bool kcd_irq_is_from_node(struct kcd_node *node, uint32_t node_id,
					uint32_t vmid)
{
	return true;
}
static inline struct kcd_node *kcd_node_by_irq_ids(struct loonggpu_device *adev,
					uint32_t node_id, uint32_t vmid) {
	struct kcd_dev *dev = adev->kcd.dev;
	uint32_t i;

	for (i = 0; i < dev->num_nodes; i++)
		if (kcd_irq_is_from_node(dev->nodes[i], node_id, vmid))
			return dev->nodes[i];

	return NULL;
}
int kcd_topology_enum_kcd_devices(uint8_t idx, struct kcd_node **kdev);
int kcd_numa_node_to_apic_id(int numa_node_id);

/* Interrupts */
#define	KCD_IRQ_FENCE_CLIENTID	0xff
#define	KCD_IRQ_FENCE_SOURCEID	0xff
#define	KCD_IRQ_IS_FENCE(client, source)				\
				((client) == KCD_IRQ_FENCE_CLIENTID &&	\
				(source) == KCD_IRQ_FENCE_SOURCEID)
int kcd_interrupt_init(struct kcd_node *dev);
void kcd_interrupt_exit(struct kcd_node *dev);
bool enqueue_ih_ring_entry(struct kcd_node *kcd, const void *ih_ring_entry);
bool interrupt_is_wanted(struct kcd_node *dev,
				const uint32_t *ih_ring_entry,
				uint32_t *patched_ihre, bool *flag);
int kcd_process_drain_interrupts(struct kcd_process_device *pdd);
void kcd_process_close_interrupt_drain(unsigned int pasid);

/* lgkcd Apertures */
int kcd_init_apertures(struct kcd_process *process);

void kcd_process_set_trap_handler(struct qcm_process_device *qpd,
				  uint64_t tba_addr,
				  uint64_t tma_addr);
void kcd_process_set_trap_debug_flag(struct qcm_process_device *qpd,
				     bool enabled);

int kcd_process_get_queue_info(struct kcd_process *p,
			       uint32_t *num_queues,
			       uint64_t *priv_data_sizes);

/* Queue Context Management */
int init_queue(struct queue **q, const struct queue_properties *properties);
void uninit_queue(struct queue *q);
void print_queue_properties(struct queue_properties *q);
void print_queue(struct queue *q);

struct mqd_manager *mqd_manager_init_lg2xx(enum KCD_MQD_TYPE type,
		struct kcd_node *dev);
struct device_queue_manager *device_queue_manager_init(struct kcd_node *dev);
void device_queue_manager_uninit(struct device_queue_manager *dqm);
struct kernel_queue *kernel_queue_init(struct kcd_node *dev,
					enum kcd_queue_type type);
void kernel_queue_uninit(struct kernel_queue *kq, bool hanging);
int kcd_dqm_evict_pasid(struct device_queue_manager *dqm, u32 pasid);

/* Process Queue Manager */
struct process_queue_node {
	struct queue *q;
	struct kernel_queue *kq;
	struct list_head process_queue_list;
};

void kcd_process_dequeue_from_device(struct kcd_process_device *pdd);
void kcd_process_dequeue_from_all_devices(struct kcd_process *p);
int pqm_init(struct process_queue_manager *pqm, struct kcd_process *p);
void pqm_uninit(struct process_queue_manager *pqm);
int pqm_create_queue(struct process_queue_manager *pqm,
			    struct kcd_node *dev,
			    struct file *f,
			    struct queue_properties *properties,
			    unsigned int *qid,
			    const void *restore_mqd,
			    uint32_t *p_doorbell_offset_in_process);
int pqm_destroy_queue(struct process_queue_manager *pqm, unsigned int qid);
int pqm_update_queue_properties(struct process_queue_manager *pqm, unsigned int qid,
			struct queue_properties *p);
int pqm_update_mqd(struct process_queue_manager *pqm, unsigned int qid);
struct kernel_queue *pqm_get_kernel_queue(struct process_queue_manager *pqm,
						unsigned int qid);
struct queue *pqm_get_user_queue(struct process_queue_manager *pqm,
						unsigned int qid);
int pqm_get_queue_snapshot(struct process_queue_manager *pqm,
			   uint64_t exception_clear_mask,
			   void __user *buf,
			   int *num_qss_entries,
			   uint32_t *entry_size);

int lgkcd_fence_wait_timeout(uint64_t *fence_addr,
			      uint64_t fence_value,
			      unsigned int timeout_ms);
/* Packet Manager */

#define KCD_FENCE_COMPLETED (100)
#define KCD_FENCE_INIT   (10)

struct packet_manager {
	struct device_queue_manager *dqm;
	struct kernel_queue *priv_queue;
	struct mutex lock;
	bool allocated;
	struct kcd_mem_obj *ib_buffer_obj;
	unsigned int ib_size_bytes;
	bool is_over_subscription;

	const struct packet_manager_funcs *pmf;
};

struct packet_manager_funcs {
	/* Support ASIC-specific packet formats for PM4 packets */
	int (*map_process)(struct packet_manager *pm, uint32_t *buffer,
			struct qcm_process_device *qpd);
	int (*map_queues)(struct packet_manager *pm, uint32_t *buffer,
			struct queue *q, bool is_static);
	int (*unmap_queues)(struct packet_manager *pm, uint32_t *buffer,
			enum kcd_unmap_queues_filter mode,
			uint32_t filter_param, bool reset);
	int (*query_status)(struct packet_manager *pm, uint32_t *buffer,
			uint64_t fence_address,	uint64_t fence_value);
	int (*submit_queue)(struct packet_manager *pm, uint32_t *buffer,
			uint32_t pasid, uint32_t queue_id);

	/* Packet sizes */
	int map_process_size;
	int map_queues_size;
	int unmap_queues_size;
	int query_status_size;
	int submit_queue_size;
};

extern const struct packet_manager_funcs kcd_lg2xx_pm_funcs;

int pm_init(struct packet_manager *pm, struct device_queue_manager *dqm);
void pm_uninit(struct packet_manager *pm, bool hanging);
int pm_send_direct(struct packet_manager *pm, struct list_head *dqm_queues);
int pm_send_query_status(struct packet_manager *pm, uint64_t fence_address,
				uint64_t fence_value);

int pm_send_unmap_queue(struct packet_manager *pm,
			enum kcd_unmap_queues_filter mode,
			uint32_t filter_param, bool reset);

void pm_release_ib(struct packet_manager *pm);

int pm_submit_queue(struct packet_manager *pm, uint32_t pasid, uint32_t queue_id);

/* Following PM funcs can be shared among VI and AI */

uint64_t kcd_get_number_elems(struct kcd_dev *kcd);

/* Events */
extern const struct kcd_event_interrupt_class event_interrupt_class_lg2xx;

extern const struct kcd_device_global_init_class device_global_init_class_cik;

int kcd_event_init_process(struct kcd_process *p);
void kcd_event_free_process(struct kcd_process *p);
int kcd_event_mmap(struct kcd_process *process, struct vm_area_struct *vma);
int kcd_wait_on_events(struct kcd_process *p,
		       uint32_t num_events, void __user *data,
		       bool all, uint32_t *user_timeout_ms,
		       uint32_t *wait_result);
void kcd_signal_event_interrupt(u32 pasid, uint32_t partial_id,
				uint32_t valid_id_bits);
void kcd_signal_hw_exception_event(u32 pasid);
int kcd_set_event(struct kcd_process *p, uint32_t event_id);
int kcd_reset_event(struct kcd_process *p, uint32_t event_id);
int kcd_kmap_event_page(struct kcd_process *p, uint64_t event_page_offset);

int kcd_event_create(struct file *devkcd, struct kcd_process *p,
		     uint32_t event_type, bool auto_reset, uint32_t node_id,
		     uint32_t *event_id, uint32_t *event_trigger_data,
		     uint64_t *event_page_offset, uint32_t *event_slot_index);

int kcd_get_num_events(struct kcd_process *p);
int kcd_event_destroy(struct kcd_process *p, uint32_t event_id);

void kcd_signal_vm_fault_event(struct kcd_node *dev, u32 pasid,
				struct kcd_vm_fault_info *info,
				struct kcd_vdd_memory_exception_data *data);

void kcd_signal_reset_event(struct kcd_node *dev);

void kcd_signal_poison_consumed_event(struct kcd_node *dev, u32 pasid);

void kcd_flush_tlb(struct kcd_process_device *pdd, enum TLB_FLUSH_TYPE type);

static inline bool kcd_flush_tlb_after_unmap(struct kcd_dev *dev)
{
	return true;
}

int kcd_send_exception_to_runtime(struct kcd_process *p,
				unsigned int queue_id,
				uint64_t error_reason);
bool kcd_is_locked(void);

/* IPC Support */
int kcd_ipc_init(void);

/* Compute profile */
void kcd_inc_compute_active(struct kcd_node *dev);
void kcd_dec_compute_active(struct kcd_node *dev);
static inline int kcd_devcgroup_check_permission(struct kcd_node *kcd)
{
	return 0;
}

static inline bool kcd_is_first_node(struct kcd_node *node)
{
	return (node == node->kcd->nodes[0]);
}

/* Debugfs */
#if defined(CONFIG_DEBUG_FS)

void kcd_debugfs_init(void);
void kcd_debugfs_fini(void);
int kcd_debugfs_mqds_by_process(struct seq_file *m, void *data);
int pqm_debugfs_mqds(struct seq_file *m, void *data);

int kcd_debugfs_hang_hws(struct kcd_node *dev);
int pm_debugfs_hang_hws(struct packet_manager *pm);
int dqm_debugfs_hang_hws(struct device_queue_manager *dqm);

#else

static inline void kcd_debugfs_init(void) {}
static inline void kcd_debugfs_fini(void) {}

#endif

#endif
