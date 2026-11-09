/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Copyright 2020-2021 Advanced Micro Devices, Inc.
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
 *
 */

#ifndef KCD_SVM_H_
#define KCD_SVM_H_

#if IS_ENABLED(CONFIG_VDD_LOONGSON_SVM)

#include <linux/rwsem.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/sched/mm.h>
#include <linux/hmm.h>
#include <linux/interval_tree.h>
#include "loonggpu.h"
#include "../lgkcd/kcd_priv.h"

#define SVM_RANGE_VRAM_DOMAIN (1UL << 0)
#define SVM_ADEV_PGMAP_OWNER(adev)\
			((void *)(adev))

struct svm_range_bo {
	struct loonggpu_bo		*bo;
	struct kref			kref;
	struct list_head		range_list; /* all svm ranges shared this bo */
	spinlock_t			list_lock;
	struct loonggpu_lgkcd_fence	*eviction_fence;
	struct work_struct		eviction_work;
	uint32_t			evicting;
	struct work_struct		release_work;
	struct kcd_node			*node;
};

enum svm_work_list_ops {
	SVM_OP_NULL,
	SVM_OP_UNMAP_RANGE,
	SVM_OP_UPDATE_RANGE_NOTIFIER,
	SVM_OP_UPDATE_RANGE_NOTIFIER_AND_MAP,
	SVM_OP_ADD_RANGE,
	SVM_OP_ADD_RANGE_AND_MAP
};

struct svm_work_list_item {
	enum svm_work_list_ops op;
	struct mm_struct *mm;
};

/**
 * struct svm_range - shared virtual memory range
 *
 * @svms:       list of svm ranges, structure defined in kcd_process
 * @migrate_mutex: to serialize range migration, validation and mapping update
 * @start:      range start address in pages
 * @last:       range last address in pages
 * @it_node:    node [start, last] stored in interval tree, start, last are page
 *              aligned, page size is (last - start + 1)
 * @list:       link list node, used to scan all ranges of svms
 * @update_list:link list node used to add to update_list
 * @mapping:    bo_va mapping structure to create and update GPU page table
 * @npages:     number of pages
 * @dma_addr:   dma mapping address on each GPU for system memory physical page
 * @ttm_res:    vram ttm resource map
 * @offset:     range start offset within mm_nodes
 * @svm_bo:     struct to manage splited loonggpu_bo
 * @svm_bo_list:link list node, to scan all ranges which share same svm_bo
 * @lock:       protect prange start, last, child_list, svm_bo_list
 * @saved_flags:save/restore current PF_MEMALLOC flags
 * @flags:      flags defined as KCD_IOCTL_SVM_FLAG_*
 * @perferred_loc: perferred location, 0 for CPU, or GPU id
 * @perfetch_loc: last prefetch location, 0 for CPU, or GPU id
 * @actual_loc: the actual location, 0 for CPU, or GPU id
 * @granularity:migration granularity, log2 num pages
 * @invalid:    not 0 means cpu page table is invalidated
 * @validate_timestamp: system timestamp when range is validated
 * @notifier:   register mmu interval notifier
 * @work_item:  deferred work item information
 * @deferred_list: list header used to add range to deferred list
 * @child_list: list header for split ranges which are not added to svms yet
 * @bitmap_access: index bitmap of GPUs which can access the range
 * @bitmap_aip: index bitmap of GPUs which can access the range in place
 *
 * Data structure for virtual memory range shared by CPU and GPUs, it can be
 * allocated from system memory ram or device vram, and migrate from ram to vram
 * or from vram to ram.
 */
struct svm_range {
	struct svm_range_list		*svms;
	struct mutex			migrate_mutex;
	unsigned long			start;
	unsigned long			last;
	struct interval_tree_node	it_node;
	struct list_head		list;
	struct list_head		update_list;
	uint64_t			npages;
	dma_addr_t			*dma_addr[MAX_GPU_INSTANCE];
	struct ttm_resource		*ttm_res;
	uint64_t			offset;
	struct svm_range_bo		*svm_bo;
	struct list_head		svm_bo_list;
	struct mutex                    lock;
	unsigned int                    saved_flags;
	uint32_t			flags;
	uint32_t			preferred_loc;
	uint32_t			prefetch_loc;
	uint32_t			actual_loc;
	uint8_t				granularity;
	atomic_t			invalid;
	ktime_t				validate_timestamp;
	struct mmu_interval_notifier	notifier;
	struct svm_work_list_item	work_item;
	struct list_head		deferred_list;
	struct list_head		child_list;
	DECLARE_BITMAP(bitmap_access, MAX_GPU_INSTANCE);
	DECLARE_BITMAP(bitmap_aip, MAX_GPU_INSTANCE);
	bool				validated_once;
	bool				mapped_to_gpu;
	bool				is_error_flag;
};

