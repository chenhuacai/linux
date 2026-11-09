// SPDX-License-Identifier: GPL-2.0

/* loonggpu_lgkcd.h defines the private interface between loonggpu and lgkcd. */

#ifndef LOONGGPU_LGKCD_H_INCLUDED
#define LOONGGPU_LGKCD_H_INCLUDED

#include <linux/list.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>
#include <linux/mmu_notifier.h>
#include <linux/memremap.h>
#include <drm/ttm/ttm_execbuf_util.h>
#include <kgd_kcd_interface.h>
#include "loonggpu_sync.h"
#include "loonggpu_vm.h"

#ifdef CONFIG_PCI_P2PDMA
#define CONFIG_VDD_LOONGSON_P2P
#include <linux/pci-p2pdma.h>
#endif

#define MAX_XCP 1

extern uint64_t loonggpu_lgkcd_total_mem_size;

enum TLB_FLUSH_TYPE {
	TLB_FLUSH_LEGACY = 0,
	TLB_FLUSH_LIGHTWEIGHT,
	TLB_FLUSH_HEAVYWEIGHT
};

struct loonggpu_device;

enum kcd_mem_attachment_type {
	KCD_MEM_ATT_SHARED,	/* Share kgd_mem->bo or another attachment's */
	KCD_MEM_ATT_USERPTR,	/* SG bo to DMA map pages from a userptr bo */
	KCD_MEM_ATT_DMABUF,	/* DMAbuf to DMA map TTM BOs */
	KCD_MEM_ATT_SG		/* Tag to DMA map SG BOs */
};

struct kcd_mem_attachment {
	struct list_head list;
	enum kcd_mem_attachment_type type;
	bool is_mapped;
	struct loonggpu_bo_va *bo_va;
	struct loonggpu_device *adev;
	uint64_t va;
	uint64_t pte_flags;
};

struct kgd_mem {
	struct mutex lock;
	struct loonggpu_bo *bo;
	struct kcd_ipc_obj *ipc_obj;
	struct dma_buf *dmabuf;
	struct hmm_range *range;
	struct list_head attachments;
	/* protected by lgkcd_process_info.lock */
	struct ttm_validate_buffer validate_list;
	struct ttm_validate_buffer resv_list;
	uint32_t domain;
	unsigned int mapped_to_gpu_memory;
	uint64_t va;

	uint32_t alloc_flags;

	uint32_t invalid;
	struct lgkcd_process_info *process_info;
	struct page **user_pages;

	struct loonggpu_sync sync;

	bool is_imported;
};

/* KCD Memory Eviction */
struct loonggpu_lgkcd_fence {
	struct dma_fence base;
	struct mm_struct *mm;
	spinlock_t lock;
	char timeline_name[TASK_COMM_LEN];
	struct svm_range_bo *svm_bo;
};

struct loonggpu_kcd_dev {
	struct kcd_dev *dev;
	int64_t vram_used[MAX_XCP];
	uint64_t vram_used_aligned[MAX_XCP];
	bool init_complete;
	struct work_struct reset_work;

	/* HMM page migration MEMORY_DEVICE_PRIVATE mapping */
	struct dev_pagemap pgmap;
};

enum kgd_engine_type {
	KGD_ENGINE_MEC1 = 1,
	KGD_ENGINE_SDMA1,
	KGD_ENGINE_MAX
};


struct lgkcd_process_info {
	/* List head of all VMs that belong to a KCD process */
	struct list_head vm_list_head;
	/* List head for all KCD BOs that belong to a KCD process. */
	struct list_head kcd_bo_list;
	/* List of userptr BOs that are valid or invalid */
	struct list_head userptr_valid_list;
	struct list_head userptr_inval_list;
	/* Lock to protect kcd_bo_list */
	struct mutex lock;

	/* Number of VMs */
	unsigned int n_vms;
	/* Eviction Fence */
	struct loonggpu_lgkcd_fence *eviction_fence;

	/* MMU-notifier related fields */
	struct mutex notifier_lock;
	uint32_t evicted_bos;
	struct delayed_work restore_userptr_work;
	struct pid *pid;
};

int loonggpu_lgkcd_init(void);
void loonggpu_lgkcd_fini(void);

void loonggpu_lgkcd_suspend(struct loonggpu_device *adev, bool run_pm);
int loonggpu_lgkcd_resume(struct loonggpu_device *adev, bool run_pm);
void loonggpu_lgkcd_interrupt(struct loonggpu_device *adev,
			const void *ih_ring_entry);
