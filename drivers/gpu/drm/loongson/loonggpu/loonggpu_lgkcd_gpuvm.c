// SPDX-License-Identifier: MIT
/*
 * Copyright 2014-2018 Advanced Micro Devices, Inc.
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
#include <linux/dma-buf.h>
#include <linux/list.h>
#include <linux/dma-resv.h>
#include <linux/pagemap.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <drm/ttm/ttm_tt.h>

#include "loonggpu_object.h"
#include "loonggpu_vm.h"
#include "loonggpu_lgkcd.h"
#include "loonggpu_shared.h"
#include "kcd_ioctl.h"
#include "../lgkcd/kcd_ipc.h"
#include "../lgkcd/kcd_smi_events.h"
#include "loonggpu_helper.h"

/* Userptr restore delay, just long enough to allow consecutive VM
 * changes to accumulate
 */
#define LOONGGPU_USERPTR_RESTORE_DELAY_MS 1
/* BO flag to indicate a KCD userptr BO */
#define LOONGGPU_LGKCD_CREATE_USERPTR_BO	(1ULL << 63)
/*
 * Align VRAM availability to 2MB to avoid fragmentation caused by 4K allocations in the tail 2MB
 * BO chunk
 */
#define VRAM_AVAILABLITY_ALIGN (1 << 21)

/* Impose limit on how much memory KCD can use */
static struct {
	uint64_t max_system_mem_limit;
	uint64_t max_ttm_mem_limit;
	int64_t system_mem_used;
	int64_t ttm_mem_used;
	spinlock_t mem_limit_lock;
} kcd_mem_limit;

static const char * const domain_bit_to_string[] = {
		"CPU",
		"GTT",
		"VRAM",
};

#define domain_string(domain) domain_bit_to_string[ffs(domain)-1]

static void loonggpu_lgkcd_restore_userptr_worker(struct work_struct *work);

static bool kcd_mem_is_attached(struct loonggpu_vm *avm,
		struct kgd_mem *mem)
{
	struct kcd_mem_attachment *entry;

	list_for_each_entry(entry, &mem->attachments, list)
		if (entry->bo_va->base.vm == avm)
			return true;

	return false;
}

/**
 * reuse_dmamap() - Check whether adev can share the original
 * userptr BO
 *
 * If both adev and bo_adev are in direct mapping or
 * in the same iommu group, they can share the original BO.
 *
 * @adev: Device to which can or cannot share the original BO
 * @bo_adev: Device to which allocated BO belongs to
 *
 * Return: returns true if adev can share original userptr BO,
 * false otherwise.
 */
static bool reuse_dmamap(struct loonggpu_device *adev, struct loonggpu_device *bo_adev)
{
	return false;
	// return (adev->ram_is_direct_mapped && bo_adev->ram_is_direct_mapped) ||
	// 		(adev->dev->iommu_group == bo_adev->dev->iommu_group);
}

/* Set memory usage limits. Current, limits are
 *  System (TTM + userptr) memory - 15/16th System RAM
 *  TTM memory - 3/8th System RAM
 */
void loonggpu_lgkcd_gpuvm_init_mem_limits(void)
{
	struct sysinfo si;
	uint64_t mem;

	if (kcd_mem_limit.max_system_mem_limit)
		return;

	si_meminfo(&si);
	mem = si.freeram - si.freehigh;
	mem *= si.mem_unit;

	spin_lock_init(&kcd_mem_limit.mem_limit_lock);
	kcd_mem_limit.max_system_mem_limit = mem - (mem >> 4);
	kcd_mem_limit.max_ttm_mem_limit = min(LOONGGPU_DEFAULT_GTT_SIZE_MB << 20,
			       (uint64_t)si.totalram * si.mem_unit * 3/4);
	pr_debug("Kernel memory limit %lluM, TTM limit %lluM\n",
		(kcd_mem_limit.max_system_mem_limit >> 20),
		(kcd_mem_limit.max_ttm_mem_limit >> 20));
}

void loonggpu_lgkcd_reserve_system_mem(uint64_t size)
{
	kcd_mem_limit.system_mem_used += size;
}

/* Estimate page table size needed to represent a given memory size
 *
 * With 4KB pages, we need one 8 byte PTE for each 4KB of memory
 * (factor 512, >> 9). With 2MB pages, we need one 8 byte PTE for 2MB
 * of memory (factor 256K, >> 18). ROCm user mode tries to optimize
 * for 2MB pages for TLB efficiency. However, small allocations and
 * fragmented system memory still need some 4KB pages. We choose a
 * compromise that should work in most cases without reserving too
 * much memory for page tables unnecessarily (factor 16K, >> 14).
 */

#define ESTIMATE_PT_SIZE(mem_size) max(((mem_size) >> 14), LOONGGPU_VM_RESERVED_VRAM)

/**
 * loonggpu_lgkcd_reserve_mem_limit() - Decrease available memory by size
 * of buffer.
 *
 * @adev: Device to which allocated BO belongs to
 * @size: Size of buffer, in bytes, encapsulated by B0. This should be
 * equivalent to loonggpu_bo_size(BO)
 * @alloc_flag: Flag used in allocating a BO as noted above
 *
 * Return:
 *	returns -ENOMEM in case of error, ZERO otherwise
 */
int loonggpu_lgkcd_reserve_mem_limit(struct loonggpu_device *adev,
		uint64_t size, u32 alloc_flag)
{
	uint64_t reserved_for_pt =
		ESTIMATE_PT_SIZE(loonggpu_lgkcd_total_mem_size);
	size_t system_mem_needed, ttm_mem_needed, vram_needed;
	int ret = 0;
	uint64_t vram_size = 0;

	system_mem_needed = 0;
	ttm_mem_needed = 0;
	vram_needed = 0;
	if (alloc_flag & KCD_IOC_ALLOC_MEM_FLAGS_GTT) {
		system_mem_needed = size;
		ttm_mem_needed = size;
	} else if (alloc_flag & KCD_IOC_ALLOC_MEM_FLAGS_VRAM) {
		/*
		 * Conservatively round up the allocation requirement to 2 MB
		 * to avoid fragmentation caused by 4K allocations in the tail
		 * 2M BO chunk.
		 */
		vram_needed = size;

		vram_size = loonggpu_lgkcd_real_memory_size(adev);
		if (adev->gmc.is_app_apu) {
			system_mem_needed = size;
			ttm_mem_needed = size;
		}
	} else if (alloc_flag & KCD_IOC_ALLOC_MEM_FLAGS_USERPTR) {
		system_mem_needed = size;
	} else if (!(alloc_flag & KCD_IOC_ALLOC_MEM_FLAGS_DOORBELL)) {
		pr_err("%s: Invalid BO type %#x\n", __func__, alloc_flag);
		return -ENOMEM;
	}

	spin_lock(&kcd_mem_limit.mem_limit_lock);

	if (kcd_mem_limit.system_mem_used + system_mem_needed >
	    kcd_mem_limit.max_system_mem_limit)
		pr_debug("Set no_system_mem_limit=1 if using shared memory\n");

	if ((kcd_mem_limit.system_mem_used + system_mem_needed >
	     kcd_mem_limit.max_system_mem_limit && !no_system_mem_limit) ||
	    (kcd_mem_limit.ttm_mem_used + ttm_mem_needed >
	     kcd_mem_limit.max_ttm_mem_limit) ||
	    (adev && adev->kcd.vram_used[0] + vram_needed >
	     vram_size - reserved_for_pt - atomic64_read(&adev->vram_pin_size))) {
		ret = -ENOMEM;
		goto release;
	}

	/* Update memory accounting by decreasing available system
	 * memory, TTM memory and GPU memory as computed above
	 */
	WARN_ONCE(vram_needed && !adev,
		  "adev reference can't be null when vram is used");
	if (adev) {
		adev->kcd.vram_used[0] += vram_needed;
		adev->kcd.vram_used_aligned[0] += adev->gmc.is_app_apu ?
				vram_needed :
				ALIGN(vram_needed, VRAM_AVAILABLITY_ALIGN);
	}
	kcd_mem_limit.system_mem_used += system_mem_needed;
	kcd_mem_limit.ttm_mem_used += ttm_mem_needed;

release:
	spin_unlock(&kcd_mem_limit.mem_limit_lock);
	return ret;
}

void loonggpu_lgkcd_unreserve_mem_limit(struct loonggpu_device *adev,
		uint64_t size, u32 alloc_flag)
{
	spin_lock(&kcd_mem_limit.mem_limit_lock);

	if (alloc_flag & KCD_IOC_ALLOC_MEM_FLAGS_GTT) {
		kcd_mem_limit.system_mem_used -= size;
		kcd_mem_limit.ttm_mem_used -= size;
	} else if (alloc_flag & KCD_IOC_ALLOC_MEM_FLAGS_VRAM) {
		WARN_ONCE(!adev,
			  "adev reference can't be null when alloc mem flags vram is set");

		if (adev) {
			adev->kcd.vram_used[0] -= size;
			if (adev->gmc.is_app_apu) {
				adev->kcd.vram_used_aligned[0] -= size;
				kcd_mem_limit.system_mem_used -= size;
				kcd_mem_limit.ttm_mem_used -= size;
			} else {
				adev->kcd.vram_used_aligned[0] -=
					ALIGN(size, VRAM_AVAILABLITY_ALIGN);
			}
		}
	} else if (alloc_flag & KCD_IOC_ALLOC_MEM_FLAGS_USERPTR) {
		kcd_mem_limit.system_mem_used -= size;
	} else if (!(alloc_flag & KCD_IOC_ALLOC_MEM_FLAGS_DOORBELL)) {
		pr_err("%s: Invalid BO type %#x\n", __func__, alloc_flag);
		goto release;
	}

	WARN_ONCE(adev && adev->kcd.vram_used[0] < 0,
		  "KCD VRAM memory accounting unbalanced");
	WARN_ONCE(kcd_mem_limit.ttm_mem_used < 0,
		  "KCD TTM memory accounting unbalanced");
	WARN_ONCE(kcd_mem_limit.system_mem_used < 0,
		  "KCD system memory accounting unbalanced");

release:
	spin_unlock(&kcd_mem_limit.mem_limit_lock);
}

void loonggpu_lgkcd_release_notify(struct loonggpu_bo *bo)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);
	u32 alloc_flags = bo->kcd_bo->alloc_flags;
	u64 size = loonggpu_bo_size(bo);

	loonggpu_lgkcd_unreserve_mem_limit(adev, size, alloc_flags);
	mutex_destroy(&bo->kcd_bo->lock);
	kfree(bo->kcd_bo);
}

/**
 * create_dmamap_sg_bo() - Creates a loonggpu_bo object to reflect information
 * about USERPTR or DOOREBELL or MMIO BO.
 *
 * @adev: Device for which dmamap BO is being created
 * @mem: BO of peer device that is being DMA mapped. Provides parameters
 *	 in building the dmamap BO
 * @bo_out: Output parameter updated with handle of dmamap BO
 */
static int
create_dmamap_sg_bo(struct loonggpu_device *adev,
		 struct kgd_mem *mem, struct loonggpu_bo **bo_out)
{
	struct drm_gem_object *gem_obj;
	int ret;

	ret = loonggpu_bo_reserve(mem->bo, false);
	if (ret)
		return ret;

	ret = loonggpu_gem_object_create(adev, lg_gbo_to_gem_obj(mem->bo).size, 1,
			LOONGGPU_GEM_DOMAIN_CPU, LOONGGPU_GEM_CREATE_PREEMPTIBLE,
			ttm_bo_type_sg, lg_tbo_to_resv(&mem->bo->tbo), &gem_obj);

	loonggpu_bo_unreserve(mem->bo);

	if (ret) {
		pr_err("Error in creating DMA mappable SG BO on domain: %d\n", ret);
		return -EINVAL;
	}

	*bo_out = gem_to_loonggpu_bo(gem_obj);
	(*bo_out)->parent = loonggpu_bo_ref(mem->bo);
	return ret;
}

/* loonggpu_lgkcd_remove_eviction_fence - Removes eviction fence from BO's
 *  reservation object.
 *
 * @bo: [IN] Remove eviction fence(s) from this BO
 * @ef: [IN] This eviction fence is removed if it
 *  is present in the shared list.
 *
 * NOTE: Must be called with BO reserved i.e. bo->tbo.resv->lock held.
 */
static int loonggpu_lgkcd_remove_eviction_fence(struct loonggpu_bo *bo,
					struct loonggpu_lgkcd_fence *ef,
					struct loonggpu_lgkcd_fence ***ef_list,
					unsigned int *ef_count)
{
	struct dma_fence *replacement;

	if (!ef)
		return -EINVAL;

	/* TODO: Instead of block before we should use the fence of the page
	 * table update and TLB flush here directly.
	 */
	replacement = dma_fence_get_stub();
	dma_resv_replace_fences(bo->tbo.base.resv, ef->base.context,
				replacement, DMA_RESV_USAGE_BOOKKEEP);
	dma_fence_put(replacement);
	return 0;
}