static inline void svm_range_lock(struct svm_range *prange)
{
	mutex_lock(&prange->lock);
	prange->saved_flags = memalloc_noreclaim_save();

}
static inline void svm_range_unlock(struct svm_range *prange)
{
	memalloc_noreclaim_restore(prange->saved_flags);
	mutex_unlock(&prange->lock);
}

static inline struct svm_range_bo *svm_range_bo_ref(struct svm_range_bo *svm_bo)
{
	if (svm_bo)
		kref_get(&svm_bo->kref);

	return svm_bo;
}

int svm_range_list_init(struct kcd_process *p);
void svm_range_list_fini(struct kcd_process *p);
int svm_ioctl(struct kcd_process *p, enum kcd_ioctl_svm_op op, uint64_t start,
	      uint64_t size, uint32_t nattrs,
	      struct kcd_ioctl_svm_attribute *attrs);
struct svm_range *svm_range_from_addr(struct svm_range_list *svms,
				      unsigned long addr,
				      struct svm_range **parent);
struct kcd_node *svm_range_get_node_by_id(struct svm_range *prange,
					  uint32_t gpu_id);
int svm_range_vram_node_new(struct kcd_node *node, struct svm_range *prange,
			    bool clear);
void svm_range_vram_node_free(struct svm_range *prange);
int svm_range_split_by_granularity(struct kcd_process *p, struct mm_struct *mm,
			       unsigned long addr, struct svm_range *parent,
			       struct svm_range *prange);
int svm_range_restore_pages(struct loonggpu_device *adev, unsigned int pasid,
			    uint32_t vmid, uint32_t node_id, uint64_t addr,
			    bool write_fault);
int svm_range_schedule_evict_svm_bo(struct loonggpu_lgkcd_fence *fence);
void svm_range_add_list_work(struct svm_range_list *svms,
			     struct svm_range *prange, struct mm_struct *mm,
			     enum svm_work_list_ops op);
void schedule_deferred_list_work(struct svm_range_list *svms);
void svm_range_dma_unmap(struct device *dev, dma_addr_t *dma_addr,
			 unsigned long offset, unsigned long npages);
void svm_range_free_dma_mappings(struct svm_range *prange, bool unmap_dma);
int svm_range_get_info(struct kcd_process *p, uint32_t *num_svm_ranges,
		       uint64_t *svm_priv_data_size);
struct kcd_process_device *
svm_range_get_pdd_by_node(struct svm_range *prange, struct kcd_node *node);
void svm_range_list_lock_and_flush_work(struct svm_range_list *svms, struct mm_struct *mm);

/* SVM API and HMM page migration work together, device memory type
 * is initialized to not 0 when page migration register device memory.
 */
#define KCD_IS_SVM_API_SUPPORTED(adev) ((adev)->kcd.pgmap.type != 0 ||\
					(adev)->gmc.is_app_apu)

void svm_range_bo_unref_async(struct svm_range_bo *svm_bo);

void svm_range_set_max_pages(struct loonggpu_device *adev);
int svm_range_switch_xnack_reserve_mem(struct kcd_process *p, bool xnack_enabled);

#else

struct kcd_process;

static inline int svm_range_list_init(struct kcd_process *p)
{
	return 0;
}
static inline void svm_range_list_fini(struct kcd_process *p)
{
	/* empty */
}

static inline int svm_range_restore_pages(struct loonggpu_device *adev,
					  unsigned int pasid,
					  uint32_t client_id, uint32_t node_id,
					  uint64_t addr, bool write_fault)
{
	return -EFAULT;
}

static inline int svm_range_schedule_evict_svm_bo(
		struct loonggpu_lgkcd_fence *fence)
{
	WARN_ONCE(1, "SVM eviction fence triggered, but SVM is disabled");
	return -EINVAL;
}

static inline int svm_range_get_info(struct kcd_process *p,
				     uint32_t *num_svm_ranges,
				     uint64_t *svm_priv_data_size)
{
	*num_svm_ranges = 0;
	*svm_priv_data_size = 0;
	return 0;
}

static inline void svm_range_set_max_pages(struct loonggpu_device *adev)
{
}

#define KCD_IS_SVM_API_SUPPORTED(dev) false

#endif /* IS_ENABLED(CONFIG_VDD_LOONGSON_SVM) */

#endif /* KCD_SVM_H_ */