void loonggpu_lgkcd_device_probe(struct loonggpu_device *adev);
void loonggpu_lgkcd_device_init(struct loonggpu_device *adev);
void loonggpu_lgkcd_device_fini_sw(struct loonggpu_device *adev);
void loonggpu_lgkcd_set_compute_idle(struct loonggpu_device *adev, bool idle);
bool loonggpu_lgkcd_have_atomics_support(struct loonggpu_device *adev);
int loonggpu_lgkcd_flush_gpu_tlb_pasid(struct loonggpu_device *adev,
				uint16_t pasid, enum TLB_FLUSH_TYPE flush_type);

bool loonggpu_lgkcd_is_kcd_vmid(struct loonggpu_device *adev, u32 vmid);

int loonggpu_lgkcd_pre_reset(struct loonggpu_device *adev);

int loonggpu_lgkcd_post_reset(struct loonggpu_device *adev);

void loonggpu_lgkcd_gpu_reset(struct loonggpu_device *adev);

int loonggpu_queue_mask_bit_to_set_resource_bit(struct loonggpu_device *adev,
					int queue_bit);

struct loonggpu_lgkcd_fence *loonggpu_lgkcd_fence_create(u64 context,
				struct mm_struct *mm,
				struct svm_range_bo *svm_bo);
#if defined(CONFIG_DEBUG_FS)
int kcd_debugfs_kcd_mem_limits(struct seq_file *m, void *data);
#endif
#if IS_ENABLED(CONFIG_VDD_LOONGSON)
bool lgkcd_fence_check_mm(struct dma_fence *f, struct mm_struct *mm);
struct loonggpu_lgkcd_fence *to_loonggpu_lgkcd_fence(struct dma_fence *f);
int loonggpu_lgkcd_remove_fence_on_pt_pd_bos(struct loonggpu_bo *bo);
int loonggpu_lgkcd_evict_userptr(struct kgd_mem *mem, struct mm_struct *mm);
#else
static inline
bool lgkcd_fence_check_mm(struct dma_fence *f, struct mm_struct *mm)
{
	return false;
}

static inline
struct loonggpu_lgkcd_fence *to_loonggpu_lgkcd_fence(struct dma_fence *f)
{
	return NULL;
}

static inline
int loonggpu_lgkcd_remove_fence_on_pt_pd_bos(struct loonggpu_bo *bo)
{
	return 0;
}

static inline
int loonggpu_lgkcd_evict_userptr(struct kgd_mem *mem,
				struct mm_struct *mm)
{
	return 0;
}
#endif
/* Shared API */
int loonggpu_lgkcd_alloc_gtt_mem(struct loonggpu_device *adev, size_t size,
				void **mem_obj, uint64_t *gpu_addr,
				void **cpu_ptr, bool mqd_gfx9);
void loonggpu_lgkcd_free_gtt_mem(struct loonggpu_device *adev, void *mem_obj);
uint32_t loonggpu_lgkcd_get_fw_version(struct loonggpu_device *adev,
				      enum kgd_engine_type type);
void loonggpu_lgkcd_get_local_mem_info(struct loonggpu_device *adev,
				      struct kcd_local_mem_info *mem_info);
uint64_t loonggpu_lgkcd_get_gpu_clock_counter(struct loonggpu_device *adev);

uint32_t loonggpu_lgkcd_get_max_engine_clock_in_mhz(struct loonggpu_device *adev);
void loonggpu_lgkcd_get_cu_info(struct loonggpu_device *adev,
			       struct kcd_cu_info *cu_info);
int loonggpu_lgkcd_get_dmabuf_info(struct loonggpu_device *adev, int dma_buf_fd,
				  struct loonggpu_device **dmabuf_adev,
				  uint64_t *bo_size, void *metadata_buffer,
				  size_t buffer_size, uint32_t *metadata_size,
				  uint32_t *flags);
int loonggpu_lgkcd_get_pcie_bandwidth_mbytes(struct loonggpu_device *adev, bool is_min);
int loonggpu_lgkcd_send_close_event_drain_irq(struct loonggpu_device *adev,
					uint32_t *payload);

/* Read user wptr from a specified user address space with page fault
 * disabled. The memory must be pinned and mapped to the hardware when
 * this is called in hqd_load functions, so it should never fault in
 * the first place. This resolves a circular lock dependency involving
 * four locks, including the DQM lock and mmap_lock.
 */
