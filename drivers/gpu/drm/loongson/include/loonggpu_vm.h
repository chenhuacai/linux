#ifndef __LOONGGPU_VM_H__
#define __LOONGGPU_VM_H__

#include <linux/idr.h>
#include <linux/kfifo.h>
#include <linux/rbtree.h>
#include <drm/gpu_scheduler.h>
#include <drm/drm_file.h>

#include "loonggpu_sync.h"
#include "loonggpu_ring.h"
#include "loonggpu_ids.h"

struct loonggpu_bo_va;
struct loonggpu_job;
struct loonggpu_bo_list_entry;

#define LOONGGPU_VM_PDE_PTE_BYTES      8

/* Maximum number of PTEs can write with one job */
#define LOONGGPU_VM_MAX_UPDATE_SIZE	(1024ull)

/* The number of PTEs a block contains */
#define LOONGGPU_VM_PTE_COUNT(adev) (1 << (adev)->vm_manager.block_size)

/* PTBs (Page Table Blocks) need to be aligned to 32K */
#define LOONGGPU_VM_PTB_ALIGN_SIZE   32768

/* Reserve 8MB VRAM for page tables */
#define LOONGGPU_VM_RESERVED_VRAM		(8ULL << 20)

#define LOONGGPU_PTE_PRESENT	(1ULL << 0)
#define LOONGGPU_PTE_HUGEPAGE	(1ULL << 1)
#define LOONGGPU_PTE_EXCEPTION	(1ULL << 2)
#define LOONGGPU_PTE_WRITEABLE	(1ULL << 3)
/* comprssed pte flags use bit 4 to 6 */
#define LOONGGPU_PTE_COMPRESSED_SHIFT (5)

/* How to programm VM fault handling */
#define LOONGGPU_VM_FAULT_STOP_NEVER	0
#define LOONGGPU_VM_FAULT_STOP_FIRST	1
#define LOONGGPU_VM_FAULT_STOP_ALWAYS	2

/* hardcode that limit for now */
#define LOONGGPU_VA_RESERVED_SIZE			(1ULL << 20)

/* VA hole for 48bit addresses on Vega10 */
#define LOONGGPU_VA_HOLE_START			0x0000800000000000ULL
#define LOONGGPU_VA_HOLE_END			0xffff800000000000ULL

/*
 * Hardware is programmed as if the hole doesn't exists with start and end
 * address values.
 *
 * This mask is used to remove the upper 16bits of the VA and so come up with
 * the linear addr value.
 */
#define LOONGGPU_VA_HOLE_MASK			0x0000ffffffffffffULL

/* max vmids dedicated for process */
#define LOONGGPU_VM_MAX_RESERVED_VMID	1

#define LOONGGPU_VM_CONTEXT_GFX 0
#define LOONGGPU_VM_CONTEXT_COMPUTE 1

/**
 * struct loonggpu_pte_update_params - Local structure
 *
 * Encapsulate some VM table update parameters to reduce
 * the number of function parameters
 *
 */
struct loonggpu_pte_update_params {

	/**
	 * @ldev: loonggpu device we do this update for
	 */
	struct loonggpu_device *ldev;

	/**
	 * @vm: optional loonggpu_vm we do this update for
	 */
	struct loonggpu_vm *vm;

	/**
	 * @src: address where to copy page table entries from
	 */
	u64 src;

	/**
	 * @ib: indirect buffer to fill with commands
	 */
	struct loonggpu_ib *ib;

	/**
	 * @func: Function which actually does the update
	 */
	void (*func)(struct loonggpu_pte_update_params *params,
		     struct loonggpu_bo *bo, u64 pe,
		     u64 addr, unsigned count, u32 incr,
		     u64 flags);
	/**
	 * @pages_addr:
	 *
	 * DMA addresses to use for mapping, used during VM update by CPU
	 */
	dma_addr_t *pages_addr;

	/**
	 * @kptr:
	 *
	 * Kernel pointer of PD/PT BO that needs to be updated,
	 * used during VM update by CPU
	 */
	void *kptr;
};

/**
 * struct loonggpu_prt_cb - Helper to disable partial resident texture feature from a fence callback
 */
struct loonggpu_prt_cb {

	/**
	 * @ldev: loonggpu device
	 */
	struct loonggpu_device *ldev;

	/**
	 * @cb: callback
	 */
	struct dma_fence_cb cb;
};

/* VMPT level enumerate, and the hiberachy is:
 * DIR0->DIR1->DIR2
 */
enum loonggpu_vm_level {
	LOONGGPU_VM_DIR0,
	LOONGGPU_VM_DIR1,
	LOONGGPU_VM_DIR2
};