int loonggpu_lgkcd_remove_fence_on_pt_pd_bos(struct loonggpu_bo *bo)
{
	struct loonggpu_bo *root = bo;
	struct loonggpu_vm_bo_base *vm_bo;
	struct loonggpu_vm *vm;
	struct lgkcd_process_info *info;
	struct loonggpu_lgkcd_fence *ef;
	int ret;

	/* we can always get vm_bo from root PD bo.*/
	while (root->parent)
		root = root->parent;

	vm_bo = list_first_entry(&bo->va, struct loonggpu_vm_bo_base,
					  bo_list);
	if (!vm_bo)
		return 0;

	vm = vm_bo->vm;
	if (!vm)
		return 0;

	info = vm->process_info;
	if (!info || !info->eviction_fence)
		return 0;

	ef = container_of(dma_fence_get(&info->eviction_fence->base),
			struct loonggpu_lgkcd_fence, base);

	BUG_ON(loonggpu_bo_reserve(bo, false));
	ret = loonggpu_lgkcd_remove_eviction_fence(bo, ef, NULL, NULL);
	loonggpu_bo_unreserve(bo);

	dma_fence_put(&ef->base);
	return ret;
}

/* loonggpu_lgkcd_add_eviction_fence - Adds eviction fence(s) back into BO's
 *  reservation object.
 *
 * @bo: [IN] Add eviction fences to this BO
 * @ef_list: [IN] List of eviction fences to be added
 * @ef_count: [IN] Number of fences in ef_list.
 *
 * NOTE: Must call loonggpu_lgkcd_add_eviction_fence before calling this
 *  function.
 */
static void loonggpu_lgkcd_add_eviction_fence(struct loonggpu_bo *bo,
				struct loonggpu_lgkcd_fence **ef_list,
				unsigned int ef_count)
{
	int i;

	if (!ef_list || !ef_count)
		return;

	for (i = 0; i < ef_count; i++) {
		loonggpu_bo_fence(bo, &ef_list[i]->base, true);
		/* Re-adding the fence takes an additional reference. Drop that
		 * reference.
		 */
		dma_fence_put(&ef_list[i]->base);
	}

	kfree(ef_list);
}

static int loonggpu_lgkcd_bo_validate(struct loonggpu_bo *bo, uint32_t domain,
				     bool wait)
{
	struct ttm_operation_ctx ctx = { false, false };
	int ret;

	if (WARN(loonggpu_ttm_tt_get_usermm(bo->tbo.ttm),
		 "Called with userptr BO"))
		return -EINVAL;

	loonggpu_bo_placement_from_domain(bo, domain);

	ret = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
	if (ret)
		goto validate_fail;
	if (wait) {
		/* not waitting for kcd fence */
		loonggpu_bo_sync_wait(bo, LOONGGPU_SYNC_NE_OWNER, LOONGGPU_FENCE_OWNER_KCD, false, false);
	}

validate_fail:
	return ret;
}

static int loonggpu_lgkcd_validate_vm_bo(void *_unused, struct loonggpu_bo *bo)
{
	return loonggpu_lgkcd_bo_validate(bo, bo->allowed_domains, false);
}

/* vm_validate_pt_pd_bos - Validate page table and directory BOs
 *
 * Page directories are not updated here because huge page handling
 * during page table updates can invalidate page directory entries
 * again. Page directories are only updated after updating page
 * tables.
 */