#define read_user_wptr(mmptr, wptr, dst)				\
	({								\
		bool valid = false;					\
		if ((mmptr) && (wptr)) {				\
			pagefault_disable();				\
			if ((mmptr) == current->mm) {			\
				valid = !get_user((dst), (wptr));	\
			} else if (current->flags & PF_KTHREAD) {	\
				kthread_use_mm(mmptr);			\
				valid = !get_user((dst), (wptr));	\
				kthread_unuse_mm(mmptr);		\
			}						\
			pagefault_enable();				\
		}							\
		valid;							\
	})

/* GPUVM API */
#define drm_priv_to_vm(drm_priv)					\
	(&((struct loonggpu_fpriv *)					\
		((struct drm_file *)(drm_priv))->driver_priv)->vm)

int loonggpu_lgkcd_gpuvm_set_vm_pasid(struct loonggpu_device *adev,
				     struct loonggpu_vm *avm, u32 pasid);
int loonggpu_lgkcd_gpuvm_acquire_process_vm(struct loonggpu_device *adev,
					struct loonggpu_vm *avm,
					void **process_info,
					struct dma_fence **ef);
void loonggpu_lgkcd_gpuvm_release_process_vm(struct loonggpu_device *adev,
					void *drm_priv);
uint64_t loonggpu_lgkcd_gpuvm_get_process_page_dir(void *drm_priv);
size_t loonggpu_lgkcd_get_available_memory(struct loonggpu_device *adev);
int loonggpu_lgkcd_gpuvm_alloc_memory_of_gpu(
		struct loonggpu_device *adev, uint64_t va, uint64_t size,
		void *drm_priv, struct kgd_mem **mem,
		uint64_t *offset, uint32_t flags);
int loonggpu_lgkcd_gpuvm_free_memory_of_gpu(
		struct loonggpu_device *adev, struct kgd_mem *mem, void *drm_priv,
		uint64_t *size);
int loonggpu_lgkcd_gpuvm_map_memory_to_gpu(struct loonggpu_device *adev,
					  struct kgd_mem *mem, void *drm_priv);
int loonggpu_lgkcd_gpuvm_unmap_memory_from_gpu(
		struct loonggpu_device *adev, struct kgd_mem *mem, void *drm_priv);
int loonggpu_lgkcd_gpuvm_sync_memory(
		struct loonggpu_device *adev, struct kgd_mem *mem, bool intr);
int loonggpu_lgkcd_gpuvm_map_gtt_bo_to_kernel(struct kgd_mem *mem,
					     void **kptr, uint64_t *size);
void loonggpu_lgkcd_gpuvm_unmap_gtt_bo_from_kernel(struct kgd_mem *mem);

int loonggpu_lgkcd_map_gtt_bo_to_gart(struct loonggpu_device *adev, struct loonggpu_bo *bo);

int loonggpu_lgkcd_gpuvm_restore_process_bos(void *process_info,
					    struct dma_fence **ef);
int loonggpu_lgkcd_gpuvm_get_vm_fault_info(struct loonggpu_device *adev,
					      struct kcd_vm_fault_info *info);
int loonggpu_lgkcd_gpuvm_import_dmabuf(struct loonggpu_device *adev,
				      struct dma_buf *dmabuf,
				      struct kcd_ipc_obj *ipc_obj,
				      uint64_t va, void *drm_priv,
				      struct kgd_mem **mem, uint64_t *size,
				      uint64_t *mmap_offset);
int loonggpu_lgkcd_gpuvm_export_dmabuf(struct kgd_mem *mem,
				      struct dma_buf **dmabuf);
int loonggpu_lgkcd_gpuvm_export_ipc_obj(struct loonggpu_device *adev, void *vm,
				       struct kgd_mem *mem,
				       struct kcd_ipc_obj **ipc_obj,
				       uint32_t flags,
				       uint32_t *restore_handle);
int loonggpu_lgkcd_gpuvm_fault(struct loonggpu_device *adev, u32 pasid,
			    u32 vmid, u32 node_id, uint64_t addr,
			    u32 fault_domain);
void loonggpu_lgkcd_debug_mem_fence(struct loonggpu_device *adev);
int loonggpu_lgkcd_reserve_mem_limit(struct loonggpu_device *adev,
		uint64_t size, u32 alloc_flag);