/* base structure for tracking BO usage in a VM */
struct loonggpu_vm_bo_base {
	/* constant after initialization */
	struct loonggpu_vm		*vm;
	struct loonggpu_bo		*bo;

	/* protected by bo being reserved */
	struct list_head		bo_list;

	/* protected by spinlock */
	struct list_head		vm_status;

	/* protected by the BO being reserved */
	bool				moved;
};

struct loonggpu_vm_pt {
	struct loonggpu_vm_bo_base	base;
	bool				huge;

	/* array of page tables, one for each directory entry */
	struct loonggpu_vm_pt		*entries;
};

#define LOONGGPU_VM_FAULT(pasid, addr) (((u64)(pasid) << 48) | (addr))
#define LOONGGPU_VM_FAULT_PASID(fault) ((u64)(fault) >> 48)
#define LOONGGPU_VM_FAULT_ADDR(fault)  ((u64)(fault) & 0xfffffffff000ULL)


struct loonggpu_task_info {
	char	process_name[TASK_COMM_LEN];
	char	task_name[TASK_COMM_LEN];
	pid_t	pid;
	pid_t	tgid;
};

struct loonggpu_vm {
	/* tree of virtual addresses mapped */
	struct rb_root_cached	va;

	/* BOs who needs a validation */
	struct list_head	evicted;

	/* PT BOs which relocated and their parent need an update */
	struct list_head	relocated;

	/* BOs moved, but not yet updated in the PT */
	struct list_head	moved;
	spinlock_t		moved_lock;

	/* All BOs of this VM not currently in the state machine */
	struct list_head	idle;

	/* BO mappings freed, but not yet updated in the PT */
	struct list_head	freed;

	/* contains the page directory */
	struct loonggpu_vm_pt     root;
	struct dma_fence	*last_update;

	/* Scheduler entity for page table updates */
	struct drm_sched_entity	entity;

	unsigned int		pasid;
	/* dedicated to vm */
	struct loonggpu_vmid	*reserved_vmid;

	/* Flag to indicate if VM tables are updated by CPU or GPU (XDMA) */
	bool                    use_cpu_for_update;

	/* Flag to indicate ATS support from PTE for GFX9 */
	bool			pte_support_ats;

	/* Up to 128 pending retry page faults */
	DECLARE_KFIFO(faults, u64, 128);

	/* Limit non-retry fault storms */
	unsigned int		fault_credit;

	/* Points to the KCD process VM info */
	struct lgkcd_process_info *process_info;

	/* List node in lgkcd_process_info.vm_list_head */
	struct list_head	vm_list_node;

	/* Valid while the PD is reserved or fenced */
	uint64_t		pd_phys_addr;

	/* Some basic info about the task */
	struct loonggpu_task_info task_info;

	/* Flag to indicate if VM is used for compute */
	bool			is_compute_context;
};

struct loonggpu_vm_manager {
	/* Handling of VMIDs */
	struct loonggpu_vmid_mgr			id_mgr;
	unsigned int				first_kcd_vmid;

	/* Handling of VM fences */
	u64					fence_context;
	unsigned				seqno[LOONGGPU_MAX_RINGS];

    uint32_t                pde_pte_bytes;

	uint64_t				max_pfn;
	uint32_t				num_level;
	uint32_t				block_size;
	uint32_t                fragment_size;
	enum loonggpu_vm_level		root_level;
	uint32_t dir0_shift, dir0_width;
	uint32_t dir1_shift, dir1_width;
	uint32_t dir2_shift, dir2_width;

	/* vram base address for page table entry  */
	u64					vram_base_offset;
	/* vm pte handling */
	const struct loonggpu_vm_pte_funcs        *vm_pte_funcs;
	struct loonggpu_ring                      *vm_pte_rings[LOONGGPU_MAX_RINGS];
	unsigned				vm_pte_num_rings;
	atomic_t				vm_pte_next_ring;

	/* partial resident texture handling */
	spinlock_t				prt_lock;
	atomic_t				num_prt_users;

	/* controls how VM page tables are updated for Graphics and Compute.
	 * BIT0[= 0] Graphics updated by XDMA [= 1] by CPU
	 * BIT1[= 0] Compute updated by XDMA [= 1] by CPU
	 */
	int					vm_update_mode;

	/* PASID to VM mapping, will be used in interrupt context to
	 * look up VM of a page fault
	 */
	struct idr				pasid_idr;
	spinlock_t				pasid_lock;
};

void loonggpu_vm_manager_init(struct loonggpu_device *adev);
void loonggpu_vm_manager_fini(struct loonggpu_device *adev);
int loonggpu_vm_set_pasid(struct loonggpu_device *adev, struct loonggpu_vm *vm,
			u32 pasid);