static int vm_validate_pt_pd_bos(struct loonggpu_vm *vm)
{
	struct loonggpu_bo *pd = vm->root.base.bo;
	struct loonggpu_device *adev = loonggpu_ttm_adev(pd->tbo.bdev);
	int ret;

	ret = loonggpu_vm_validate_pt_bos(adev, vm, loonggpu_lgkcd_validate_vm_bo, NULL);
	if (ret) {
		pr_err("failed to validate PT BOs\n");
		return ret;
	}

	ret = loonggpu_lgkcd_validate_vm_bo(NULL, pd);
	if (ret) {
		pr_err("loonggpu: failed to validate PD\n");
		return ret;
	}

	vm->pd_phys_addr = loonggpu_bo_gpu_offset(vm->root.base.bo);

	if (vm->use_cpu_for_update) {
		ret = loonggpu_bo_kmap(pd, NULL);
		if (ret) {
			pr_err("loonggpu: failed to kmap PD, ret=%d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int sync_vm_fence(struct loonggpu_device *adev, struct loonggpu_sync *sync,
			 struct dma_fence *f)
{
	int ret = loonggpu_sync_fence(adev, sync, f, false);

	/* Sync objects can't handle multiple GPUs (contexts) updating
	 * sync->last_vm_update. Fortunately we don't need it for
	 * KCD's purposes, so we can just drop that fence.
	 */
	if (sync->last_vm_update) {
		dma_fence_put(sync->last_vm_update);
		sync->last_vm_update = NULL;
	}

	return ret;
}

static int vm_update_pds(struct loonggpu_vm *vm, struct loonggpu_sync *sync)
{
	struct loonggpu_bo *pd = vm->root.base.bo;
	struct loonggpu_device *adev = loonggpu_ttm_adev(pd->tbo.bdev);
	int ret;

	ret = loonggpu_vm_update_directories(adev, vm);
	if (ret)
		return ret;

	return sync_vm_fence(adev, sync, vm->last_update);
}

static uint64_t get_pte_flags(struct loonggpu_device *adev, struct kgd_mem *mem)
{
	uint32_t mapping_flags = LOONGGPU_VM_PAGE_READABLE |
				 LOONGGPU_VM_MTYPE_DEFAULT;

	if (mem->alloc_flags & KCD_IOC_ALLOC_MEM_FLAGS_WRITABLE)
		mapping_flags |= LOONGGPU_VM_PAGE_WRITEABLE;
	if (mem->alloc_flags & KCD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE)
		mapping_flags |= LOONGGPU_VM_PAGE_EXECUTABLE;

	return loonggpu_gmc_get_pte_flags(adev, mapping_flags);
}

/**
 * create_sg_table() - Create an sg_table for a contiguous DMA addr range
 * @addr: The starting address to point to
 * @size: Size of memory area in bytes being pointed to
 *
 * Allocates an instance of sg_table and initializes it to point to memory
 * area specified by input parameters. The address used to build is assumed
 * to be DMA mapped, if needed.
 *
 * DOORBELL BOs use only one scatterlist node in their sg_table
 * because they are physically contiguous.
 *
 * Return: Initialized instance of SG Table or NULL
 */
static struct sg_table *create_sg_table(uint64_t addr, uint32_t size)
{
	struct sg_table *sg = kmalloc(sizeof(*sg), GFP_KERNEL);

	if (!sg)
		return NULL;
	if (sg_alloc_table(sg, 1, GFP_KERNEL)) {
		kfree(sg);
		return NULL;
	}
	sg_dma_address(sg->sgl) = addr;
	sg->sgl->length = size;
#ifdef CONFIG_NEED_SG_DMA_LENGTH
	sg->sgl->dma_length = size;
#endif
	return sg;
}

static int
kcd_mem_dmamap_userptr(struct kgd_mem *mem,
		       struct kcd_mem_attachment *attachment)
{
	enum dma_data_direction direction =
		mem->alloc_flags & KCD_IOC_ALLOC_MEM_FLAGS_WRITABLE ?
		DMA_BIDIRECTIONAL : DMA_TO_DEVICE;
	struct ttm_operation_ctx ctx = {.interruptible = true};
	struct loonggpu_bo *bo = attachment->bo_va->base.bo;
	struct loonggpu_device *adev = attachment->adev;
	struct ttm_tt *src_ttm = mem->bo->tbo.ttm;
	struct ttm_tt *ttm = bo->tbo.ttm;
	int ret;

	if (WARN_ON(ttm->num_pages != src_ttm->num_pages))
		return -EINVAL;

	ttm->sg = kmalloc(sizeof(*ttm->sg), GFP_KERNEL);
	if (unlikely(!ttm->sg))
		return -ENOMEM;

	/* Same sequence as in loonggpu_ttm_tt_pin_userptr */
	ret = sg_alloc_table_from_pages(ttm->sg, src_ttm->pages,
					ttm->num_pages, 0,
					(u64)ttm->num_pages << PAGE_SHIFT,
					GFP_KERNEL);
	if (unlikely(ret))
		goto free_sg;

	ret = dma_map_sg(adev->dev, ttm->sg->sgl, ttm->sg->nents, direction);
	if (unlikely(!ret))
		goto release_sg;

	loonggpu_bo_placement_from_domain(bo, LOONGGPU_GEM_DOMAIN_GTT);
	ret = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
	if (ret)
		goto unmap_sg;

	return 0;

unmap_sg:
	dma_unmap_sg(adev->dev, ttm->sg->sgl, ttm->sg->nents, direction);
release_sg:
	pr_err("DMA map userptr failed: %d\n", ret);
	sg_free_table(ttm->sg);
free_sg:
	kfree(ttm->sg);
	ttm->sg = NULL;
	return ret;
}

static int
kcd_mem_dmamap_sg_bo(struct kgd_mem *mem,
		     struct kcd_mem_attachment *attachment)
{
	struct ttm_operation_ctx ctx = {.interruptible = true};
	struct loonggpu_bo *bo = attachment->bo_va->base.bo;
	struct loonggpu_device *adev = attachment->adev;
	struct ttm_tt *ttm = bo->tbo.ttm;
	enum dma_data_direction dir;
	dma_addr_t dma_addr;
	int ret;

	/* Expect SG Table of dmapmap BO to be NULL */
	if (unlikely(ttm->sg)) {
		pr_err("SG Table of BO for peer device is UNEXPECTEDLY NON-NULL");
		return -EINVAL;
	}

	dir = mem->alloc_flags & KCD_IOC_ALLOC_MEM_FLAGS_WRITABLE ?
			DMA_BIDIRECTIONAL : DMA_TO_DEVICE;
	dma_addr = mem->bo->tbo.sg->sgl->dma_address;
	pr_debug("BO size: %d\n", mem->bo->tbo.sg->sgl->length);
	pr_debug("BO address before DMA mapping: %llx\n", dma_addr);

	if (!(mem->alloc_flags & KCD_IOC_ALLOC_MEM_FLAGS_DOORBELL)) {
		dma_addr = dma_map_resource(adev->dev, dma_addr,
				mem->bo->tbo.sg->sgl->length, dir,
				DMA_ATTR_SKIP_CPU_SYNC);
		ret = dma_mapping_error(adev->dev, dma_addr);
		if (unlikely(ret))
			return ret;
	}
	pr_debug("BO address after DMA mapping: %llx\n", dma_addr);

	ttm->sg = create_sg_table(dma_addr, mem->bo->tbo.sg->sgl->length);
	if (unlikely(!ttm->sg)) {
		ret = -ENOMEM;
		goto unmap_sg;
	}

	loonggpu_bo_placement_from_domain(bo, LOONGGPU_GEM_DOMAIN_GTT);
	ret = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
	if (unlikely(ret))
		goto free_sg;

	return ret;

free_sg:
	sg_free_table(ttm->sg);
	kfree(ttm->sg);
	ttm->sg = NULL;
unmap_sg:
	if (!(mem->alloc_flags & KCD_IOC_ALLOC_MEM_FLAGS_DOORBELL))
		dma_unmap_resource(adev->dev, dma_addr,
				mem->bo->tbo.sg->sgl->length, dir,
				DMA_ATTR_SKIP_CPU_SYNC);
	return ret;
}

static int
kcd_mem_dmamap_dmabuf(struct kcd_mem_attachment *attachment)
{
	struct ttm_operation_ctx ctx = {.interruptible = true};
	struct loonggpu_bo *bo = attachment->bo_va->base.bo;
	int ret;

	loonggpu_bo_placement_from_domain(bo, LOONGGPU_GEM_DOMAIN_CPU);
	ret = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
	if (ret)
		return ret;

	loonggpu_bo_placement_from_domain(bo, LOONGGPU_GEM_DOMAIN_GTT);
	return ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
}

static int
kcd_mem_dmamap_attachment(struct kgd_mem *mem,
			  struct kcd_mem_attachment *attachment)
{
	switch (attachment->type) {
	case KCD_MEM_ATT_SHARED:
		return 0;
	case KCD_MEM_ATT_USERPTR:
		return kcd_mem_dmamap_userptr(mem, attachment);
	case KCD_MEM_ATT_DMABUF:
		return kcd_mem_dmamap_dmabuf(attachment);
	case KCD_MEM_ATT_SG:
		return kcd_mem_dmamap_sg_bo(mem, attachment);
	default:
		WARN_ON_ONCE(1);
	}
	return -EINVAL;
}

static void
kcd_mem_dmaunmap_userptr(struct kgd_mem *mem,
			 struct kcd_mem_attachment *attachment)
{
	enum dma_data_direction direction =
		mem->alloc_flags & KCD_IOC_ALLOC_MEM_FLAGS_WRITABLE ?
		DMA_BIDIRECTIONAL : DMA_TO_DEVICE;
	struct ttm_operation_ctx ctx = {.interruptible = false};
	struct loonggpu_bo *bo = attachment->bo_va->base.bo;
	struct loonggpu_device *adev = attachment->adev;
	struct ttm_tt *ttm = bo->tbo.ttm;

	if (unlikely(!ttm->sg))
		return;

	loonggpu_bo_placement_from_domain(bo, LOONGGPU_GEM_DOMAIN_CPU);
	ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);

	dma_unmap_sg(adev->dev, ttm->sg->sgl, ttm->sg->nents, direction);
	sg_free_table(ttm->sg);
	kfree(ttm->sg);
	ttm->sg = NULL;
}

static void
kcd_mem_dmaunmap_sg_bo(struct kgd_mem *mem,
			 struct kcd_mem_attachment *attachment)
{
	enum dma_data_direction direction =
		mem->alloc_flags & KCD_IOC_ALLOC_MEM_FLAGS_WRITABLE ?
		DMA_BIDIRECTIONAL : DMA_TO_DEVICE;
	struct ttm_operation_ctx ctx = {.interruptible = false};
	struct loonggpu_bo *bo = attachment->bo_va->base.bo;
	struct loonggpu_device *adev = attachment->adev;
	struct ttm_tt *ttm = bo->tbo.ttm;

	if (unlikely(!ttm->sg))
		return;

	loonggpu_bo_placement_from_domain(bo, LOONGGPU_GEM_DOMAIN_CPU);
	ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);

	if (!(mem->alloc_flags & KCD_IOC_ALLOC_MEM_FLAGS_DOORBELL))
		dma_unmap_resource(adev->dev, ttm->sg->sgl->dma_address,
			ttm->sg->sgl->length, direction,
			DMA_ATTR_SKIP_CPU_SYNC);

	sg_free_table(ttm->sg);
	kfree(ttm->sg);
	ttm->sg = NULL;
	bo->tbo.sg = NULL;
}

static void
kcd_mem_dmaunmap_dmabuf(struct kcd_mem_attachment *attachment)
{
	/* This is a no-op. We don't want to trigger eviction fences when
	 * unmapping DMABufs. Therefore the invalidation (moving to system
	 * domain) is done in kcd_mem_dmamap_dmabuf.
	 */
}

static void
kcd_mem_dmaunmap_attachment(struct kgd_mem *mem,
			    struct kcd_mem_attachment *attachment)
{
	switch (attachment->type) {
	case KCD_MEM_ATT_SHARED:
		break;
	case KCD_MEM_ATT_USERPTR:
		kcd_mem_dmaunmap_userptr(mem, attachment);
		break;
	case KCD_MEM_ATT_DMABUF:
		kcd_mem_dmaunmap_dmabuf(attachment);
		break;
        case KCD_MEM_ATT_SG:
		kcd_mem_dmaunmap_sg_bo(mem, attachment);
		break;
	default:
		WARN_ON_ONCE(1);
	}
}

static int kcd_mem_export_dmabuf(struct kgd_mem *mem)
{
	if (!mem->dmabuf) {
		struct dma_buf *ret = lg_loonggpu_gem_prime_export(
			&lg_gbo_to_gem_obj(mem->bo),
			mem->alloc_flags & KCD_IOC_ALLOC_MEM_FLAGS_WRITABLE ?
				DRM_RDWR : 0);
		if (IS_ERR(ret))
			return PTR_ERR(ret);
		mem->dmabuf = ret;
	}

	return 0;
}

static int
kcd_mem_attach_dmabuf(struct loonggpu_device *adev, struct kgd_mem *mem,
		      struct loonggpu_bo **bo)
{
	struct drm_gem_object *gobj;
	int ret;

	ret = kcd_mem_export_dmabuf(mem);
	if (ret)
		return ret;

	gobj = loonggpu_gem_prime_import(adev_to_drm(adev), mem->dmabuf);
	if (IS_ERR(gobj))
		return PTR_ERR(gobj);

	*bo = gem_to_loonggpu_bo(gobj);
	(*bo)->flags |= LOONGGPU_GEM_CREATE_PREEMPTIBLE;

	return 0;
}

/* kcd_mem_attach - Add a BO to a VM
 *
 * Everything that needs to bo done only once when a BO is first added
 * to a VM. It can later be mapped and unmapped many times without
 * repeating these steps.
 *
 * 0. Create BO for DMA mapping, if needed
 * 1. Allocate and initialize BO VA entry data structure
 * 2. Add BO to the VM
 * 3. Determine ASIC-specific PTE flags
 * 4. Alloc page tables and directories if needed
 * 4a.  Validate new page tables and directories
 */
static int kcd_mem_attach(struct loonggpu_device *adev, struct kgd_mem *mem,
		struct loonggpu_vm *vm)
{
	struct loonggpu_device *bo_adev = loonggpu_ttm_adev(mem->bo->tbo.bdev);
	unsigned long bo_size = lg_gbo_to_gem_obj(mem->bo).size;
	struct loonggpu_bo *pd = vm->root.base.bo;
	uint64_t va = mem->va;
	struct kcd_mem_attachment *attachment[2] = {NULL, NULL};
	struct loonggpu_bo *bo[2] = {NULL, NULL};
	int i = 0, ret;

	if (!va) {
		pr_err("Invalid VA when adding BO to VM\n");
		return -EINVAL;
	}

	/* Determine access to VRAM and DOORBELL BOs of peer devices.
	 *
	 * The access path of DOORBELL BOs is always over PCIe. In contrast
	 * the access path of VRAM BOs depends upon the type of link that
	 * connects the peer device. Access over PCIe is allowed if the peer
	 * device has large BAR.
	 */
	if ((adev != bo_adev && !adev->gmc.is_app_apu) &&
	    ((mem->domain == LOONGGPU_GEM_DOMAIN_VRAM) ||
	     (mem->alloc_flags & KCD_IOC_ALLOC_MEM_FLAGS_DOORBELL))) {
		if (!loonggpu_device_is_peer_accessible(bo_adev, adev))
			return -EINVAL;
	}

	attachment[i] = kzalloc(sizeof(*attachment[i]), GFP_KERNEL);
	if (unlikely(!attachment[i])) {
		ret = -ENOMEM;
		goto unwind;
	}

	pr_debug("\t add VA 0x%llx - 0x%llx to vm %p\n", va,
			va + bo_size, vm);

	if ((mem->bo->tbo.type != ttm_bo_type_sg) && (adev == bo_adev ||
		(loonggpu_ttm_tt_get_usermm(mem->bo->tbo.ttm) && reuse_dmamap(adev, bo_adev)))) {
		/* Mappings on the local GPU, or VRAM mappings in the
			* local hive, or userptr mapping can reuse dma map
			* address space share the original BO
			*/
		attachment[i]->type = KCD_MEM_ATT_SHARED;
		bo[i] = mem->bo;
		drm_gem_object_get(&lg_gbo_to_gem_obj(bo[i]));
	} else if (i > 0) {
		/* Multiple mappings on the same GPU share the BO */
		attachment[i]->type = KCD_MEM_ATT_SHARED;
		bo[i] = bo[0];
		drm_gem_object_get(&lg_gbo_to_gem_obj(bo[i]));
	} else if (loonggpu_ttm_tt_get_usermm(mem->bo->tbo.ttm)) {
		/* Create an SG BO to DMA-map userptrs on other GPUs */
		attachment[i]->type = KCD_MEM_ATT_USERPTR;
		ret = create_dmamap_sg_bo(adev, mem, &bo[i]);
		if (ret)
			goto unwind;
	/* Handle DOORBELL BOs of peer devices and MMIO BOs of local and peer devices */
	} else if (mem->bo->tbo.type == ttm_bo_type_sg) {
		WARN_ONCE(!(mem->alloc_flags & KCD_IOC_ALLOC_MEM_FLAGS_DOORBELL),
				"Handing invalid SG BO in ATTACH request");
		attachment[i]->type = KCD_MEM_ATT_SG;
		ret = create_dmamap_sg_bo(adev, mem, &bo[i]);
		if (ret)
			goto unwind;
	/* Enable acces to GTT and VRAM BOs of peer devices */
	} else if (mem->domain == LOONGGPU_GEM_DOMAIN_GTT ||
			mem->domain == LOONGGPU_GEM_DOMAIN_VRAM) {
		attachment[i]->type = KCD_MEM_ATT_DMABUF;
		ret = kcd_mem_attach_dmabuf(adev, mem, &bo[i]);
		if (ret)
			goto unwind;
		pr_debug("Employ DMABUF mechanism to enable peer GPU access\n");
	} else {
		WARN_ONCE(true, "Handling invalid ATTACH request");
		ret = -EINVAL;
		goto unwind;
	}

	/* Add BO to VM internal data structures */
	ret = loonggpu_bo_reserve(bo[i], false);
	if (ret) {
		pr_debug("Unable to reserve BO during memory attach");
		goto unwind;
	}
	attachment[i]->bo_va = loonggpu_vm_bo_add(adev, vm, bo[i]);
	loonggpu_bo_unreserve(bo[i]);
	if (unlikely(!attachment[i]->bo_va)) {
		ret = -ENOMEM;
		pr_err("Failed to add BO object to VM. ret == %d\n",
			ret);
		goto unwind;
	}
	attachment[i]->va = va;
	attachment[i]->pte_flags = get_pte_flags(adev, mem);
	attachment[i]->adev = adev;
	list_add(&attachment[i]->list, &mem->attachments);

	/* Allocate new page tables if needed and validate
	* them. Clearing of new page tables and validate need to wait
	* on move fences. We don't want that to trigger the eviction
	* fence, so remove it temporarily.
	*/
	ret = loonggpu_bo_reserve(pd, false);
	if (ret) {
		pr_debug("Unable to reserve pd during memory attach");
		goto unwind;
	}
	loonggpu_lgkcd_remove_eviction_fence(pd,
					vm->process_info->eviction_fence,
					NULL, NULL);

	ret = loonggpu_vm_alloc_pts(adev, vm, va, bo_size);
	if (ret) {
		pr_err("Failed to allocate pts, err=%d\n", ret);
		goto err_alloc_pts;
	}

	ret = vm_validate_pt_pd_bos(vm);
	if (ret) {
		pr_err("validate_pt_pd_bos() failed\n");
		goto err_alloc_pts;
	}

	/* Add the eviction fence back */
	loonggpu_bo_fence(pd, &vm->process_info->eviction_fence->base, true);
	loonggpu_bo_unreserve(pd);
	va += bo_size;

	return 0;

err_alloc_pts:
	loonggpu_bo_unreserve(pd);
	loonggpu_bo_fence(pd, &vm->process_info->eviction_fence->base, true);
unwind:
	if (!attachment[i])
		return ret;
	if (attachment[i]->bo_va) {
		loonggpu_bo_reserve(bo[i], true);
		loonggpu_vm_bo_rmv(adev, attachment[i]->bo_va);
		loonggpu_bo_unreserve(bo[i]);
		list_del(&attachment[i]->list);
	}
	if (bo[i])
		lg_drm_gem_object_put(&lg_gbo_to_gem_obj(bo[i]));
	kfree(attachment[i]);
	return ret;
}

static void kcd_mem_detach(struct kcd_mem_attachment *attachment)
{
	struct loonggpu_bo *bo = attachment->bo_va->base.bo;

	pr_debug("\t remove VA 0x%llx in entry %p\n",
			attachment->va, attachment);
	loonggpu_vm_bo_rmv(attachment->adev, attachment->bo_va);
	lg_drm_gem_object_put(&lg_gbo_to_gem_obj(bo));
	list_del(&attachment->list);
	kfree(attachment);
}

static void add_kgd_mem_to_kcd_bo_list(struct kgd_mem *mem,
				struct lgkcd_process_info *process_info,
				bool userptr)
{
	struct ttm_validate_buffer *entry = &mem->validate_list;
	struct loonggpu_bo *bo = mem->bo;

	INIT_LIST_HEAD(&entry->head);
	lg_ttm_validate_buffer_set_shared(entry, true);
	entry->bo = &bo->tbo;
	mutex_lock(&process_info->lock);
	if (userptr)
		list_add_tail(&entry->head,
			      &process_info->userptr_valid_list);
	else
		list_add_tail(&entry->head, &process_info->kcd_bo_list);
	mutex_unlock(&process_info->lock);
}

static void remove_kgd_mem_from_kcd_bo_list(struct kgd_mem *mem,
		struct lgkcd_process_info *process_info)
{
	mutex_lock(&process_info->lock);
	list_del(&mem->validate_list.head);
	mutex_unlock(&process_info->lock);
}

/* Initializes user pages. It registers the MMU notifier and validates
 * the userptr BO in the GTT domain.
 *
 * The BO must already be on the userptr_valid_list. Otherwise an
 * eviction and restore may happen that leaves the new BO unmapped
 * with the user mode queues running.
 *
 * Takes the process_info->lock to protect against concurrent restore
 * workers.
 *
 * Returns 0 for success, negative errno for errors.
 */
static int init_user_pages(struct kgd_mem *mem, uint64_t user_addr)
{
	struct lgkcd_process_info *process_info = mem->process_info;
	struct loonggpu_bo *bo = mem->bo;
	struct ttm_operation_ctx ctx = { true, false };
	int ret = 0;

	mutex_lock(&process_info->lock);

	ret = loonggpu_ttm_tt_set_userptr(bo->tbo.ttm, user_addr, 0);
	if (ret) {
		pr_err("%s: Failed to set userptr: %d\n", __func__, ret);
		goto out;
	}

	ret = loonggpu_mn_register(bo, user_addr);
	if (ret) {
		pr_err("%s: Failed to register MMU notifier: %d\n",
		       __func__, ret);
		goto out;
	}

	/* If no restore worker is running concurrently, user_pages
	 * should not be allocated
	 */
	WARN(mem->user_pages, "Leaking user_pages array");

	mem->user_pages = kvmalloc_array(bo->tbo.ttm->num_pages,
					   sizeof(struct page *),
					   GFP_KERNEL | __GFP_ZERO);
	if (!mem->user_pages) {
		pr_err("%s: Failed to allocate pages array\n", __func__);
		ret = -ENOMEM;
		goto unregister_out;
	}

	ret = loonggpu_ttm_tt_get_user_pages(bo->tbo.ttm, mem->user_pages);
	if (ret) {
		pr_err("%s: Failed to get user pages: %d\n", __func__, ret);
		goto free_out;
	}

	loonggpu_ttm_tt_set_user_pages(bo->tbo.ttm, mem->user_pages);

	ret = loonggpu_bo_reserve(bo, true);
	if (ret) {
		pr_err("%s: Failed to reserve BO\n", __func__);
		goto release_out;
	}
	loonggpu_bo_placement_from_domain(bo, mem->domain);
	ret = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
	if (ret)
		pr_err("%s: failed to validate BO\n", __func__);
	loonggpu_bo_unreserve(bo);

release_out:
	if (ret)
		release_pages(mem->user_pages, bo->tbo.ttm->num_pages);
free_out:
	kvfree(mem->user_pages);
	mem->user_pages = NULL;
unregister_out:
	if (ret)
		loonggpu_mn_unregister(bo);
out:
	mutex_unlock(&process_info->lock);
	return ret;
}

/* Reserving a BO and its page table BOs must happen atomically to
 * avoid deadlocks. Some operations update multiple VMs at once. Track
 * all the reservation info in a context structure. Optionally a sync
 * object can track VM updates.
 */
struct bo_vm_reservation_context {
	struct loonggpu_bo_list_entry kcd_bo; /* BO list entry for the KCD BO */
	unsigned int n_vms;		    /* Number of VMs reserved	    */
	struct loonggpu_bo_list_entry *vm_pd; /* Array of VM BO list entries  */
	struct ww_acquire_ctx ticket;	    /* Reservation ticket	    */
	struct list_head list, duplicates;  /* BO lists			    */
	struct loonggpu_sync *sync;	    /* Pointer to sync object	    */
	bool reserved;			    /* Whether BOs are reserved	    */
};

enum bo_vm_match {
	BO_VM_NOT_MAPPED = 0,	/* Match VMs where a BO is not mapped */
	BO_VM_MAPPED,		/* Match VMs where a BO is mapped     */
	BO_VM_ALL,		/* Match all VMs a BO was added to    */
};

/**
 * reserve_bo_and_vm - reserve a BO and a VM unconditionally.
 * @mem: KCD BO structure.
 * @vm: the VM to reserve.
 * @ctx: the struct that will be used in unreserve_bo_and_vms().
 */
static int reserve_bo_and_vm(struct kgd_mem *mem,
			      struct loonggpu_vm *vm,
			      struct bo_vm_reservation_context *ctx)
{
	struct loonggpu_bo *bo = mem->bo;
	int ret;

	WARN_ON(!vm);

	ctx->reserved = false;
	ctx->n_vms = 1;
	ctx->sync = &mem->sync;

	INIT_LIST_HEAD(&ctx->list);
	INIT_LIST_HEAD(&ctx->duplicates);

	ctx->vm_pd = kcalloc(ctx->n_vms, sizeof(*ctx->vm_pd), GFP_KERNEL);
	if (!ctx->vm_pd)
		return -ENOMEM;

	ctx->kcd_bo.robj = bo;
	ctx->kcd_bo.priority = 0;
	ctx->kcd_bo.tv.bo = &bo->tbo;
	lg_ttm_validate_buffer_set_shared(&ctx->kcd_bo.tv, true);
	ctx->kcd_bo.user_pages = NULL;
	list_add(&ctx->kcd_bo.tv.head, &ctx->list);

	loonggpu_vm_get_pd_bo(vm, &ctx->list, &ctx->vm_pd[0]);

	ret = lg_ttm_eu_reserve_buffers(&ctx->ticket, &ctx->list,
				     false, &ctx->duplicates, false);
	if (!ret)
		ctx->reserved = true;
	else {
		pr_err("Failed to reserve buffers in ttm\n");
		kfree(ctx->vm_pd);
		ctx->vm_pd = NULL;
	}

	return ret;
}

/**
 * reserve_bo_and_cond_vms - reserve a BO and some VMs conditionally
 * @mem: KCD BO structure.
 * @vm: the VM to reserve. If NULL, then all VMs associated with the BO
 * is used. Otherwise, a single VM associated with the BO.
 * @map_type: the mapping status that will be used to filter the VMs.
 * @ctx: the struct that will be used in unreserve_bo_and_vms().
 *
 * Returns 0 for success, negative for failure.
 */
static int reserve_bo_and_cond_vms(struct kgd_mem *mem,
				struct loonggpu_vm *vm, enum bo_vm_match map_type,
				struct bo_vm_reservation_context *ctx)
{
	struct loonggpu_bo *bo = mem->bo;
	struct kcd_mem_attachment *entry;
	unsigned int i;
	int ret;

	ctx->reserved = false;
	ctx->n_vms = 0;
	ctx->vm_pd = NULL;
	ctx->sync = &mem->sync;

	INIT_LIST_HEAD(&ctx->list);
	INIT_LIST_HEAD(&ctx->duplicates);

	list_for_each_entry(entry, &mem->attachments, list) {
		if ((vm && vm != entry->bo_va->base.vm) ||
			(entry->is_mapped != map_type
			&& map_type != BO_VM_ALL))
			continue;

		ctx->n_vms++;
	}

	if (ctx->n_vms != 0) {
		ctx->vm_pd = kcalloc(ctx->n_vms, sizeof(*ctx->vm_pd),
				     GFP_KERNEL);
		if (!ctx->vm_pd)
			return -ENOMEM;
	}

	ctx->kcd_bo.robj = bo;
	ctx->kcd_bo.priority = 0;
	ctx->kcd_bo.tv.bo = &bo->tbo;
	lg_ttm_validate_buffer_set_shared(&ctx->kcd_bo.tv, true);
	ctx->kcd_bo.user_pages = NULL;
	list_add(&ctx->kcd_bo.tv.head, &ctx->list);

	i = 0;
	list_for_each_entry(entry, &mem->attachments, list) {
		if ((vm && vm != entry->bo_va->base.vm) ||
			(entry->is_mapped != map_type
			&& map_type != BO_VM_ALL))
			continue;

		loonggpu_vm_get_pd_bo(entry->bo_va->base.vm, &ctx->list,
				&ctx->vm_pd[i]);
		i++;
	}

	ret = lg_ttm_eu_reserve_buffers(&ctx->ticket, &ctx->list,
				     false, &ctx->duplicates, false);
	if (!ret)
		ctx->reserved = true;
	else
		pr_err("Failed to reserve buffers in ttm.\n");

	if (ret) {
		kfree(ctx->vm_pd);
		ctx->vm_pd = NULL;
	}

	return ret;
}

/**
 * unreserve_bo_and_vms - Unreserve BO and VMs from a reservation context
 * @ctx: Reservation context to unreserve
 * @wait: Optionally wait for a sync object representing pending VM updates
 * @intr: Whether the wait is interruptible
 *
 * Also frees any resources allocated in
 * reserve_bo_and_(cond_)vm(s). Returns the status from
 * loonggpu_sync_wait.
 */
static int unreserve_bo_and_vms(struct bo_vm_reservation_context *ctx,
				 bool wait, bool intr)
{
	int ret = 0;

	if (wait)
		ret = loonggpu_sync_wait(ctx->sync, intr);

	if (ctx->reserved)
		ttm_eu_backoff_reservation(&ctx->ticket, &ctx->list);
	kfree(ctx->vm_pd);

	ctx->sync = NULL;

	ctx->reserved = false;
	ctx->vm_pd = NULL;

	return ret;
}

static void unmap_bo_from_gpuvm(struct kgd_mem *mem,
				struct kcd_mem_attachment *entry,
				struct loonggpu_sync *sync)
{
	struct loonggpu_bo_va *bo_va = entry->bo_va;
	struct loonggpu_device *adev = entry->adev;
	struct loonggpu_vm *vm = bo_va->base.vm;
	struct loonggpu_bo *pd = vm->root.base.bo;

	/* Remove eviction fence from PD (and thereby from PTs too as
	 * they share the resv. object). Otherwise during PT update
	 * job (see loonggpu_vm_bo_update_mapping), eviction fence would
	 * get added to job->sync object and job execution would
	 * trigger the eviction fence.
	 */
	loonggpu_lgkcd_remove_eviction_fence(pd,
					    vm->process_info->eviction_fence,
					    NULL, NULL);

	loonggpu_vm_bo_unmap(adev, bo_va, entry->va);

	loonggpu_vm_clear_freed(adev, vm, &bo_va->last_pt_update);

	/* Add the eviction fence back */
	loonggpu_bo_fence(pd, &vm->process_info->eviction_fence->base, true);

	sync_vm_fence(adev, sync, bo_va->last_pt_update);

	kcd_mem_dmaunmap_attachment(mem, entry);
}

static int update_gpuvm_pte(struct kgd_mem *mem,
			    struct kcd_mem_attachment *entry,
			    struct loonggpu_sync *sync)
{
	struct loonggpu_vm *vm;
	struct loonggpu_bo_va *bo_va;
	struct loonggpu_bo *bo;
	struct loonggpu_device *adev = entry->adev;
	int ret;

	bo_va = entry->bo_va;
	vm = bo_va->base.vm;
	bo = bo_va->base.bo;

	ret = kcd_mem_dmamap_attachment(mem, entry);
	if (ret)
		return ret;

	/* Update the page tables  */
	ret = loonggpu_vm_bo_update(adev, bo_va, false);
	if (ret) {
		pr_err("loonggpu_vm_bo_update failed\n");
		return ret;
	}

	return sync_vm_fence(adev, sync, bo_va->last_pt_update);
}

static int map_bo_to_gpuvm(struct kgd_mem *mem,
			   struct kcd_mem_attachment *entry,
			   struct loonggpu_sync *sync,
			   bool no_update_pte)
{
	int ret;

	/* Set virtual address for the allocation */
	ret = loonggpu_vm_bo_map(entry->adev, entry->bo_va, entry->va, 0,
			       loonggpu_bo_size(entry->bo_va->base.bo),
			       entry->pte_flags);
	if (ret) {
		pr_err("Failed to map VA 0x%llx in vm. ret %d\n",
				entry->va, ret);
		return ret;
	}

	if (no_update_pte)
		return 0;

	ret = update_gpuvm_pte(mem, entry, sync);
	if (ret) {
		pr_err("update_gpuvm_pte() failed\n");
		goto update_gpuvm_pte_failed;
	}

	return 0;

update_gpuvm_pte_failed:
	unmap_bo_from_gpuvm(mem, entry, sync);
	return ret;
}

static int process_validate_vms(struct lgkcd_process_info *process_info)
{
	struct loonggpu_vm *peer_vm;
	int ret;

	list_for_each_entry(peer_vm, &process_info->vm_list_head,
			    vm_list_node) {
		ret = vm_validate_pt_pd_bos(peer_vm);
		if (ret)
			return ret;
	}

	return 0;
}

static int process_sync_pds_resv(struct lgkcd_process_info *process_info,
				 struct loonggpu_sync *sync)
{
	struct loonggpu_vm *peer_vm;
	int ret;

	list_for_each_entry(peer_vm, &process_info->vm_list_head,
			    vm_list_node) {
		struct loonggpu_bo *pd = peer_vm->root.base.bo;

		ret = loonggpu_sync_resv(loonggpu_ttm_adev(pd->tbo.bdev), sync, lg_tbo_to_resv(&pd->tbo), LOONGGPU_SYNC_ALWAYS,
				       LOONGGPU_FENCE_OWNER_KCD, false);
		if (ret)
			return ret;
	}

	return 0;
}

static int process_update_pds(struct lgkcd_process_info *process_info,
			      struct loonggpu_sync *sync)
{
	struct loonggpu_vm *peer_vm;
	int ret;

	list_for_each_entry(peer_vm, &process_info->vm_list_head,
			    vm_list_node) {
		ret = vm_update_pds(peer_vm, sync);
		if (ret)
			return ret;
	}

	return 0;
}

static int init_kcd_vm(struct loonggpu_vm *vm, void **process_info,
		       struct dma_fence **ef)
{
	struct lgkcd_process_info *info = NULL;
	int ret;

	if (!*process_info) {
		info = kzalloc(sizeof(*info), GFP_KERNEL);
		if (!info)
			return -ENOMEM;

		mutex_init(&info->lock);
		mutex_init(&info->notifier_lock);
		INIT_LIST_HEAD(&info->vm_list_head);
		INIT_LIST_HEAD(&info->kcd_bo_list);
		INIT_LIST_HEAD(&info->userptr_valid_list);
		INIT_LIST_HEAD(&info->userptr_inval_list);

		info->eviction_fence =
			loonggpu_lgkcd_fence_create(dma_fence_context_alloc(1),
						   current->mm,
						   NULL);
		if (!info->eviction_fence) {
			pr_err("Failed to create eviction fence\n");
			ret = -ENOMEM;
			goto create_evict_fence_fail;
		}

		info->pid = get_task_pid(current->group_leader, PIDTYPE_PID);
		INIT_DELAYED_WORK(&info->restore_userptr_work,
				  loonggpu_lgkcd_restore_userptr_worker);

		*process_info = info;
		*ef = dma_fence_get(&info->eviction_fence->base);
	}

	vm->process_info = *process_info;

	/* Validate page directory and attach eviction fence */
	ret = loonggpu_bo_reserve(vm->root.base.bo, true);
	if (ret)
		goto reserve_pd_fail;
	ret = vm_validate_pt_pd_bos(vm);
	if (ret) {
		pr_err("validate_pt_pd_bos() failed\n");
		goto validate_pd_fail;
	}
	ret = lg_ttm_bo_wait(&vm->root.base.bo->tbo, false, false);
	if (ret)
		goto wait_pd_fail;
	loonggpu_bo_fence(vm->root.base.bo,
		       &vm->process_info->eviction_fence->base, true);
	loonggpu_bo_unreserve(vm->root.base.bo);

	/* Update process info */
	mutex_lock(&vm->process_info->lock);
	list_add_tail(&vm->vm_list_node,
			&(vm->process_info->vm_list_head));
	vm->process_info->n_vms++;
	mutex_unlock(&vm->process_info->lock);

	return 0;

wait_pd_fail:
validate_pd_fail:
	loonggpu_bo_unreserve(vm->root.base.bo);
reserve_pd_fail:
	vm->process_info = NULL;
	if (info) {
		/* Two fence references: one in info and one in *ef */
		dma_fence_put(&info->eviction_fence->base);
		dma_fence_put(*ef);
		*ef = NULL;
		*process_info = NULL;
		put_pid(info->pid);
create_evict_fence_fail:
		mutex_destroy(&info->lock);
		mutex_destroy(&info->notifier_lock);
		kfree(info);
	}
	return ret;
}

int loonggpu_lgkcd_gpuvm_set_vm_pasid(struct loonggpu_device *adev,
				     struct loonggpu_vm *avm, u32 pasid)

{
	int ret;

	/* Free the original loonggpu allocated pasid,
	 * will be replaced with kcd allocated pasid.
	 */
	if (avm->pasid) {
		loonggpu_pasid_free(avm->pasid);
		loonggpu_vm_set_pasid(adev, avm, 0);
	}

	ret = loonggpu_vm_set_pasid(adev, avm, pasid);
	if (ret)
		return ret;

	return 0;
}

int loonggpu_lgkcd_gpuvm_acquire_process_vm(struct loonggpu_device *adev,
					   struct loonggpu_vm *avm,
					   void **process_info,
					   struct dma_fence **ef)
{
	int ret;

	/* Already a compute VM? */
	if (avm->process_info)
		return -EINVAL;

	/* Convert VM into a compute VM */
	ret = loonggpu_vm_make_compute(adev, avm);
	if (ret)
		return ret;

	/* Initialize KCD part of the VM and process info */
	ret = init_kcd_vm(avm, process_info, ef);
	if (ret)
		return ret;

	loonggpu_vm_set_task_info(avm);

	return 0;
}

void loonggpu_lgkcd_gpuvm_destroy_cb(struct loonggpu_device *adev,
				    struct loonggpu_vm *vm)
{
	struct lgkcd_process_info *process_info = vm->process_info;
	struct loonggpu_bo *pd = vm->root.base.bo;

	if (!process_info)
		return;

	/* Release eviction fence from PD */
	if (lg_tbo_to_resv(&pd->tbo) == lg_tbo_to_ttm_resv(&pd->tbo))
		loonggpu_lgkcd_remove_fence_on_pt_pd_bos(pd);

	/* Update process info */
	mutex_lock(&process_info->lock);
	process_info->n_vms--;
	list_del(&vm->vm_list_node);
	mutex_unlock(&process_info->lock);

	vm->process_info = NULL;

	/* Release per-process resources when last compute VM is destroyed */
	if (!process_info->n_vms) {
		WARN_ON(!list_empty(&process_info->kcd_bo_list));
		WARN_ON(!list_empty(&process_info->userptr_valid_list));
		WARN_ON(!list_empty(&process_info->userptr_inval_list));

		dma_fence_put(&process_info->eviction_fence->base);
		cancel_delayed_work_sync(&process_info->restore_userptr_work);
		put_pid(process_info->pid);
		mutex_destroy(&process_info->lock);
		mutex_destroy(&process_info->notifier_lock);
		kfree(process_info);
	}
}

void loonggpu_lgkcd_gpuvm_release_process_vm(struct loonggpu_device *adev,
					    void *drm_priv)
{
	struct loonggpu_vm *avm;

	if (WARN_ON(!adev || !drm_priv))
		return;

	avm = drm_priv_to_vm(drm_priv);

	pr_debug("Releasing process vm %p\n", avm);

	/* The original pasid of loonggpu vm has already been
	 * released during making a loonggpu vm to a compute vm
	 * The current pasid is managed by kcd and will be
	 * released on kcd process destroy. Set loonggpu pasid
	 * to 0 to avoid duplicate release.
	 */
	loonggpu_vm_release_compute(adev, avm);
}

uint64_t loonggpu_lgkcd_gpuvm_get_process_page_dir(void *drm_priv)
{
	struct loonggpu_vm *avm = drm_priv_to_vm(drm_priv);

	return avm->pd_phys_addr;
}

size_t loonggpu_lgkcd_get_available_memory(struct loonggpu_device *adev)
{
	uint64_t reserved_for_pt =
		ESTIMATE_PT_SIZE(loonggpu_lgkcd_total_mem_size);
	ssize_t available;
	uint64_t vram_available, system_mem_available, ttm_mem_available;

	spin_lock(&kcd_mem_limit.mem_limit_lock);
	vram_available = loonggpu_lgkcd_real_memory_size(adev)
		- adev->kcd.vram_used_aligned[0]
		- atomic64_read(&adev->vram_pin_size)
		- reserved_for_pt;

	if (adev->gmc.is_app_apu) {
		system_mem_available = no_system_mem_limit ?
					kcd_mem_limit.max_system_mem_limit :
					kcd_mem_limit.max_system_mem_limit -
					kcd_mem_limit.system_mem_used;

		ttm_mem_available = kcd_mem_limit.max_ttm_mem_limit -
				kcd_mem_limit.ttm_mem_used;

		available = min3(system_mem_available, ttm_mem_available,
				 vram_available);
		available = ALIGN_DOWN(available, PAGE_SIZE);
	} else {
		available = ALIGN_DOWN(vram_available, VRAM_AVAILABLITY_ALIGN);
	}

	spin_unlock(&kcd_mem_limit.mem_limit_lock);

	if (available < 0)
		available = 0;

	return available;
}

/**
 * loonggpu_lgkcd_gpuvm_pin_bo() - Pins a BO using following criteria
 * @bo: Handle of buffer object being pinned
 * @domain: Domain into which BO should be pinned
 *
 *   - USERPTR BOs are UNPINNABLE and will return error
 *   - All other BO types (GTT, VRAM and DOORBELL) will have their
 *     PIN count incremented. It is valid to PIN a BO multiple times
 *
 * Return: ZERO if successful in pinning, Non-Zero in case of error.
 */
static int loonggpu_lgkcd_gpuvm_pin_bo(struct loonggpu_bo *bo, u32 domain)
{
	int ret = 0;

	ret = loonggpu_bo_reserve(bo, false);
	if (unlikely(ret))
		return ret;

	ret = loonggpu_bo_pin_restricted(bo, domain, 0, 0);
	if (ret)
		pr_err("Error in Pinning BO to domain: %d\n", domain);

	lg_ttm_bo_wait(&bo->tbo, false, false);
	loonggpu_bo_unreserve(bo);

	return ret;
}

/**
 * loonggpu_lgkcd_gpuvm_unpin_bo() - Unpins BO using following criteria
 * @bo: Handle of buffer object being unpinned
 *
 *   - Is a illegal request for USERPTR BOs and is ignored
 *   - All other BO types (GTT, VRAM, MMIO and DOORBELL) will have their
 *     PIN count decremented. Calls to UNPIN must balance calls to PIN
 */
static void loonggpu_lgkcd_gpuvm_unpin_bo(struct loonggpu_bo *bo)
{
	int ret = 0;

	ret = loonggpu_bo_reserve(bo, false);
	if (unlikely(ret))
		return;

	loonggpu_bo_unpin(bo);
	loonggpu_bo_unreserve(bo);
}

int loonggpu_lgkcd_gpuvm_alloc_memory_of_gpu(
		struct loonggpu_device *adev, uint64_t va, uint64_t size,
		void *drm_priv, struct kgd_mem **mem,
		uint64_t *offset, uint32_t flags)
{
	struct loonggpu_vm *avm = drm_priv_to_vm(drm_priv);
	enum ttm_bo_type bo_type = ttm_bo_type_device;
	struct sg_table *sg = NULL;
	uint64_t user_addr = 0;
	struct loonggpu_bo *bo;
	struct loonggpu_bo_param bp;
	u32 domain, alloc_domain;
	uint64_t aligned_size;
	u64 alloc_flags;
	int ret;

	/*
	 * Check on which domain to allocate BO
	 */
	if (flags & KCD_IOC_ALLOC_MEM_FLAGS_VRAM) {
		domain = alloc_domain = LOONGGPU_GEM_DOMAIN_VRAM;

		if (adev->gmc.is_app_apu) {
			domain = LOONGGPU_GEM_DOMAIN_GTT;
			alloc_domain = LOONGGPU_GEM_DOMAIN_GTT;
			alloc_flags = 0;
		} else {
			alloc_flags |= (flags & KCD_IOC_ALLOC_MEM_FLAGS_PUBLIC) ?
			LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED : 0;
		}
	} else if (flags & KCD_IOC_ALLOC_MEM_FLAGS_GTT) {
		domain = alloc_domain = LOONGGPU_GEM_DOMAIN_GTT;
		alloc_flags = 0;
	} else {
		domain = LOONGGPU_GEM_DOMAIN_GTT;
		alloc_domain = LOONGGPU_GEM_DOMAIN_CPU;
		alloc_flags = LOONGGPU_GEM_CREATE_PREEMPTIBLE;

		if (flags & KCD_IOC_ALLOC_MEM_FLAGS_USERPTR) {
			if (!offset || !*offset)
				return -EINVAL;
			user_addr = *offset;
		} else if (flags & KCD_IOC_ALLOC_MEM_FLAGS_DOORBELL) {
			bo_type = ttm_bo_type_sg;
			if (size > UINT_MAX)
				return -EINVAL;
			sg = create_sg_table(*offset, size);
			if (!sg)
				return -ENOMEM;
		}else {
			return -EINVAL;
		}
	}

	*mem = kzalloc(sizeof(struct kgd_mem), GFP_KERNEL);
	if (!*mem) {
		ret = -ENOMEM;
		return ret;
	}
	INIT_LIST_HEAD(&(*mem)->attachments);
	mutex_init(&(*mem)->lock);

	aligned_size = PAGE_ALIGN(size);

	(*mem)->alloc_flags = flags;

	loonggpu_sync_create(&(*mem)->sync);

	ret = loonggpu_lgkcd_reserve_mem_limit(adev, aligned_size, flags);
	if (ret) {
		pr_debug("Insufficient memory\n");
		goto err_reserve_limit;
	}

	pr_debug("\tcreate BO VA 0x%llx size 0x%llx domain %s \n",
		 va, size,
		 domain_string(alloc_domain));

	memset(&bp, 0, sizeof(bp));
	bp.size = aligned_size;
	bp.byte_align = 1;
	bp.domain = alloc_domain;
	bp.flags = alloc_flags;
	bp.type = bo_type;
	bp.resv = NULL;
	ret = loonggpu_bo_create(adev, &bp, &bo);
	if (ret) {
		pr_debug("Failed to create BO on domain %s. ret %d\n",
				domain_string(alloc_domain), ret);
		goto err_bo_create;
	}

	ret = drm_vma_node_allow(&lg_gbo_to_gem_obj(bo).vma_node, drm_priv);
	if (ret) {
		pr_debug("Failed to allow vma node access. ret %d\n", ret);
		goto err_node_allow;
	}

	if (bo_type == ttm_bo_type_sg) {
		bo->tbo.sg = sg;
		bo->tbo.ttm->sg = sg;
	}
	bo->kcd_bo = *mem;
	(*mem)->bo = bo;
	if (user_addr)
		bo->flags |= LOONGGPU_LGKCD_CREATE_USERPTR_BO;

	(*mem)->va = va;
	(*mem)->domain = domain;
	(*mem)->mapped_to_gpu_memory = 0;
	(*mem)->process_info = avm->process_info;
	lg_gbo_to_gem_obj(bo).funcs = &loonggpu_gem_object_funcs;
	add_kgd_mem_to_kcd_bo_list(*mem, avm->process_info, user_addr);

	if (user_addr) {
		pr_debug("creating userptr BO for user_addr = %llx\n", user_addr);
		ret = init_user_pages(*mem, user_addr);
		if (ret)
			goto allocate_init_user_pages_failed;
	} else  if (flags & KCD_IOC_ALLOC_MEM_FLAGS_DOORBELL) {
		ret = loonggpu_lgkcd_gpuvm_pin_bo(bo, LOONGGPU_GEM_DOMAIN_GTT);
		if (ret) {
			pr_err("Pinning MMIO/DOORBELL BO during ALLOC FAILED\n");
			goto err_pin_bo;
		}
		bo->allowed_domains = LOONGGPU_GEM_DOMAIN_GTT;
		bo->preferred_domains = LOONGGPU_GEM_DOMAIN_GTT;
	}

	if (offset)
		*offset = loonggpu_bo_mmap_offset(bo);

	return 0;

err_pin_bo:
allocate_init_user_pages_failed:
	remove_kgd_mem_from_kcd_bo_list(*mem, avm->process_info);
	drm_vma_node_revoke(&lg_gbo_to_gem_obj(bo).vma_node, drm_priv);
err_node_allow:
	lg_drm_gem_object_put(&lg_gbo_to_gem_obj(bo));
	/* Don't unreserve system mem limit twice */
	goto err_out;
err_bo_create:
	loonggpu_lgkcd_unreserve_mem_limit(adev, aligned_size, flags);
err_reserve_limit:
	mutex_destroy(&(*mem)->lock);
	kfree(*mem);
err_out:
	return ret;
}

int loonggpu_lgkcd_gpuvm_free_memory_of_gpu(
		struct loonggpu_device *adev, struct kgd_mem *mem, void *drm_priv,
		uint64_t *size)
{
	struct lgkcd_process_info *process_info = mem->process_info;
	unsigned long bo_size = lg_gbo_to_gem_obj(mem->bo).size;
	bool use_release_notifier = (mem->bo->kcd_bo == mem);
	struct kcd_mem_attachment *entry, *tmp;
	struct bo_vm_reservation_context ctx;
	unsigned int mapped_to_gpu_memory;
	int ret;
	bool is_imported = false;

	mutex_lock(&mem->lock);

	/* Unpin MMIO/DOORBELL BO's that were pinned during allocation */
	if (mem->alloc_flags &
	    (KCD_IOC_ALLOC_MEM_FLAGS_DOORBELL)) {
		loonggpu_lgkcd_gpuvm_unpin_bo(mem->bo);
	}

	mapped_to_gpu_memory = mem->mapped_to_gpu_memory;
	is_imported = mem->is_imported;
	mutex_unlock(&mem->lock);
	/* lock is not needed after this, since mem is unused and will
	 * be freed anyway
	 */

	if (mapped_to_gpu_memory > 0) {
		pr_debug("BO VA 0x%llx size 0x%lx is still mapped.\n",
				mem->va, bo_size);
		return -EBUSY;
	}

	/* No more MMU notifiers */
	loonggpu_mn_unregister(mem->bo);

	/* Make sure restore workers don't access the BO any more */
	mutex_lock(&process_info->lock);
	list_del(&mem->validate_list.head);
	mutex_unlock(&process_info->lock);

	/* Free user pages if necessary */
	if (mem->user_pages) {
		pr_debug("%s: Freeing user_pages array\n", __func__);
		if (mem->user_pages[0]) {
			release_pages(mem->user_pages,
					mem->bo->tbo.ttm->num_pages);
		}
		kvfree(mem->user_pages);
		mem->user_pages = NULL;
	}

	ret = reserve_bo_and_cond_vms(mem, NULL, BO_VM_ALL, &ctx);
	if (unlikely(ret))
		return ret;

	/* The eviction fence should be removed by the last unmap.
	 * TODO: Log an error condition if the bo still has the eviction fence
	 * attached
	 */
	loonggpu_lgkcd_remove_eviction_fence(mem->bo,
					process_info->eviction_fence,
					NULL, NULL);
	pr_debug("Release VA 0x%llx - 0x%llx\n", mem->va,
		mem->va + bo_size);

	/* Remove from VM internal data structures */
	list_for_each_entry_safe(entry, tmp, &mem->attachments, list)
		kcd_mem_detach(entry);

	ret = unreserve_bo_and_vms(&ctx, false, false);

	/* Free the sync object */
	loonggpu_sync_free(&mem->sync);

	/* If the SG is not NULL, it's one we created for a doorbell or mmio
	 * remap BO. We need to free it.
	 */
	if (mem->bo->tbo.sg) {
		sg_free_table(mem->bo->tbo.sg);
		kfree(mem->bo->tbo.sg);
	}

	/* Update the size of the BO being freed if it was allocated from
	 * VRAM and is not imported. For APP APU VRAM allocations are done
	 * in GTT domain
	 */
	if (size) {
		if (!is_imported &&
		   (mem->bo->preferred_domains == LOONGGPU_GEM_DOMAIN_VRAM ||
		   (adev->gmc.is_app_apu &&
		    mem->bo->preferred_domains == LOONGGPU_GEM_DOMAIN_GTT)))
			*size = bo_size;
		else
			*size = 0;
	}

	/* Unreference the ipc_obj if applicable */
	kcd_ipc_obj_put(&mem->ipc_obj);
	drm_vma_node_revoke(&lg_gbo_to_gem_obj(mem->bo).vma_node, drm_priv);

	if (mem->dmabuf)
		dma_buf_put(mem->dmabuf);

	/* Free the BO*/
	lg_drm_gem_object_put(&lg_gbo_to_gem_obj(mem->bo));

	if (!use_release_notifier)
		kfree(mem);

	return ret;
}

int loonggpu_lgkcd_gpuvm_map_memory_to_gpu(
		struct loonggpu_device *adev, struct kgd_mem *mem,
		void *drm_priv)
{
	struct loonggpu_vm *avm = drm_priv_to_vm(drm_priv);
	int ret;
	struct loonggpu_bo *bo;
	uint32_t domain;
	struct kcd_mem_attachment *entry;
	struct bo_vm_reservation_context ctx;
	unsigned long bo_size;
	bool is_invalid_userptr = false;

	bo = mem->bo;
	if (!bo) {
		pr_err("Invalid BO when mapping memory to GPU\n");
		return -EINVAL;
	}

	/* Make sure restore is not running concurrently. Since we
	 * don't map invalid userptr BOs, we rely on the next restore
	 * worker to do the mapping
	 */
	mutex_lock(&mem->process_info->lock);

	/* Lock notifier lock. If we find an invalid userptr BO, we can be
	 * sure that the MMU notifier is no longer running
	 * concurrently and the queues are actually stopped
	 */
	if (loonggpu_ttm_tt_get_usermm(bo->tbo.ttm)) {
		mutex_lock(&mem->process_info->notifier_lock);
		is_invalid_userptr = !!mem->invalid;
		mutex_unlock(&mem->process_info->notifier_lock);
	}

	mutex_lock(&mem->lock);

	domain = mem->domain;
	bo_size = lg_gbo_to_gem_obj(bo).size;

	pr_debug("Map VA 0x%llx - 0x%llx to vm %p domain %s\n",
			mem->va,
			mem->va + bo_size,
			avm, domain_string(domain));

	if (!kcd_mem_is_attached(avm, mem)) {
		ret = kcd_mem_attach(adev, mem, avm);
		if (ret)
			goto out;
	}

	ret = reserve_bo_and_vm(mem, avm, &ctx);
	if (unlikely(ret))
		goto out;

	/* Userptr can be marked as "not invalid", but not actually be
	 * validated yet (still in the system domain). In that case
	 * the queues are still stopped and we can leave mapping for
	 * the next restore worker
	 */
	if (loonggpu_ttm_tt_get_usermm(bo->tbo.ttm) &&
	    lg_get_bo_mem_type(bo) == TTM_PL_SYSTEM)
		is_invalid_userptr = true;

	ret = vm_validate_pt_pd_bos(avm);
	if (unlikely(ret))
		goto out_unreserve;

	if (mem->mapped_to_gpu_memory == 0 &&
	    !loonggpu_ttm_tt_get_usermm(bo->tbo.ttm)) {
		/* Validate BO only once. The eviction fence gets added to BO
		 * the first time it is mapped. Validate will wait for all
		 * background evictions to complete.
		 */
		ret = loonggpu_lgkcd_bo_validate(bo, domain, true);
		if (ret) {
			pr_debug("Validate failed\n");
			goto out_unreserve;
		}
	}

	list_for_each_entry(entry, &mem->attachments, list) {
		if (entry->bo_va->base.vm != avm || entry->is_mapped)
			continue;

		pr_debug("\t map VA 0x%llx - 0x%llx in entry %p\n",
			 entry->va, entry->va + bo_size, entry);

		ret = map_bo_to_gpuvm(mem, entry, ctx.sync,
				      is_invalid_userptr);
		if (ret) {
			pr_err("Failed to map bo to gpuvm\n");
			goto out_unreserve;
		}

		ret = vm_update_pds(avm, ctx.sync);
		if (ret) {
			pr_err("Failed to update page directories\n");
			goto out_unreserve;
		}

		entry->is_mapped = true;
		mem->mapped_to_gpu_memory++;
		pr_debug("\t INC mapping count %d\n",
			 mem->mapped_to_gpu_memory);
	}

	if (!loonggpu_ttm_tt_get_usermm(bo->tbo.ttm) && !bo->tbo.pin_count)
		loonggpu_bo_fence(bo,
				&avm->process_info->eviction_fence->base,
				true);
	ret = unreserve_bo_and_vms(&ctx, false, false);

	goto out;

out_unreserve:
	unreserve_bo_and_vms(&ctx, false, false);
out:
	mutex_unlock(&mem->process_info->lock);
	mutex_unlock(&mem->lock);
	return ret;
}

int loonggpu_lgkcd_gpuvm_unmap_memory_from_gpu(
		struct loonggpu_device *adev, struct kgd_mem *mem, void *drm_priv)
{
	struct loonggpu_vm *avm = drm_priv_to_vm(drm_priv);
	struct lgkcd_process_info *process_info = avm->process_info;
	unsigned long bo_size = lg_gbo_to_gem_obj(mem->bo).size;
	struct kcd_mem_attachment *entry;
	struct bo_vm_reservation_context ctx;
	int ret;

	mutex_lock(&mem->lock);

	ret = reserve_bo_and_cond_vms(mem, avm, BO_VM_MAPPED, &ctx);
	if (unlikely(ret))
		goto out;
	/* If no VMs were reserved, it means the BO wasn't actually mapped */
	if (ctx.n_vms == 0) {
		ret = -EINVAL;
		goto unreserve_out;
	}

	ret = vm_validate_pt_pd_bos(avm);
	if (unlikely(ret))
		goto unreserve_out;

	pr_debug("Unmap VA 0x%llx - 0x%llx from vm %p\n",
		mem->va,
		mem->va + bo_size,
		avm);

	list_for_each_entry(entry, &mem->attachments, list) {
		if (entry->bo_va->base.vm != avm || !entry->is_mapped)
			continue;

		pr_debug("\t unmap VA 0x%llx - 0x%llx from entry %p\n",
			 entry->va, entry->va + bo_size, entry);

		unmap_bo_from_gpuvm(mem, entry, ctx.sync);
		entry->is_mapped = false;

		mem->mapped_to_gpu_memory--;
		pr_debug("\t DEC mapping count %d\n",
			 mem->mapped_to_gpu_memory);
	}

	/* If BO is unmapped from all VMs, unfence it. It can be evicted if
	 * required.
	 */
	if (mem->mapped_to_gpu_memory == 0 &&
	    !loonggpu_ttm_tt_get_usermm(mem->bo->tbo.ttm) &&
	    !mem->bo->tbo.pin_count)
		loonggpu_lgkcd_remove_eviction_fence(mem->bo,
						process_info->eviction_fence,
						NULL, NULL);

unreserve_out:
	unreserve_bo_and_vms(&ctx, false, false);
out:
	mutex_unlock(&mem->lock);
	return ret;
}

int loonggpu_lgkcd_gpuvm_sync_memory(
		struct loonggpu_device *adev, struct kgd_mem *mem, bool intr)
{
	struct loonggpu_sync sync;
	int ret;

	loonggpu_sync_create(&sync);

	mutex_lock(&mem->lock);
	loonggpu_sync_clone(&mem->sync, &sync);
	mutex_unlock(&mem->lock);

	ret = loonggpu_sync_wait(&sync, intr);
	loonggpu_sync_free(&sync);
	return ret;
}

/**
 * loonggpu_lgkcd_map_gtt_bo_to_gart - Map BO to GART and increment reference count
 * @adev: Device to which allocated BO belongs
 * @bo: Buffer object to be mapped
 *
 * Before return, bo reference count is incremented. To release the reference and unpin/
 * unmap the BO, call loonggpu_lgkcd_free_gtt_mem.
 */
int loonggpu_lgkcd_map_gtt_bo_to_gart(struct loonggpu_device *adev, struct loonggpu_bo *bo)
{
	int ret;

	ret = loonggpu_bo_reserve(bo, true);
	if (ret) {
		pr_err("Failed to reserve bo. ret %d\n", ret);
		goto err_reserve_bo_failed;
	}

	ret = loonggpu_bo_pin(bo, LOONGGPU_GEM_DOMAIN_GTT);
	if (ret) {
		pr_err("Failed to pin bo. ret %d\n", ret);
		goto err_pin_bo_failed;
	}

	ret = loonggpu_ttm_alloc_gart(&bo->tbo);
	if (ret) {
		pr_err("Failed to bind bo to GART. ret %d\n", ret);
		goto err_map_bo_gart_failed;
	}

	loonggpu_lgkcd_remove_eviction_fence(
		bo, bo->kcd_bo->process_info->eviction_fence, NULL, NULL);

	loonggpu_bo_unreserve(bo);

	bo = loonggpu_bo_ref(bo);

	return 0;

err_map_bo_gart_failed:
	loonggpu_bo_unpin(bo);
err_pin_bo_failed:
	loonggpu_bo_unreserve(bo);
err_reserve_bo_failed:

	return ret;
}

/** loonggpu_lgkcd_gpuvm_map_gtt_bo_to_kernel() - Map a GTT BO for kernel CPU access
 *
 * @mem: Buffer object to be mapped for CPU access
 * @kptr[out]: pointer in kernel CPU address space
 * @size[out]: size of the buffer
 *
 * Pins the BO and maps it for kernel CPU access. The eviction fence is removed
 * from the BO, since pinned BOs cannot be evicted. The bo must remain on the
 * validate_list, so the GPU mapping can be restored after a page table was
 * evicted.
 *
 * Return: 0 on success, error code on failure
 */
int loonggpu_lgkcd_gpuvm_map_gtt_bo_to_kernel(struct kgd_mem *mem,
					     void **kptr, uint64_t *size)
{
	int ret;
	struct loonggpu_bo *bo = mem->bo;

	if (loonggpu_ttm_tt_get_usermm(bo->tbo.ttm)) {
		pr_err("userptr can't be mapped to kernel\n");
		return -EINVAL;
	}

	mutex_lock(&mem->process_info->lock);

	ret = loonggpu_bo_reserve(bo, true);
	if (ret) {
		pr_err("Failed to reserve bo. ret %d\n", ret);
		goto bo_reserve_failed;
	}

	ret = loonggpu_bo_pin(bo, LOONGGPU_GEM_DOMAIN_GTT);
	if (ret) {
		pr_err("Failed to pin bo. ret %d\n", ret);
		goto pin_failed;
	}

	ret = loonggpu_bo_kmap(bo, kptr);
	if (ret) {
		pr_err("Failed to map bo to kernel. ret %d\n", ret);
		goto kmap_failed;
	}

	loonggpu_lgkcd_remove_eviction_fence(
		bo, mem->process_info->eviction_fence, NULL, NULL);

	if (size)
		*size = loonggpu_bo_size(bo);

	loonggpu_bo_unreserve(bo);

	mutex_unlock(&mem->process_info->lock);
	return 0;

kmap_failed:
	loonggpu_bo_unpin(bo);
pin_failed:
	loonggpu_bo_unreserve(bo);
bo_reserve_failed:
	mutex_unlock(&mem->process_info->lock);

	return ret;
}

/** loonggpu_lgkcd_gpuvm_map_gtt_bo_to_kernel() - Unmap a GTT BO for kernel CPU access
 *
 * @mem: Buffer object to be unmapped for CPU access
 *
 * Removes the kernel CPU mapping and unpins the BO. It does not restore the
 * eviction fence, so this function should only be used for cleanup before the
 * BO is destroyed.
 */
void loonggpu_lgkcd_gpuvm_unmap_gtt_bo_from_kernel(struct kgd_mem *mem)
{
	struct loonggpu_bo *bo = mem->bo;

	loonggpu_bo_reserve(bo, true);
	loonggpu_bo_kunmap(bo);
	loonggpu_bo_unpin(bo);
	loonggpu_bo_unreserve(bo);
}

int loonggpu_lgkcd_gpuvm_get_vm_fault_info(struct loonggpu_device *adev,
					  struct kcd_vm_fault_info *mem)
{
	if (atomic_read(&adev->gmc.vm_fault_info_updated) == 1) {
		*mem = *(struct kcd_vm_fault_info *)adev->gmc.vm_fault_info;
		mb(); /* make sure read happened */
		atomic_set(&adev->gmc.vm_fault_info_updated, 0);
	}
	return 0;
}

int loonggpu_lgkcd_gpuvm_import_dmabuf(struct loonggpu_device *adev,
				      struct dma_buf *dma_buf,
				      struct kcd_ipc_obj *ipc_obj,
				      uint64_t va, void *drm_priv,
				      struct kgd_mem **mem, uint64_t *size,
				      uint64_t *mmap_offset)
{
	struct loonggpu_vm *avm = drm_priv_to_vm(drm_priv);
	struct drm_gem_object *obj;
	struct loonggpu_bo *bo;
	int ret;

	obj = loonggpu_gem_prime_import(adev_to_drm(adev), dma_buf);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	bo = gem_to_loonggpu_bo(obj);
	if (!(bo->preferred_domains & (LOONGGPU_GEM_DOMAIN_VRAM |
				    LOONGGPU_GEM_DOMAIN_GTT))) {
		/* Only VRAM and GTT BOs are supported */
		ret = -EINVAL;
		goto err_put_obj;
	}

	*mem = kzalloc(sizeof(struct kgd_mem), GFP_KERNEL);
	if (!*mem) {
		ret = -ENOMEM;
		goto err_put_obj;
	}

	ret = drm_vma_node_allow(&obj->vma_node, drm_priv);
	if (ret)
		goto err_free_mem;

	if (size)
		*size = loonggpu_bo_size(bo);

	if (mmap_offset)
		*mmap_offset = loonggpu_bo_mmap_offset(bo);

	INIT_LIST_HEAD(&(*mem)->attachments);
	mutex_init(&(*mem)->lock);

	if (bo->kcd_bo)
		(*mem)->alloc_flags = bo->kcd_bo->alloc_flags;
	else
		(*mem)->alloc_flags =
			((bo->preferred_domains & LOONGGPU_GEM_DOMAIN_VRAM) ?
			KCD_IOC_ALLOC_MEM_FLAGS_VRAM : KCD_IOC_ALLOC_MEM_FLAGS_GTT)
			| KCD_IOC_ALLOC_MEM_FLAGS_WRITABLE
			| KCD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE;

	get_dma_buf(dma_buf);
	(*mem)->dmabuf = dma_buf;
	(*mem)->bo = bo;
	(*mem)->ipc_obj = ipc_obj;
	(*mem)->va = va;
	(*mem)->domain = (bo->preferred_domains & LOONGGPU_GEM_DOMAIN_VRAM) && !adev->gmc.is_app_apu ?
		LOONGGPU_GEM_DOMAIN_VRAM : LOONGGPU_GEM_DOMAIN_GTT;

	(*mem)->mapped_to_gpu_memory = 0;
	(*mem)->process_info = avm->process_info;
	add_kgd_mem_to_kcd_bo_list(*mem, avm->process_info, false);
	loonggpu_sync_create(&(*mem)->sync);
	(*mem)->is_imported = true;

	return 0;

err_free_mem:
	kfree(*mem);
err_put_obj:
	lg_drm_gem_object_put(obj);
	return ret;
}

int loonggpu_lgkcd_gpuvm_export_dmabuf(struct kgd_mem *mem,
				      struct dma_buf **dma_buf)
{
	int ret;

	mutex_lock(&mem->lock);
	ret = kcd_mem_export_dmabuf(mem);
	if (ret)
		goto out;

	get_dma_buf(mem->dmabuf);
	*dma_buf = mem->dmabuf;
out:
	mutex_unlock(&mem->lock);
	return ret;
}

int loonggpu_lgkcd_gpuvm_export_ipc_obj(struct loonggpu_device *adev, void *vm,
				       struct kgd_mem *mem,
				       struct kcd_ipc_obj **ipc_obj,
				       uint32_t flags,
				       uint32_t *restore_handle)
{
	struct dma_buf *dmabuf;
	int r = 0;

	if (!adev || !vm || !mem)
		return -EINVAL;

	mutex_lock(&mem->lock);

	if (mem->ipc_obj) {
		*ipc_obj = mem->ipc_obj;
		goto unlock_out;
	}

	r = kcd_mem_export_dmabuf(mem);
	if (r)
		goto unlock_out;

	get_dma_buf(mem->dmabuf);
	dmabuf = mem->dmabuf;

	r = kcd_ipc_store_insert(dmabuf, &mem->ipc_obj, flags, restore_handle);
	if (r)
		dma_buf_put(dmabuf);
	else
		*ipc_obj = mem->ipc_obj;

unlock_out:
	mutex_unlock(&mem->lock);
	return r;
}

/* Evict a userptr BO by stopping the queues if necessary
 *
 * Runs in MMU notifier, may be in RECLAIM_FS context. This means it
 * cannot do any memory allocations, and cannot take any locks that
 * are held elsewhere while allocating memory.
 *
 * It doesn't do anything to the BO itself. The real work happens in
 * restore, where we get updated page addresses. This function only
 * ensures that GPU access to the BO is stopped.
 */
int loonggpu_lgkcd_evict_userptr(struct kgd_mem *mem,
				struct mm_struct *mm)
{
	struct lgkcd_process_info *process_info = mem->process_info;
	int r = 0;

	mutex_lock(&process_info->notifier_lock);

	mem->invalid++;
	if (++process_info->evicted_bos == 1) {
		/* First eviction, stop the queues */
		r = kgd2kcd_quiesce_mm(mm,
				       KCD_QUEUE_EVICTION_TRIGGER_USERPTR);
		if (r)
			pr_err("Failed to quiesce KCD\n");
		schedule_delayed_work(&process_info->restore_userptr_work,
			msecs_to_jiffies(LOONGGPU_USERPTR_RESTORE_DELAY_MS));
	}
	mutex_unlock(&process_info->notifier_lock);

	return r;
}

/* Update invalid userptr BOs
 *
 * Moves invalidated (evicted) userptr BOs from userptr_valid_list to
 * userptr_inval_list and updates user pages for all BOs that have
 * been invalidated since their last update.
 */
static int update_invalid_user_pages(struct lgkcd_process_info *process_info,
				     struct mm_struct *mm)
{
	struct kgd_mem *mem, *tmp_mem;
	struct loonggpu_bo *bo;
	struct ttm_operation_ctx ctx = { false, false };
	uint32_t invalid;
	int ret = 0;

	mutex_lock(&process_info->notifier_lock);

	/* Move all invalidated BOs to the userptr_inval_list */
	list_for_each_entry_safe(mem, tmp_mem,
				 &process_info->userptr_valid_list,
				 validate_list.head)
		if (mem->invalid)
			list_move_tail(&mem->validate_list.head,
				       &process_info->userptr_inval_list);

	/* Go through userptr_inval_list and update any invalid user_pages */
	list_for_each_entry(mem, &process_info->userptr_inval_list,
			    validate_list.head) {
		invalid = mem->invalid;
		if (!invalid)
			/* BO hasn't been invalidated since the last
			 * revalidation attempt. Keep its page list.
			 */
			continue;

		bo = mem->bo;

		/* BO reservations and getting user pages (hmm_range_fault)
		 * must happen outside the notifier lock
		 */
		mutex_unlock(&process_info->notifier_lock);

		/* Move the BO to system (CPU) domain if necessary to unmap
		 * and free the SG table
		 */
		if (lg_get_bo_mem_type(bo) != TTM_PL_SYSTEM) {
			if (loonggpu_bo_reserve(bo, true))
				return -EAGAIN;
			loonggpu_bo_placement_from_domain(bo, LOONGGPU_GEM_DOMAIN_CPU);
			ret = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
			loonggpu_bo_unreserve(bo);
			if (ret) {
				pr_err("%s: Failed to invalidate userptr BO\n",
				       __func__);
				return -EAGAIN;
			}
		}

		if (!mem->user_pages) {
			mem->user_pages =
				kvmalloc_array(bo->tbo.ttm->num_pages,
					       sizeof(struct page *),
					       GFP_KERNEL | __GFP_ZERO);
			if (!mem->user_pages) {
				pr_err("%s: Failed to allocate pages array\n",
				       __func__);
				return -ENOMEM;
			}
		} else if (mem->user_pages[0]) {
			release_pages(mem->user_pages, bo->tbo.ttm->num_pages);
		}

		/* Get updated user pages */
		ret = loonggpu_ttm_tt_get_user_pages(bo->tbo.ttm, mem->user_pages);
		if (ret) {
			mem->user_pages[0] = NULL;
			pr_debug("Failed %d to get user pages\n", ret);

			/* Return -EFAULT bad address error as success. It will
			 * fail later with a VM fault if the GPU tries to access
			 * it. Better than hanging indefinitely with stalled
			 * user mode queues.
			 *
			 * Return other error -EBUSY or -ENOMEM to retry restore
			 */
			if (ret != -EFAULT)
				return ret;

			ret = 0;
		}

		mutex_lock(&process_info->notifier_lock);

		/* Mark the BO as valid unless it was invalidated
		 * again concurrently.
		 */
		if (mem->invalid != invalid) {
			ret = -EAGAIN;
			goto unlock_out;
		}

		mem->invalid = 0;
	}

unlock_out:
	mutex_unlock(&process_info->notifier_lock);

	return ret;
}

/* Validate invalid userptr BOs
 *
 * Validates BOs on the userptr_inval_list. Also updates GPUVM page tables
 * with new page addresses and waits for the page table updates to complete.
 */
static int validate_invalid_user_pages(struct lgkcd_process_info *process_info)
{
	struct loonggpu_bo_list_entry *pd_bo_list_entries;
	struct list_head resv_list, duplicates;
	struct ww_acquire_ctx ticket;
	struct loonggpu_sync sync;

	struct loonggpu_vm *peer_vm;
	struct kgd_mem *mem, *tmp_mem;
	struct loonggpu_bo *bo;
	struct ttm_operation_ctx ctx = { false, false };
	int i, ret;

	pd_bo_list_entries = kcalloc(process_info->n_vms,
				     sizeof(struct loonggpu_bo_list_entry),
				     GFP_KERNEL);
	if (!pd_bo_list_entries) {
		pr_err("%s: Failed to allocate PD BO list entries\n", __func__);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&resv_list);
	INIT_LIST_HEAD(&duplicates);

	/* Get all the page directory BOs that need to be reserved */
	i = 0;
	list_for_each_entry(peer_vm, &process_info->vm_list_head,
			    vm_list_node)
		loonggpu_vm_get_pd_bo(peer_vm, &resv_list,
				    &pd_bo_list_entries[i++]);

	/* Add the userptr_inval_list entries to resv_list */
	list_for_each_entry(mem, &process_info->userptr_inval_list,
			    validate_list.head) {
		list_add_tail(&mem->resv_list.head, &resv_list);
		mem->resv_list.bo = mem->validate_list.bo;
		lg_ttm_validate_buffer_copy_shared(&mem->resv_list, &mem->validate_list);
	}

	/* Reserve all BOs and page tables for validation */
	ret = lg_ttm_eu_reserve_buffers(&ticket, &resv_list, false, &duplicates, false);
	WARN(!list_empty(&duplicates), "Duplicates should be empty");
	if (ret)
		goto out;

	loonggpu_sync_create(&sync);

	/* Avoid triggering eviction fences when unmapping invalid
	 * userptr BOs (waits for all fences, doesn't use
	 * FENCE_OWNER_VM)
	 */
	list_for_each_entry(peer_vm, &process_info->vm_list_head,
			    vm_list_node)
		loonggpu_lgkcd_remove_eviction_fence(peer_vm->root.base.bo,
						process_info->eviction_fence,
						NULL, NULL);

	ret = process_validate_vms(process_info);
	if (ret)
		goto unreserve_out;

	/* Validate BOs and update GPUVM page tables */
	list_for_each_entry_safe(mem, tmp_mem,
				 &process_info->userptr_inval_list,
				 validate_list.head) {
		struct kcd_mem_attachment *attachment;

		bo = mem->bo;

		/* Copy pages array and validate the BO if we got user pages */
		if (mem->user_pages[0]) {
			loonggpu_ttm_tt_set_user_pages(bo->tbo.ttm,
						     mem->user_pages);
			loonggpu_bo_placement_from_domain(bo, mem->domain);
			ret = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
			if (ret) {
				pr_err("%s: failed to validate BO\n", __func__);
				goto unreserve_out;
			}
		}

		/* Validate succeeded, now the BO owns the pages, free
		* our copy of the pointer array. Put this BO back on
		* the userptr_valid_list. If we need to revalidate
		* it, we need to start from scratch.
		*/
		kvfree(mem->user_pages);
		mem->user_pages = NULL;
		list_move_tail(&mem->validate_list.head,
			       &process_info->userptr_valid_list);

		/* Update mapping. If the BO was not validated
		 * (because we couldn't get user pages), this will
		 * clear the page table entries, which will result in
		 * VM faults if the GPU tries to access the invalid
		 * memory.
		 */
		list_for_each_entry(attachment, &mem->attachments, list) {
			if (!attachment->is_mapped)
				continue;

			kcd_mem_dmaunmap_attachment(mem, attachment);
			ret = update_gpuvm_pte(mem, attachment, &sync);
			if (ret) {
				pr_err("%s: update PTE failed\n", __func__);
				/* make sure this gets validated again */
				mutex_lock(&process_info->notifier_lock);
				mem->invalid++;
				mutex_unlock(&process_info->notifier_lock);
				goto unreserve_out;
			}
		}
	}

	/* Update page directories */
	ret = process_update_pds(process_info, &sync);

unreserve_out:
	list_for_each_entry(peer_vm, &process_info->vm_list_head,
			    vm_list_node)
		loonggpu_bo_fence(peer_vm->root.base.bo,
				&process_info->eviction_fence->base, true);
	ttm_eu_backoff_reservation(&ticket, &resv_list);
	loonggpu_sync_wait(&sync, false);
	loonggpu_sync_free(&sync);
out:
	kfree(pd_bo_list_entries);

	return ret;
}

/* Confirm that all user pages are valid while holding the notifier lock
 *
 * Moves valid BOs from the userptr_inval_list back to userptr_val_list.
 */
static int confirm_valid_user_pages_locked(struct lgkcd_process_info *process_info)
{
	struct kgd_mem *mem, *tmp_mem;
	int ret = 0;

	list_for_each_entry_safe(mem, tmp_mem,
				 &process_info->userptr_inval_list,
				 validate_list.head) {
		if (mem->invalid) {
			WARN(1, "Valid BO is marked invalid");
			ret = -EAGAIN;
			continue;
		}
	}

	return ret;
}

/* Worker callback to restore evicted userptr BOs
 *
 * Tries to update and validate all userptr BOs. If successful and no
 * concurrent evictions happened, the queues are restarted. Otherwise,
 * reschedule for another attempt later.
 */
static void loonggpu_lgkcd_restore_userptr_worker(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct lgkcd_process_info *process_info =
		container_of(dwork, struct lgkcd_process_info,
			     restore_userptr_work);
	struct task_struct *usertask;
	struct mm_struct *mm;
	uint32_t evicted_bos;

	mutex_lock(&process_info->notifier_lock);
	evicted_bos = process_info->evicted_bos;
	mutex_unlock(&process_info->notifier_lock);
	if (!evicted_bos)
		return;

	/* Reference task and mm in case of concurrent process termination */
	usertask = get_pid_task(process_info->pid, PIDTYPE_PID);
	if (!usertask)
		return;
	mm = get_task_mm(usertask);
	if (!mm) {
		put_task_struct(usertask);
		return;
	}

	mutex_lock(&process_info->lock);

	if (update_invalid_user_pages(process_info, mm))
		goto unlock_out;
	/* userptr_inval_list can be empty if all evicted userptr BOs
	 * have been freed. In that case there is nothing to validate
	 * and we can just restart the queues.
	 */
	if (!list_empty(&process_info->userptr_inval_list)) {
		if (validate_invalid_user_pages(process_info))
			goto unlock_out;
	}
	/* Final check for concurrent evicton and atomic update. If
	 * another eviction happens after successful update, it will
	 * be a first eviction that calls quiesce_mm. The eviction
	 * reference counting inside KCD will handle this case.
	 */
	mutex_lock(&process_info->notifier_lock);

	if (confirm_valid_user_pages_locked(process_info)) {
		WARN(1, "User pages unexpectedly invalid");
		goto unlock_notifier_out;
	}

	if (process_info->evicted_bos != evicted_bos)
		goto unlock_notifier_out;
	process_info->evicted_bos = evicted_bos = 0;

	if (kgd2kcd_resume_mm(mm)) {
		pr_err("%s: Failed to resume KCD\n", __func__);
		/* No recovery from this failure. Probably the CP is
		 * hanging. No point trying again.
		 */
	}

unlock_notifier_out:
	mutex_unlock(&process_info->notifier_lock);
unlock_out:
	mutex_unlock(&process_info->lock);

	/* If validation failed, reschedule another attempt */
	if (evicted_bos) {
		schedule_delayed_work(&process_info->restore_userptr_work,
			msecs_to_jiffies(LOONGGPU_USERPTR_RESTORE_DELAY_MS));
		/* TODO */
		// kcd_smi_event_queue_restore_rescheduled(mm);
	}
	mmput(mm);
	put_task_struct(usertask);
}

/** loonggpu_lgkcd_gpuvm_restore_process_bos - Restore all BOs for the given
 *   KCD process identified by process_info
 *
 * @process_info: lgkcd_process_info of the KCD process
 *
 * After memory eviction, restore thread calls this function. The function
 * should be called when the Process is still valid. BO restore involves -
 *
 * 1.  Release old eviction fence and create new one
 * 2.  Get two copies of PD BO list from all the VMs. Keep one copy as pd_list.
 * 3   Use the second PD list and kcd_bo_list to create a list (ctx.list) of
 *     BOs that need to be reserved.
 * 4.  Reserve all the BOs
 * 5.  Validate of PD and PT BOs.
 * 6.  Validate all KCD BOs using kcd_bo_list and Map them and add new fence
 * 7.  Add fence to all PD and PT BOs.
 * 8.  Unreserve all BOs
 */
int loonggpu_lgkcd_gpuvm_restore_process_bos(void *info, struct dma_fence **ef)
{
	struct loonggpu_bo_list_entry *pd_bo_list;
	struct lgkcd_process_info *process_info = info;
	struct loonggpu_vm *peer_vm;
	struct kgd_mem *mem;
	struct bo_vm_reservation_context ctx;
	struct loonggpu_lgkcd_fence *new_fence;
	struct list_head duplicate_save;
	struct loonggpu_sync sync_obj;
	unsigned long failed_size = 0;
	unsigned long total_size = 0;
	int ret = 0, i;

	INIT_LIST_HEAD(&duplicate_save);
	INIT_LIST_HEAD(&ctx.list);
	INIT_LIST_HEAD(&ctx.duplicates);

	pd_bo_list = kcalloc(process_info->n_vms,
			     sizeof(struct loonggpu_bo_list_entry),
			     GFP_KERNEL);
	if (!pd_bo_list)
		return -ENOMEM;

	i = 0;
	mutex_lock(&process_info->lock);

	list_for_each_entry(peer_vm, &process_info->vm_list_head,
			vm_list_node)
		loonggpu_vm_get_pd_bo(peer_vm, &ctx.list, &pd_bo_list[i++]);

	/* Reserve all BOs and page tables/directory. Add all BOs from
	 * kcd_bo_list to ctx.list
	 */
	list_for_each_entry(mem, &process_info->kcd_bo_list,
			    validate_list.head) {

		list_add_tail(&mem->resv_list.head, &ctx.list);
		mem->resv_list.bo = mem->validate_list.bo;
		lg_ttm_validate_buffer_copy_shared(&mem->resv_list, &mem->validate_list);
	}

	ret = lg_ttm_eu_reserve_buffers(&ctx.ticket, &ctx.list,
				     false, &duplicate_save, false);
	if (ret) {
		pr_debug("Memory eviction: TTM Reserve Failed. Try again\n");
		goto ttm_reserve_fail;
	}

	loonggpu_sync_create(&sync_obj);

	/* Validate PDs and PTs */
	ret = process_validate_vms(process_info);
	if (ret)
		goto validate_map_fail;

	ret = process_sync_pds_resv(process_info, &sync_obj);
	if (ret) {
		pr_debug("Memory eviction: Failed to sync to PD BO moving fence. Try again\n");
		goto validate_map_fail;
	}

	/* Validate BOs and map them to GPUVM (update VM page tables). */
	list_for_each_entry(mem, &process_info->kcd_bo_list,
			    validate_list.head) {

		struct loonggpu_bo *bo = mem->bo;
		uint32_t domain = mem->domain;
		struct kcd_mem_attachment *attachment;

		total_size += loonggpu_bo_size(bo);

		ret = loonggpu_lgkcd_bo_validate(bo, domain, false);
		if (ret) {
			pr_debug("Memory eviction: Validate BOs failed\n");
			failed_size += loonggpu_bo_size(bo);
			ret = loonggpu_lgkcd_bo_validate(bo,
						LOONGGPU_GEM_DOMAIN_GTT, false);
			if (ret) {
				pr_debug("Memory eviction: Try again\n");
				goto validate_map_fail;
			}
		}

		list_for_each_entry(attachment, &mem->attachments, list) {
			if (!attachment->is_mapped)
				continue;

			if (attachment->bo_va->base.bo->tbo.pin_count)
				continue;

			kcd_mem_dmaunmap_attachment(mem, attachment);
			ret = update_gpuvm_pte(mem, attachment, &sync_obj);
			if (ret) {
				pr_debug("Memory eviction: update PTE failed. Try again\n");
				goto validate_map_fail;
			}
		}
	}

	if (failed_size)
		pr_debug("0x%lx/0x%lx in system\n", failed_size, total_size);

	/* Update page directories */
	ret = process_update_pds(process_info, &sync_obj);
	if (ret) {
		pr_debug("Memory eviction: update PDs failed. Try again\n");
		goto validate_map_fail;
	}

	/* Wait for validate and PT updates to finish */
	loonggpu_sync_wait(&sync_obj, false);

	/* Release old eviction fence and create new one, because fence only
	 * goes from unsignaled to signaled, fence cannot be reused.
	 * Use context and mm from the old fence.
	 */
	new_fence = loonggpu_lgkcd_fence_create(
				process_info->eviction_fence->base.context,
				process_info->eviction_fence->mm,
				NULL);
	if (!new_fence) {
		pr_err("Failed to create eviction fence\n");
		ret = -ENOMEM;
		goto validate_map_fail;
	}
	dma_fence_put(&process_info->eviction_fence->base);
	process_info->eviction_fence = new_fence;
	*ef = dma_fence_get(&new_fence->base);

	/* Wait for validate to finish and attach new eviction fence */
	list_for_each_entry(mem, &process_info->kcd_bo_list,
		validate_list.head)
		lg_ttm_bo_wait(&mem->bo->tbo, false, false);

	list_for_each_entry(mem, &process_info->kcd_bo_list,
		validate_list.head)
		loonggpu_bo_fence(mem->bo,
			&process_info->eviction_fence->base, true);

	/* Attach eviction fence to PD / PT BOs */
	list_for_each_entry(peer_vm, &process_info->vm_list_head,
			    vm_list_node) {
		struct loonggpu_bo *bo = peer_vm->root.base.bo;

		loonggpu_bo_fence(bo, &process_info->eviction_fence->base, true);
	}

validate_map_fail:
	ttm_eu_backoff_reservation(&ctx.ticket, &ctx.list);
	loonggpu_sync_free(&sync_obj);
ttm_reserve_fail:
	mutex_unlock(&process_info->lock);
	kfree(pd_bo_list);
	return ret;
}

int loonggpu_lgkcd_gpuvm_fault(struct loonggpu_device *adev, u32 pasid,
			    u32 vmid, u32 node_id, uint64_t addr,
			    u32 fault_domain)
{
	if (loonggpu_noretry)
		return 0;
	
	return loonggpu_vm_handle_fault(adev, pasid, vmid, node_id,
			addr, fault_domain);
}

#if defined(CONFIG_DEBUG_FS)

int kcd_debugfs_kcd_mem_limits(struct seq_file *m, void *data)
{

	spin_lock(&kcd_mem_limit.mem_limit_lock);
	seq_printf(m, "System mem used %lldM out of %lluM\n",
		  (kcd_mem_limit.system_mem_used >> 20),
		  (kcd_mem_limit.max_system_mem_limit >> 20));
	seq_printf(m, "TTM mem used %lldM out of %lluM\n",
		  (kcd_mem_limit.ttm_mem_used >> 20),
		  (kcd_mem_limit.max_ttm_mem_limit >> 20));
	spin_unlock(&kcd_mem_limit.mem_limit_lock);

	return 0;
}

#endif