void loonggpu_lgkcd_unreserve_mem_limit(struct loonggpu_device *adev,
		uint64_t size, u32 alloc_flag);

u64 loonggpu_lgkcd_real_memory_size(struct loonggpu_device *adev);

#if IS_ENABLED(CONFIG_VDD_LOONGSON)
void loonggpu_lgkcd_gpuvm_init_mem_limits(void);
void loonggpu_lgkcd_gpuvm_destroy_cb(struct loonggpu_device *adev,
				struct loonggpu_vm *vm);

/**
 * @loonggpu_lgkcd_release_notify() - Notify KCD when GEM object is released
 *
 * Allows KCD to release its resources associated with the GEM object.
 */
void loonggpu_lgkcd_release_notify(struct loonggpu_bo *bo);
void loonggpu_lgkcd_reserve_system_mem(uint64_t size);
#else
static inline
void loonggpu_lgkcd_gpuvm_init_mem_limits(void)
{
}

static inline
void loonggpu_lgkcd_gpuvm_destroy_cb(struct loonggpu_device *adev,
					struct loonggpu_vm *vm)
{
}

static inline
void loonggpu_lgkcd_release_notify(struct loonggpu_bo *bo)
{
}
#endif

#if IS_ENABLED(CONFIG_VDD_LOONGSON_SVM)
int kgd2kcd_init_zone_device(struct loonggpu_device *adev);
#else
static inline
int kgd2kcd_init_zone_device(struct loonggpu_device *adev)
{
	return 0;
}
#endif

/* KGD2KCD callbacks */
int kgd2kcd_quiesce_mm(struct mm_struct *mm, uint32_t trigger);
int kgd2kcd_resume_mm(struct mm_struct *mm);
int kgd2kcd_schedule_evict_and_restore_process(struct mm_struct *mm,
						struct dma_fence *fence);
#if IS_ENABLED(CONFIG_VDD_LOONGSON)
int kgd2kcd_init(void);
void kgd2kcd_exit(void);
struct kcd_dev *kgd2kcd_probe(struct loonggpu_device *adev);
bool kgd2kcd_device_init(struct kcd_dev *kcd,
			 const struct kgd2kcd_shared_resources *gpu_resources);
void kgd2kcd_device_exit(struct kcd_dev *kcd);
void kgd2kcd_suspend(struct kcd_dev *kcd, bool run_pm);
int kgd2kcd_resume(struct kcd_dev *kcd, bool run_pm);
int kgd2kcd_pre_reset(struct kcd_dev *kcd);
int kgd2kcd_post_reset(struct kcd_dev *kcd);
void kgd2kcd_interrupt(struct kcd_dev *kcd, const void *ih_ring_entry);
void kgd2kcd_smi_event_throttle(struct kcd_dev *kcd, uint64_t throttle_bitmask);
int kgd2kcd_check_and_lock_kcd(void);
void kgd2kcd_unlock_kcd(void);
#else
static inline int kgd2kcd_init(void)
{
	return -ENOENT;
}

static inline void kgd2kcd_exit(void)
{
}

static inline
struct kcd_dev *kgd2kcd_probe(struct loonggpu_device *adev)
{
	return NULL;
}

static inline
bool kgd2kcd_device_init(struct kcd_dev *kcd,
				const struct kgd2kcd_shared_resources *gpu_resources)
{
	return false;
}

static inline void kgd2kcd_device_exit(struct kcd_dev *kcd)
{
}

static inline void kgd2kcd_suspend(struct kcd_dev *kcd, bool run_pm)
{
}

static inline int kgd2kcd_resume(struct kcd_dev *kcd, bool run_pm)
{
	return 0;
}

static inline int kgd2kcd_pre_reset(struct kcd_dev *kcd)
{
	return 0;
}

static inline int kgd2kcd_post_reset(struct kcd_dev *kcd)
{
	return 0;
}

static inline
void kgd2kcd_interrupt(struct kcd_dev *kcd, const void *ih_ring_entry)
{
}

static inline
void kgd2kcd_smi_event_throttle(struct kcd_dev *kcd, uint64_t throttle_bitmask)
{
}

static inline int kgd2kcd_check_and_lock_kcd(void)
{
	return 0;
}

static inline void kgd2kcd_unlock_kcd(void)
{
}
#endif
#endif /* LOONGGPU_LGKCD_H_INCLUDED */