int loonggpu_vm_make_compute(struct loonggpu_device *adev, struct loonggpu_vm *vm);
void loonggpu_vm_release_compute(struct loonggpu_device *adev, struct loonggpu_vm *vm);
int loonggpu_vm_init(struct loonggpu_device *adev, struct loonggpu_vm *vm,
		   int vm_context, unsigned int pasid);
void loonggpu_vm_fini(struct loonggpu_device *adev, struct loonggpu_vm *vm);
int loonggpu_vm_bo_update_mapping(struct loonggpu_device *ldev,
				       struct dma_fence *exclusive,
				       dma_addr_t *pages_addr,
				       struct loonggpu_vm *vm,
				       u64 start, u64 last,
				       u64 flags, u64 addr,
				       struct dma_fence **fence);
bool loonggpu_vm_pasid_fault_credit(struct loonggpu_device *adev,
				  unsigned int pasid);
void loonggpu_vm_get_pd_bo(struct loonggpu_vm *vm,
			 struct list_head *validated,
			 struct loonggpu_bo_list_entry *entry);
bool loonggpu_vm_ready(struct loonggpu_vm *vm);
int loonggpu_vm_validate_pt_bos(struct loonggpu_device *adev, struct loonggpu_vm *vm,
			      int (*callback)(void *p, struct loonggpu_bo *bo),
			      void *param);
int loonggpu_vm_alloc_pts(struct loonggpu_device *adev,
			struct loonggpu_vm *vm,
			uint64_t saddr, uint64_t size);
int loonggpu_vm_flush(struct loonggpu_ring *ring, struct loonggpu_job *job, bool need_pipe_sync);
int loonggpu_vm_update_directories(struct loonggpu_device *adev,
				 struct loonggpu_vm *vm);
int loonggpu_vm_clear_freed(struct loonggpu_device *adev,
			  struct loonggpu_vm *vm,
			  struct dma_fence **fence);
int loonggpu_vm_handle_moved(struct loonggpu_device *adev,
			   struct loonggpu_vm *vm);
int loonggpu_vm_bo_update(struct loonggpu_device *adev,
			struct loonggpu_bo_va *bo_va,
			bool clear);
void loonggpu_vm_bo_invalidate(struct loonggpu_device *adev,
			     struct loonggpu_bo *bo, bool evicted);
struct loonggpu_bo_va *loonggpu_vm_bo_find(struct loonggpu_vm *vm,
				       struct loonggpu_bo *bo);
struct loonggpu_bo_va *loonggpu_vm_bo_add(struct loonggpu_device *adev,
				      struct loonggpu_vm *vm,
				      struct loonggpu_bo *bo);
int loonggpu_vm_bo_map(struct loonggpu_device *adev,
		     struct loonggpu_bo_va *bo_va,
		     uint64_t addr, uint64_t offset,
		     uint64_t size, uint64_t flags);
int loonggpu_vm_bo_replace_map(struct loonggpu_device *adev,
			     struct loonggpu_bo_va *bo_va,
			     uint64_t addr, uint64_t offset,
			     uint64_t size, uint64_t flags);
int loonggpu_vm_bo_unmap(struct loonggpu_device *adev,
		       struct loonggpu_bo_va *bo_va,
		       uint64_t addr);
int loonggpu_vm_bo_clear_mappings(struct loonggpu_device *adev,
				struct loonggpu_vm *vm,
				uint64_t saddr, uint64_t size);
struct loonggpu_bo_va_mapping *loonggpu_vm_bo_lookup_mapping(struct loonggpu_vm *vm,
							 uint64_t addr);
void loonggpu_vm_bo_trace_cs(struct loonggpu_vm *vm, struct ww_acquire_ctx *ticket);
void loonggpu_vm_bo_rmv(struct loonggpu_device *adev,
		      struct loonggpu_bo_va *bo_va);
void loonggpu_vm_adjust_size(struct loonggpu_device *adev, uint32_t min_vm_size,
			   unsigned max_level, unsigned max_bits);
int loonggpu_vm_ioctl(struct drm_device *dev, void *data, struct drm_file *filp);
bool loonggpu_vm_need_pipeline_sync(struct loonggpu_ring *ring,
				  struct loonggpu_job *job);

void loonggpu_vm_get_entry(struct loonggpu_pte_update_params *p, u64 addr,
			struct loonggpu_vm_pt **entry,
			struct loonggpu_vm_pt **parent);
void loonggpu_vm_get_task_info(struct loonggpu_device *adev, unsigned int pasid,
			 struct loonggpu_task_info *task_info);

void loonggpu_vm_set_task_info(struct loonggpu_vm *vm);

int loonggpu_vm_handle_fault(struct loonggpu_device *adev, u32 pasid,
			    u32 vmid, u32 node_id, uint64_t addr,
			    u32 fault_domain);

#endif /* __LOONGGPU_VM_H__ */
