#include <linux/list.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include <asm/dma.h>
#include <asm-generic/bug.h>
#include <drm/drm_cache.h>
#include "loonggpu.h"
#include "loonggpu_drm.h"
#include "loonggpu_trace.h"
#include "loonggpu_lgkcd.h"
#include "loonggpu_helper.h"
#include "loonggpu_gtt_mgr_helper.h"
#include "loonggpu_bo_pin_helper.h"
#include "loonggpu_sync_helper.h"
#include "loonggpu_dma_resv_helper.h"
#include "loonggpu_dc_vbios.h"

/**
 * DOC: loonggpu_object
 *
 * This defines the interfaces to operate on an &loonggpu_bo buffer object which
 * represents memory used by driver (VRAM, system memory, etc.). The driver
 * provides DRM/GEM APIs to userspace. DRM/GEM APIs then use these interfaces
 * to create/destroy/set buffer object which are then managed by the kernel TTM
 * memory manager.
 * The interfaces are also used internally by kernel clients, including gfx,
 * uvd, etc. for kernel managed allocations used by the GPU.
 *
 */

static bool loonggpu_bo_need_backup(struct loonggpu_device *adev)
{
	if (adev->flags & LOONGGPU_IS_APU)
		return false;

	if (loonggpu_gpu_recovery == 0 || loonggpu_gpu_recovery == -1)
		return false;

	return true;
}

/**
 * loonggpu_bo_subtract_pin_size - Remove BO from pin_size accounting
 *
 * @bo: &loonggpu_bo buffer object
 *
 * This function is called when a BO stops being pinned, and updates the
 * &loonggpu_device pin_size values accordingly.
 */
static void loonggpu_bo_subtract_pin_size(struct loonggpu_bo *bo)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);

	if (lg_get_bo_mem_type(bo) == TTM_PL_VRAM) {
		atomic64_sub(loonggpu_bo_size(bo), &adev->vram_pin_size);
		atomic64_sub(loonggpu_vram_mgr_bo_visible_size(bo),
			     &adev->visible_pin_size);
	} else if (lg_get_bo_mem_type(bo) == TTM_PL_TT) {
		atomic64_sub(loonggpu_bo_size(bo), &adev->gart_pin_size);
	}
}

static void loonggpu_bo_destroy(struct ttm_buffer_object *tbo)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(tbo->bdev);
	struct loonggpu_bo *bo = ttm_to_loonggpu_bo(tbo);

	if (lg_bo_pin_count(bo) > 0)
		loonggpu_bo_subtract_pin_size(bo);

	if (bo->kcd_bo)
		loonggpu_lgkcd_release_notify(bo);

	loonggpu_bo_kunmap(bo);

	if (lg_gbo_to_gem_obj(bo).import_attach)
		drm_prime_gem_destroy(&lg_gbo_to_gem_obj(bo), bo->tbo.sg);
	drm_gem_object_release(&lg_gbo_to_gem_obj(bo));
	loonggpu_bo_unref(&bo->parent);
	if (!list_empty(&bo->shadow_list)) {
		mutex_lock(&adev->shadow_list_lock);
		list_del_init(&bo->shadow_list);
		mutex_unlock(&adev->shadow_list_lock);
	}
	kfree(bo->metadata);
	kfree(bo);
}

/**
 * loonggpu_bo_is_loonggpu_bo - check if the buffer object is an &loonggpu_bo
 * @bo: buffer object to be checked
 *
 * Uses destroy function associated with the object to determine if this is
 * an &loonggpu_bo.
 *
 * Returns:
 * true if the object belongs to &loonggpu_bo, false if not.
 */
bool loonggpu_bo_is_loonggpu_bo(struct ttm_buffer_object *bo)
{
	if (bo->destroy == &loonggpu_bo_destroy)
		return true;
	return false;
}

/**
 * loonggpu_bo_placement_from_domain - set buffer's placement
 * @abo: &loonggpu_bo buffer object whose placement is to be set
 * @domain: requested domain
 *
 * Sets buffer's placement according to requested domain and the buffer's
 * flags.
 */
void loonggpu_bo_placement_from_domain(struct loonggpu_bo *abo, u32 domain)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(abo->tbo.bdev);
	struct ttm_placement *placement = &abo->placement;
	struct ttm_place *places = abo->placements;
	u64 flags = abo->flags;
	u32 c = 0;

	if (flags & LOONGGPU_GEM_CREATE_COMPRESSED_MASK &&
	    domain & LOONGGPU_GEM_DOMAIN_VRAM) {
		abo->flags |= LOONGGPU_GEM_CREATE_PREEMPTIBLE |
			      LOONGGPU_GEM_CREATE_VRAM_CONTIGUOUS;
		flags = abo->flags;
	}

	if (domain & LOONGGPU_GEM_DOMAIN_VRAM) {
		unsigned visible_pfn = adev->gmc.visible_vram_size >> PAGE_SHIFT;

		places[c].fpfn = 0;
		places[c].lpfn = 0;
	#if defined(TTM_PL_FLAG_VRAM)
		if (loonggpu_using_ram)
			places[c].flags = TTM_PL_FLAG_CACHED |
				TTM_PL_FLAG_VRAM;
		else
			places[c].flags = TTM_PL_FLAG_UNCACHED |
				TTM_PL_FLAG_VRAM |
				(adev->wc_support ? TTM_PL_FLAG_WC : 0);
	#else
	#if defined(TTM_PL_FLAG_UNCACHED)
		if (loonggpu_using_ram)
			places[c].flags = TTM_PL_FLAG_CACHED;
		else
			places[c].flags = TTM_PL_FLAG_UNCACHED |
					  (adev->wc_support ?
					   TTM_PL_FLAG_WC : 0);
	#endif
		places[c].mem_type = TTM_PL_VRAM;
	#endif

	#if defined(TTM_PL_FLAG_UNCACHED)
		if (flags & LOONGGPU_GEM_CREATE_CPU_GTT_USWC)
			places[c].flags |= adev->wc_support ? TTM_PL_FLAG_WC
					   : TTM_PL_FLAG_UNCACHED;
	#endif

		if (flags & LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED)
			places[c].lpfn = visible_pfn;
		else
			places[c].flags |= TTM_PL_FLAG_TOPDOWN;

		if (flags & LOONGGPU_GEM_CREATE_VRAM_CONTIGUOUS || adev->family_type == CHIP_NO_GPU)
			places[c].flags |= TTM_PL_FLAG_CONTIGUOUS;
		c++;
	}

	if (domain & LOONGGPU_GEM_DOMAIN_DOORBELL) {
		places[c].fpfn = 0;
		places[c].lpfn = 0;
#if defined(TTM_PL_FLAG_PRIV)
		places[c].flags = LOONGGPU_PL_FLAG_DOORBELL | TTM_PL_FLAG_UNCACHED;
#else
		places[c].flags = 0;
		places[c].mem_type = LOONGGPU_PL_DOORBELL;
#endif
		c++;
	}

	if (domain & LOONGGPU_GEM_DOMAIN_GTT) {
		places[c].fpfn = 0;
		if (flags & LOONGGPU_GEM_CREATE_SHADOW)
			places[c].lpfn = adev->gmc.gart_size >> PAGE_SHIFT;
		else
			places[c].lpfn = 0;
	#if defined(TTM_PL_FLAG_TT)
		places[c].flags = TTM_PL_FLAG_TT;
	#else
		places[c].flags = 0;
		places[c].mem_type = TTM_PL_TT;
	#endif
	#if defined(TTM_PL_FLAG_UNCACHED)
		if (flags & LOONGGPU_GEM_CREATE_CPU_GTT_USWC)
			places[c].flags |= (adev->wc_support ? TTM_PL_FLAG_WC :
					0) | TTM_PL_FLAG_UNCACHED;
		else
			places[c].flags |= TTM_PL_FLAG_CACHED;
	#endif
		c++;
	}

	if (domain & LOONGGPU_GEM_DOMAIN_CPU) {
		places[c].fpfn = 0;
		places[c].lpfn = 0;
	#if defined(TTM_PL_FLAG_SYSTEM)
		places[c].flags = TTM_PL_FLAG_SYSTEM;
	#else
		places[c].mem_type = TTM_PL_SYSTEM;
		places[c].flags = 0;
	#endif
	#if defined(TTM_PL_FLAG_UNCACHED)
		if (flags & LOONGGPU_GEM_CREATE_CPU_GTT_USWC)
			places[c].flags |= (adev->wc_support ? TTM_PL_FLAG_WC :
					0) | TTM_PL_FLAG_UNCACHED;
		else
			places[c].flags |= TTM_PL_FLAG_CACHED;
	#endif
		c++;
	}

	if (!c) {
		places[c].fpfn = 0;
		places[c].lpfn = 0;
	#if defined(TTM_PL_FLAG_SYSTEM)
		places[c].flags = TTM_PL_MASK_CACHING | TTM_PL_FLAG_SYSTEM;
	#else
	#if defined(TTM_PL_MASK_CACHING)
		places[c].flags = TTM_PL_MASK_CACHING;
	#endif
		places[c].mem_type = TTM_PL_SYSTEM;
	#endif
		c++;
	}

	BUG_ON(c >= LOONGGPU_BO_MAX_PLACEMENTS);

	placement->num_placement = c;
	placement->placement = places;
}

/**
 * loonggpu_bo_create_reserved - create reserved BO for kernel use
 *
 * @adev: loonggpu device object
 * @size: size for the new BO
 * @align: alignment for the new BO
 * @domain: where to place it
 * @bo_ptr: used to initialize BOs in structures
 * @gpu_addr: GPU addr of the pinned BO
 * @cpu_addr: optional CPU address mapping
 *
 * Allocates and pins a BO for kernel internal use, and returns it still
 * reserved.
 *
 * Note: For bo_ptr new BO is only created if bo_ptr points to NULL.
 *
 * Returns:
 * 0 on success, negative error code otherwise.
 */
int loonggpu_bo_create_reserved(struct loonggpu_device *adev,
			      unsigned long size, int align,
			      u32 domain, struct loonggpu_bo **bo_ptr,
			      u64 *gpu_addr, void **cpu_addr)
{
	struct loonggpu_bo_param bp;
	bool free = false;
	int r;

	memset(&bp, 0, sizeof(bp));
	bp.size = size;
	bp.byte_align = align;
	bp.domain = domain;
	bp.flags = LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED |
		LOONGGPU_GEM_CREATE_VRAM_CONTIGUOUS;
	bp.type = ttm_bo_type_kernel;
	bp.resv = NULL;

	if (!*bo_ptr) {
		r = loonggpu_bo_create(adev, &bp, bo_ptr);
		if (r) {
			dev_err(adev->dev, "(%d) failed to allocate kernel bo\n",
				r);
			return r;
		}
		free = true;
	}

	r = loonggpu_bo_reserve(*bo_ptr, false);
	if (r) {
		dev_err(adev->dev, "(%d) failed to reserve kernel bo\n", r);
		goto error_free;
	}

	r = loonggpu_bo_pin(*bo_ptr, domain);
	if (r) {
		dev_err(adev->dev, "(%d) kernel bo pin failed\n", r);
		goto error_unreserve;
	}

	r = loonggpu_ttm_alloc_gart(&(*bo_ptr)->tbo);
	if (r) {
		dev_err(adev->dev, "%p bind failed\n", *bo_ptr);
		goto error_unpin;
	}

	if (gpu_addr)
		*gpu_addr = loonggpu_bo_gpu_offset(*bo_ptr);

	if (cpu_addr) {
		r = loonggpu_bo_kmap(*bo_ptr, cpu_addr);
		if (r) {
			dev_err(adev->dev, "(%d) kernel bo map failed\n", r);
			goto error_unpin;
		}
	}

	return 0;

error_unpin:
	loonggpu_bo_unpin(*bo_ptr);
error_unreserve:
	loonggpu_bo_unreserve(*bo_ptr);

error_free:
	if (free)
		loonggpu_bo_unref(bo_ptr);

	return r;
}

/**
 * loonggpu_bo_create_kernel - create BO for kernel use
 *
 * @adev: loonggpu device object
 * @size: size for the new BO
 * @align: alignment for the new BO
 * @domain: where to place it
 * @bo_ptr:  used to initialize BOs in structures
 * @gpu_addr: GPU addr of the pinned BO
 * @cpu_addr: optional CPU address mapping
 *
 * Allocates and pins a BO for kernel internal use.
 *
 * Note: For bo_ptr new BO is only created if bo_ptr points to NULL.
 *
 * Returns:
 * 0 on success, negative error code otherwise.
 */
int loonggpu_bo_create_kernel(struct loonggpu_device *adev,
			    unsigned long size, int align,
			    u32 domain, struct loonggpu_bo **bo_ptr,
			    u64 *gpu_addr, void **cpu_addr)
{
	int r;

	r = loonggpu_bo_create_reserved(adev, size, align, domain, bo_ptr,
				      gpu_addr, cpu_addr);

	if (r)
		return r;

	loonggpu_bo_unreserve(*bo_ptr);

	return 0;
}

/**
 * loonggpu_bo_free_kernel - free BO for kernel use
 *
 * @bo: loonggpu BO to free
 * @gpu_addr: pointer to where the BO's GPU memory space address was stored
 * @cpu_addr: pointer to where the BO's CPU memory space address was stored
 *
 * unmaps and unpin a BO for kernel internal use.
 */
void loonggpu_bo_free_kernel(struct loonggpu_bo **bo, u64 *gpu_addr,
			   void **cpu_addr)
{
	if (*bo == NULL)
		return;

	if (likely(loonggpu_bo_reserve(*bo, true) == 0)) {
		if (cpu_addr)
			loonggpu_bo_kunmap(*bo);

		loonggpu_bo_unpin(*bo);
		loonggpu_bo_unreserve(*bo);
	}
	loonggpu_bo_unref(bo);

	if (gpu_addr)
		*gpu_addr = 0;

	if (cpu_addr)
		*cpu_addr = NULL;
}

/* Validate bo size is bit bigger then the request domain */
static bool loonggpu_bo_validate_size(struct loonggpu_device *adev,
					  unsigned long size, u32 domain)
{
	lg_ttm_manager_t *man = NULL;

	/*
	 * If GTT is part of requested domains the check must succeed to
	 * allow fall back to GTT
	 */
	if (domain & LOONGGPU_GEM_DOMAIN_GTT) {
		man = lg_bdev_to_ttm_man(&adev->mman.bdev, TTM_PL_TT);

		if (size < lg_get_man_size(man))
			return true;
		else
			goto fail;
	}

	if (domain & LOONGGPU_GEM_DOMAIN_VRAM) {
		man = lg_bdev_to_ttm_man(&adev->mman.bdev, TTM_PL_VRAM);
		if (size < lg_get_man_size(man))
			return true;
		else
			goto fail;
	}


	/* TODO add more domains checks, such as LOONGGPU_GEM_DOMAIN_CPU */
	return true;

fail:
	DRM_DEBUG("BO size %lu > total memory in domain: %llu\n", size,
		  man->size << PAGE_SHIFT);
	return false;
}

static int loonggpu_bo_do_create(struct loonggpu_device *adev,
			       struct loonggpu_bo_param *bp,
			       struct loonggpu_bo **bo_ptr)
{
	struct ttm_operation_ctx ctx = {
		.interruptible = (bp->type != ttm_bo_type_kernel),
		.no_wait_gpu = false,
		.resv = bp->resv,
		.allow_res_evict = (bp->type != ttm_bo_type_kernel),
	};
	struct loonggpu_bo *bo;
	lg_dma_resv_t *resv;
	unsigned long page_align, size = bp->size;
	size_t acc_size;
	int r;

	page_align = roundup(bp->byte_align, PAGE_SIZE) >> PAGE_SHIFT;
	size = ALIGN(size, PAGE_SIZE);

	if (!loonggpu_bo_validate_size(adev, size, bp->domain))
		return -ENOMEM;

	acc_size = 0;
	*bo_ptr = NULL;

	bo = kzalloc(sizeof(struct loonggpu_bo), GFP_KERNEL);
	if (bo == NULL)
		return -ENOMEM;
	drm_gem_private_object_init(adev->ddev, &lg_gbo_to_gem_obj(bo), size);
	INIT_LIST_HEAD(&bo->shadow_list);
	INIT_LIST_HEAD(&bo->va);
	bo->preferred_domains = bp->preferred_domain ? bp->preferred_domain :
		bp->domain;
	bo->allowed_domains = bo->preferred_domains;
	if (bp->type != ttm_bo_type_kernel &&
	    !(bp->flags & LOONGGPU_GEM_CREATE_DISCARDABLE) &&
	    bo->allowed_domains == LOONGGPU_GEM_DOMAIN_VRAM)
		bo->allowed_domains |= LOONGGPU_GEM_DOMAIN_GTT;

	bo->flags = bp->flags;

	if (!drm_arch_can_wc_memory())
		bo->flags &= ~LOONGGPU_GEM_CREATE_CPU_GTT_USWC;

	bo->tbo.bdev = &adev->mman.bdev;
	loonggpu_bo_placement_from_domain(bo, bp->domain);
	if (bp->type == ttm_bo_type_kernel)
		bo->tbo.priority = 1;

	r = ttm_bo_init_reserved(&adev->mman.bdev, &bo->tbo, bp->type,
				&bo->placement, page_align, &ctx, NULL, bp->resv, &loonggpu_bo_destroy);
	if (unlikely(r != 0))
		return r;

	if (!loonggpu_gmc_vram_full_visible(&adev->gmc) &&
	    lg_get_bo_mem_type(bo) == TTM_PL_VRAM &&
	    lg_tbo_to_mem(&bo->tbo)->start < adev->gmc.visible_vram_size >> PAGE_SHIFT)
		loonggpu_cs_report_moved_bytes(adev, ctx.bytes_moved,
					     ctx.bytes_moved);
	else
		loonggpu_cs_report_moved_bytes(adev, ctx.bytes_moved, 0);

	if (bp->flags & LOONGGPU_GEM_CREATE_VRAM_CLEARED && adev->family_type != CHIP_NO_GPU &&
	#if defined(TTM_PL_FLAG_VRAM)
		bo->tbo.mem.placement & TTM_PL_FLAG_VRAM
	#else
		lg_get_bo_mem_type(bo) == TTM_PL_VRAM
	#endif
	) {

		struct dma_fence *fence;

		resv = to_dma_resv(bo);
		r = lg_fill_buffer(bo, 0, resv, &fence, true);
		if (unlikely(r))
			goto fail_unreserve;

		loonggpu_bo_fence(bo, fence, false);
		dma_fence_put(fence);
	}

	if (!bp->resv)
		loonggpu_bo_unreserve(bo);
	*bo_ptr = bo;

	trace_loonggpu_bo_create(bo);

	/* Treat CPU_ACCESS_REQUIRED only as a hint if given by UMD */
	if (bp->type == ttm_bo_type_device)
		bo->flags &= ~LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;

	return 0;

fail_unreserve:
	if (!bp->resv)
		ww_mutex_unlock(&resv->lock);
	loonggpu_bo_unref(&bo);
	return r;
}

static int loonggpu_bo_create_shadow(struct loonggpu_device *adev,
				   unsigned long size, int byte_align,
				   struct loonggpu_bo *bo)
{
	struct loonggpu_bo_param bp;
	int r;

	if (bo->shadow)
		return 0;

	memset(&bp, 0, sizeof(bp));
	bp.size = size;
	bp.byte_align = byte_align;
	bp.domain = LOONGGPU_GEM_DOMAIN_GTT;
	bp.flags = LOONGGPU_GEM_CREATE_CPU_GTT_USWC |
		LOONGGPU_GEM_CREATE_SHADOW;
	bp.type = ttm_bo_type_kernel;
	bp.resv = to_dma_resv(bo);

	r = loonggpu_bo_do_create(adev, &bp, &bo->shadow);
	if (!r) {
		bo->shadow->parent = loonggpu_bo_ref(bo);
		mutex_lock(&adev->shadow_list_lock);
		list_add_tail(&bo->shadow_list, &adev->shadow_list);
		mutex_unlock(&adev->shadow_list_lock);
	}

	return r;
}

/**
 * loonggpu_bo_create - create an &loonggpu_bo buffer object
 * @adev: loonggpu device object
 * @bp: parameters to be used for the buffer object
 * @bo_ptr: pointer to the buffer object pointer
 *
 * Creates an &loonggpu_bo buffer object; and if requested, also creates a
 * shadow object.
 * Shadow object is used to backup the original buffer object, and is always
 * in GTT.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_create(struct loonggpu_device *adev,
		     struct loonggpu_bo_param *bp,
		     struct loonggpu_bo **bo_ptr)
{
	u64 flags = bp->flags;
	int r;

	bp->flags = bp->flags & ~LOONGGPU_GEM_CREATE_SHADOW;
	r = loonggpu_bo_do_create(adev, bp, bo_ptr);
	if (r)
		return r;

	if ((flags & LOONGGPU_GEM_CREATE_SHADOW) && loonggpu_bo_need_backup(adev)) {
		lg_dma_resv_t *resv = to_dma_resv(*bo_ptr);

		if (!bp->resv)
			WARN_ON(lg_dma_resv_lock(resv, NULL));

		r = loonggpu_bo_create_shadow(adev, bp->size, bp->byte_align, (*bo_ptr));

		if (!bp->resv)
			lg_dma_resv_unlock(resv);

		if (r)
			loonggpu_bo_unref(bo_ptr);
	}

	return r;
}

/**
 * loonggpu_bo_backup_to_shadow - Backs up an &loonggpu_bo buffer object
 * @adev: loonggpu device object
 * @ring: loonggpu_ring for the engine handling the buffer operations
 * @bo: &loonggpu_bo buffer to be backed up
 * @resv: reservation object with embedded fence
 * @fence: dma_fence associated with the operation
 * @direct: whether to submit the job directly
 *
 * Copies an &loonggpu_bo buffer object to its shadow object.
 * Not used for now.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_backup_to_shadow(struct loonggpu_device *adev,
			       struct loonggpu_ring *ring,
			       struct loonggpu_bo *bo,
			       lg_dma_resv_t *resv,
			       struct dma_fence **fence,
			       bool direct)

{
	struct loonggpu_bo *shadow = bo->shadow;
	uint64_t bo_addr, shadow_addr;
	int r;

	if (!shadow)
		return -EINVAL;

	bo_addr = loonggpu_bo_gpu_offset(bo);
	shadow_addr = loonggpu_bo_gpu_offset(bo->shadow);

	r = lg_dma_resv_reserve_shared(to_dma_resv(bo), 1);
	if (r)
		goto err;

	r = loonggpu_copy_buffer(ring, bo_addr, shadow_addr,
			       loonggpu_bo_size(bo), resv, fence,
			       direct, false);
	if (!r)
		loonggpu_bo_fence(bo, *fence, true);

err:
	return r;
}

/**
 * loonggpu_bo_validate - validate an &loonggpu_bo buffer object
 * @bo: pointer to the buffer object
 *
 * Sets placement according to domain; and changes placement and caching
 * policy of the buffer object according to the placement.
 * This is used for validating shadow bos.  It calls ttm_bo_validate() to
 * make sure the buffer is resident where it needs to be.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_validate(struct loonggpu_bo *bo)
{
	struct ttm_operation_ctx ctx = { false, false };
	uint32_t domain;
	int r;

	if (lg_bo_pin_count(bo))
		return 0;

	domain = bo->preferred_domains;

retry:
	loonggpu_bo_placement_from_domain(bo, domain);
	r = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
	if (unlikely(r == -ENOMEM) && domain != bo->allowed_domains) {
		domain = bo->allowed_domains;
		goto retry;
	}

	return r;
}

/**
 * loonggpu_bo_restore_from_shadow - restore an &loonggpu_bo buffer object
 * @adev: loonggpu device object
 * @ring: loonggpu_ring for the engine handling the buffer operations
 * @bo: &loonggpu_bo buffer to be restored
 * @resv: reservation object with embedded fence
 * @fence: dma_fence associated with the operation
 * @direct: whether to submit the job directly
 *
 * Copies a buffer object's shadow content back to the object.
 * This is used for recovering a buffer from its shadow in case of a gpu
 * reset where vram context may be lost.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_restore_from_shadow(struct loonggpu_device *adev,
				  struct loonggpu_ring *ring,
				  struct loonggpu_bo *bo,
				  lg_dma_resv_t *resv,
				  struct dma_fence **fence,
				  bool direct)

{
	struct loonggpu_bo *shadow = bo->shadow;
	uint64_t bo_addr, shadow_addr;
	int r;

	if (!shadow)
		return -EINVAL;

	bo_addr = loonggpu_bo_gpu_offset(bo);
	shadow_addr = loonggpu_bo_gpu_offset(bo->shadow);

	r = lg_dma_resv_reserve_shared(to_dma_resv(bo), 1);
	if (r)
		goto err;

	r = loonggpu_copy_buffer(ring, shadow_addr, bo_addr,
			       loonggpu_bo_size(bo), resv, fence,
			       direct, false);
	if (!r)
		loonggpu_bo_fence(bo, *fence, true);

err:
	return r;
}

/**
 * loonggpu_bo_kmap - map an &loonggpu_bo buffer object
 * @bo: &loonggpu_bo buffer object to be mapped
 * @ptr: kernel virtual address to be returned
 *
 * Calls ttm_bo_kmap() to set up the kernel virtual mapping; calls
 * loonggpu_bo_kptr() to get the kernel virtual address.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_kmap(struct loonggpu_bo *bo, void **ptr)
{
	lg_dma_resv_t *resv = to_dma_resv(bo);
	void *kptr;
	long r;

	if (bo->flags & LOONGGPU_GEM_CREATE_NO_CPU_ACCESS)
		return -EPERM;

	kptr = loonggpu_bo_kptr(bo);
	if (kptr) {
		if (ptr)
			*ptr = kptr;
		return 0;
	}

	r = lg_dma_resv_wait_timeout_rcu(resv, LG_DMA_RESV_USAGE_WRITE, false, false, MAX_SCHEDULE_TIMEOUT);
	if (r < 0)
		return r;

	r = ttm_bo_kmap(&bo->tbo, 0, lg_tbo_to_num_pages(&bo->tbo), &bo->kmap);
	if (r)
		return r;

	if (ptr)
		*ptr = loonggpu_bo_kptr(bo);

	return 0;
}

/**
 * loonggpu_bo_kptr - returns a kernel virtual address of the buffer object
 * @bo: &loonggpu_bo buffer object
 *
 * Calls ttm_kmap_obj_virtual() to get the kernel virtual address
 *
 * Returns:
 * the virtual address of a buffer object area.
 */
void *loonggpu_bo_kptr(struct loonggpu_bo *bo)
{
	bool is_iomem;

	return ttm_kmap_obj_virtual(&bo->kmap, &is_iomem);
}

/**
 * loonggpu_bo_kunmap - unmap an &loonggpu_bo buffer object
 * @bo: &loonggpu_bo buffer object to be unmapped
 *
 * Unmaps a kernel map set up by loonggpu_bo_kmap().
 */
void loonggpu_bo_kunmap(struct loonggpu_bo *bo)
{
	if (bo->kmap.bo)
		ttm_bo_kunmap(&bo->kmap);
}

/**
 * loonggpu_bo_ref - reference an &loonggpu_bo buffer object
 * @bo: &loonggpu_bo buffer object
 *
 * References the contained &ttm_buffer_object.
 *
 * Returns:
 * a refcounted pointer to the &loonggpu_bo buffer object.
 */
struct loonggpu_bo *loonggpu_bo_ref(struct loonggpu_bo *bo)
{
	if (bo == NULL)
		return NULL;

	kref_get(&bo->tbo.kref);
	return bo;
}

/**
 * loonggpu_bo_unref - unreference an &loonggpu_bo buffer object
 * @bo: &loonggpu_bo buffer object
 *
 * Unreferences the contained &ttm_buffer_object and clear the pointer
 */
void loonggpu_bo_unref(struct loonggpu_bo **bo)
{
	struct ttm_buffer_object *tbo;

	if ((*bo) == NULL)
		return;

	tbo = &((*bo)->tbo);
	ttm_bo_fini(tbo);
	*bo = NULL;
}

/**
 * loonggpu_bo_pin_restricted - pin an &loonggpu_bo buffer object
 * @bo: &loonggpu_bo buffer object to be pinned
 * @domain: domain to be pinned to
 * @min_offset: the start of requested address range
 * @max_offset: the end of requested address range
 *
 * Pins the buffer object according to requested domain and address range. If
 * the memory is unbound gart memory, binds the pages into gart table. Adjusts
 * pin_count and pin_size accordingly.
 *
 * Pinning means to lock pages in memory along with keeping them at a fixed
 * offset. It is required when a buffer can not be moved, for example, when
 * a display buffer is being scanned out.
 *
 * Compared with loonggpu_bo_pin(), this function gives more flexibility on
 * where to pin a buffer if there are specific restrictions on where a buffer
 * must be located.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_pin_restricted(struct loonggpu_bo *bo, u32 domain,
			     u64 min_offset, u64 max_offset)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);
	struct ttm_operation_ctx ctx = { false, false };
	int r, i;

	if (loonggpu_ttm_tt_get_usermm(bo->tbo.ttm))
		return -EPERM;

	if (WARN_ON_ONCE(min_offset > max_offset))
		return -EINVAL;

	/* A shared bo cannot be migrated to VRAM */
	if (bo->prime_shared_count) {
		if (domain & LOONGGPU_GEM_DOMAIN_GTT)
			domain = LOONGGPU_GEM_DOMAIN_GTT;
		else
			return -EINVAL;
	}

	/* This assumes only APU display buffers are pinned with (VRAM|GTT).
	 * See function loonggpu_display_supported_domains()
	 */
	domain = loonggpu_bo_get_preferred_pin_domain(adev, domain);

	if (lg_bo_pin_count(bo)) {
		uint32_t mem_type = lg_get_bo_mem_type(bo);

		if (!(domain & loonggpu_mem_type_to_domain(mem_type)))
			return -EINVAL;

		lg_ttm_bo_pin(bo);

		if (max_offset != 0) {
			u64 domain_start = lg_bdev_to_gpu_offset(bo->tbo.bdev, mem_type);
			WARN_ON_ONCE(max_offset <
				     (loonggpu_bo_gpu_offset(bo) - domain_start));
		}

		return 0;
	}

	bo->flags |= LOONGGPU_GEM_CREATE_VRAM_CONTIGUOUS;
	/* force to pin into visible video ram */
	if (!(bo->flags & LOONGGPU_GEM_CREATE_NO_CPU_ACCESS))
		bo->flags |= LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
	loonggpu_bo_placement_from_domain(bo, domain);
	for (i = 0; i < bo->placement.num_placement; i++) {
		unsigned fpfn, lpfn;

		fpfn = min_offset >> PAGE_SHIFT;
		lpfn = max_offset >> PAGE_SHIFT;

		if (fpfn > bo->placements[i].fpfn)
			bo->placements[i].fpfn = fpfn;
		if (!bo->placements[i].lpfn ||
		    (lpfn && lpfn < bo->placements[i].lpfn))
			bo->placements[i].lpfn = lpfn;
	}

	r = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
	if (unlikely(r)) {
		dev_err(adev->dev, "%p pin failed\n", bo);
		goto error;
	}

	lg_bo_pin(bo);

	domain = loonggpu_mem_type_to_domain(lg_get_bo_mem_type(bo));
	if (domain == LOONGGPU_GEM_DOMAIN_VRAM) {
		atomic64_add(loonggpu_bo_size(bo), &adev->vram_pin_size);
		atomic64_add(loonggpu_vram_mgr_bo_visible_size(bo),
			     &adev->visible_pin_size);
	} else if (domain == LOONGGPU_GEM_DOMAIN_GTT) {
		atomic64_add(loonggpu_bo_size(bo), &adev->gart_pin_size);
	}

error:
	return r;
}

/**
 * loonggpu_bo_pin - pin an &loonggpu_bo buffer object
 * @bo: &loonggpu_bo buffer object to be pinned
 * @domain: domain to be pinned to
 *
 * A simple wrapper to loonggpu_bo_pin_restricted().
 * Provides a simpler API for buffers that do not have any strict restrictions
 * on where a buffer must be located.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_pin(struct loonggpu_bo *bo, u32 domain)
{
	return loonggpu_bo_pin_restricted(bo, domain, 0, 0);
}

/**
 * loonggpu_bo_unpin - unpin an &loonggpu_bo buffer object
 * @bo: &loonggpu_bo buffer object to be unpinned
 *
 * Decreases the pin_count, and clears the flags if pin_count reaches 0.
 * Changes placement and pin size accordingly.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_unpin(struct loonggpu_bo *bo)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);
	struct ttm_operation_ctx ctx = { false, false };
	int r, i;

	if (!lg_bo_pin_count(bo)) {
		dev_warn(adev->dev, "%p unpin not necessary\n", bo);
		return 0;
	}

	lg_ttm_bo_unpin(bo);
	if (lg_bo_pin_count(bo))
		return 0;

	loonggpu_bo_subtract_pin_size(bo);

	for (i = 0; i < bo->placement.num_placement; i++) {
		bo->placements[i].lpfn = 0;
	}
	r = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
	if (unlikely(r))
		dev_err(adev->dev, "%p validate failed for unpin\n", bo);

	return r;
}

/**
 * loonggpu_bo_evict_vram - evict VRAM buffers
 * @adev: loonggpu device object
 *
 * Evicts all VRAM buffers on the lru list of the memory type.
 * Mainly used for evicting vram at suspend time.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_evict_vram(struct loonggpu_device *adev)
{
	/* late 2.6.33 fix IGP hibernate - we need pm ops to do this correct */
	if (0 && (adev->flags & LOONGGPU_IS_APU)) {
		/* Useless to evict on IGP chips */
		return 0;
	}
	return lg_ttm_bo_evict_mm(&adev->mman.bdev, TTM_PL_VRAM);
}

/**
 * loonggpu_bo_init - initialize memory manager
 * @adev: loonggpu device object
 *
 * Calls loonggpu_ttm_init() to initialize loonggpu memory manager.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_init(struct loonggpu_device *adev)
{
	/* reserve PAT memory space to WC for VRAM */
	arch_io_reserve_memtype_wc(adev->gmc.aper_base,
				   adev->gmc.aper_size);

	/* Add an MTRR for the VRAM */
	adev->gmc.vram_mtrr = arch_phys_wc_add(adev->gmc.aper_base,
					      adev->gmc.aper_size);
	DRM_DEBUG("Detected VRAM RAM=%lluM, BAR=%lluM\n",
		 adev->gmc.mc_vram_size >> 20,
		 (unsigned long long)adev->gmc.aper_size >> 20);
	DRM_DEBUG("RAM width %dbits %s\n", adev->gmc.vram_width,
		 loonggpu_vram_type_names[adev->gmc.vram_type]);
	return loonggpu_ttm_init(adev);
}

/**
 * loonggpu_bo_late_init - late init
 * @adev: loonggpu device object
 *
 * Calls loonggpu_ttm_late_init() to free resources used earlier during
 * initialization.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_late_init(struct loonggpu_device *adev)
{
	loonggpu_ttm_late_init(adev);

	return 0;
}

/**
 * loonggpu_bo_fini - tear down memory manager
 * @adev: loonggpu device object
 *
 * Reverses loonggpu_bo_init() to tear down memory manager.
 */
void loonggpu_bo_fini(struct loonggpu_device *adev)
{
	loonggpu_ttm_fini(adev);
	arch_phys_wc_del(adev->gmc.vram_mtrr);
	arch_io_free_memtype_wc(adev->gmc.aper_base, adev->gmc.aper_size);
}

/**
 * loonggpu_bo_fbdev_mmap - mmap fbdev memory
 * @bo: &loonggpu_bo buffer object
 * @vma: vma as input from the fbdev mmap method
 *
 * Calls ttm_fbdev_mmap() to mmap fbdev memory if it is backed by a bo.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_fbdev_mmap(struct loonggpu_bo *bo,
			     struct vm_area_struct *vma)
{
	return lg_ttm_fbdev_mmap(vma, &bo->tbo);
}

/**
 * loonggpu_bo_set_tiling_flags - set tiling flags
 * @bo: &loonggpu_bo buffer object
 * @tiling_flags: new flags
 *
 * Sets buffer object's tiling flags with the new one. Used by GEM ioctl or
 * kernel driver to set the tiling flags on a buffer.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_set_tiling_flags(struct loonggpu_bo *bo, u64 tiling_flags)
{
	if (LOONGGPU_TILING_GET(tiling_flags, TILE_SPLIT) > 6)
		return -EINVAL;

	bo->tiling_flags = tiling_flags;
	return 0;
}

/**
 * loonggpu_bo_get_tiling_flags - get tiling flags
 * @bo: &loonggpu_bo buffer object
 * @tiling_flags: returned flags
 *
 * Gets buffer object's tiling flags. Used by GEM ioctl or kernel driver to
 * set the tiling flags on a buffer.
 */
void loonggpu_bo_get_tiling_flags(struct loonggpu_bo *bo, u64 *tiling_flags)
{
	lg_dma_resv_t *resv = to_dma_resv(bo);
	lockdep_assert_held(&resv->lock.base);

	if (tiling_flags)
		*tiling_flags = bo->tiling_flags;
}

/**
 * loonggpu_bo_set_metadata - set metadata
 * @bo: &loonggpu_bo buffer object
 * @metadata: new metadata
 * @metadata_size: size of the new metadata
 * @flags: flags of the new metadata
 *
 * Sets buffer object's metadata, its size and flags.
 * Used via GEM ioctl.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_set_metadata (struct loonggpu_bo *bo, void *metadata,
			    uint32_t metadata_size, uint64_t flags)
{
	void *buffer;

	if (!metadata_size) {
		if (bo->metadata_size) {
			kfree(bo->metadata);
			bo->metadata = NULL;
			bo->metadata_size = 0;
		}
		return 0;
	}

	if (metadata == NULL)
		return -EINVAL;

	buffer = kmemdup(metadata, metadata_size, GFP_KERNEL);
	if (buffer == NULL)
		return -ENOMEM;

	kfree(bo->metadata);
	bo->metadata_flags = flags;
	bo->metadata = buffer;
	bo->metadata_size = metadata_size;

	return 0;
}

/**
 * loonggpu_bo_get_metadata - get metadata
 * @bo: &loonggpu_bo buffer object
 * @buffer: returned metadata
 * @buffer_size: size of the buffer
 * @metadata_size: size of the returned metadata
 * @flags: flags of the returned metadata
 *
 * Gets buffer object's metadata, its size and flags. buffer_size shall not be
 * less than metadata_size.
 * Used via GEM ioctl.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_get_metadata(struct loonggpu_bo *bo, void *buffer,
			   size_t buffer_size, uint32_t *metadata_size,
			   uint64_t *flags)
{
	if (!buffer && !metadata_size)
		return -EINVAL;

	if (buffer) {
		if (buffer_size < bo->metadata_size)
			return -EINVAL;

		if (bo->metadata_size)
			memcpy(buffer, bo->metadata, bo->metadata_size);
	}

	if (metadata_size)
		*metadata_size = bo->metadata_size;
	if (flags)
		*flags = bo->metadata_flags;

	return 0;
}

/**
 * loonggpu_bo_move_notify - notification about a memory move
 * @bo: pointer to a buffer object
 * @evict: if this move is evicting the buffer from the graphics address space
 * @new_mem: new information of the bufer object
 *
 * Marks the corresponding &loonggpu_bo buffer object as invalid, also performs
 * bookkeeping.
 * TTM driver callback which is called when ttm moves a buffer.
 */
void loonggpu_bo_move_notify(struct ttm_buffer_object *bo,
			   bool evict,
			   lg_ttm_mem_t *new_mem)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->bdev);
	struct loonggpu_bo *abo;
	lg_ttm_mem_t *old_mem = lg_tbo_to_mem(bo);

	if (!loonggpu_bo_is_loonggpu_bo(bo))
		return;

	abo = ttm_to_loonggpu_bo(bo);
	loonggpu_vm_bo_invalidate(adev, abo, evict);

	loonggpu_bo_kunmap(abo);

	/* remember the eviction */
	if (evict)
		atomic64_inc(&adev->num_evictions);

	/* update statistics */
	if (!new_mem)
		return;

	/* move_notify is called before move happens */
	trace_loonggpu_bo_move(abo, new_mem->mem_type, old_mem->mem_type);
}

/**
 * loonggpu_bo_fault_reserve_notify - notification about a memory fault
 * @bo: pointer to a buffer object
 *
 * Notifies the driver we are taking a fault on this BO and have reserved it,
 * also performs bookkeeping.
 * TTM driver callback for dealing with vm faults.
 *
 * Returns:
 * 0 for success or a negative error code on failure.
 */
int loonggpu_bo_fault_reserve_notify(struct ttm_buffer_object *bo)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->bdev);
	struct ttm_operation_ctx ctx = { false, false };
	struct loonggpu_bo *abo;
	unsigned long offset, size;
	int r;

	if (!loonggpu_bo_is_loonggpu_bo(bo))
		return 0;

	abo = ttm_to_loonggpu_bo(bo);

	/* Remember that this BO was accessed by the CPU */
	abo->flags |= LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;

	if (lg_tbo_to_mem(bo)->mem_type != TTM_PL_VRAM)
		return 0;

	size = lg_get_tbo_res_size(bo);
	offset = lg_tbo_to_mem(bo)->start << PAGE_SHIFT;
	if ((offset + size) <= adev->gmc.visible_vram_size)
		return 0;

	/* Can't move a pinned BO to visible VRAM */
	if (abo->tbo.pin_count > 0)
		return -EINVAL;

	/* hurrah the memory is not visible ! */
	atomic64_inc(&adev->num_vram_cpu_page_faults);
	loonggpu_bo_placement_from_domain(abo, LOONGGPU_GEM_DOMAIN_VRAM |
					LOONGGPU_GEM_DOMAIN_GTT);

	/* Avoid costly evictions; only set GTT as a busy placement */
	r = ttm_bo_validate(bo, &abo->placement, &ctx);
	if (unlikely(r != 0))
		return r;

	offset = lg_tbo_to_mem(bo)->start << PAGE_SHIFT;
	/* this should never happen */
	if (lg_tbo_to_mem(bo)->mem_type == TTM_PL_VRAM &&
	    (offset + size) > adev->gmc.visible_vram_size)
		return -EINVAL;

	return 0;
}

/**
 * loonggpu_bo_fence - add fence to buffer object
 *
 * @bo: buffer object in question
 * @fence: fence to add
 * @shared: true if fence should be added shared
 *
 */
void loonggpu_bo_fence(struct loonggpu_bo *bo, struct dma_fence *fence,
		     bool shared)
{
	lg_dma_resv_t *resv = to_dma_resv(bo);

	if (shared)
		lg_dma_resv_add_shared_fence(resv, fence);
	else
		lg_dma_resv_add_excl_fence(resv, fence);
}

/**
 * loonggpu_bo_sync_wait_resv - Wait for BO reservation fences
 *
 * @adev: loonggpu device pointer
 * @resv: reservation object to sync to
 * @sync_mode: synchronization mode
 * @owner: fence owner
 * @intr: Whether the wait is interruptible
 *
 * Extract the fences from the reservation object and waits for them to finish.
 *
 * Returns:
 * 0 on success, errno otherwise.
 */
static int loonggpu_bo_sync_wait_resv(struct loonggpu_device *adev, lg_dma_resv_t *resv,
			     enum loonggpu_sync_mode sync_mode, void *owner,
			     bool intr, bool explicit)
{
	struct loonggpu_sync sync;
	int r;

	loonggpu_sync_create(&sync);
	lg_loonggpu_sync_resv(adev, &sync, resv, sync_mode, owner, explicit);
	r = loonggpu_sync_wait(&sync, intr);
	loonggpu_sync_free(&sync);

	return r;
}

/**
 * loonggpu_bo_sync_wait - Wrapper for loonggpu_bo_sync_wait_resv
 * @bo: buffer object to wait for
 * @owner: fence owner
 * @intr: Whether the wait is interruptible
 *
 * Wrapper to wait for fences in a BO.
 * Returns:
 * 0 on success, errno otherwise.
 */
int loonggpu_bo_sync_wait(struct loonggpu_bo *bo, enum loonggpu_sync_mode sync_mode,
			void *owner, bool intr, bool explicit)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);

	return loonggpu_bo_sync_wait_resv(adev, lg_tbo_to_resv(&bo->tbo),
					sync_mode, owner, intr, explicit);
}

/**
 * loonggpu_bo_gpu_offset - return GPU offset of bo
 * @bo:	loonggpu object for which we query the offset
 *
 * Note: object should either be pinned or reserved when calling this
 * function, it might be useful to add check for this for debugging.
 *
 * Returns:
 * current GPU offset of the object.
 */
u64 loonggpu_bo_gpu_offset(struct loonggpu_bo *bo)
{
	lg_dma_resv_t *resv = to_dma_resv(bo);
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);

	WARN_ON_ONCE(lg_tbo_to_mem(&bo->tbo)->mem_type == TTM_PL_SYSTEM);
	WARN_ON_ONCE(lg_tbo_to_mem(&bo->tbo)->mem_type == TTM_PL_TT &&
		     !loonggpu_gtt_mgr_has_gart_addr(lg_tbo_to_mem(&bo->tbo)));
	WARN_ON_ONCE(!ww_mutex_is_locked(&resv->lock) && !lg_bo_pin_count(bo) &&
			!(bo->flags & LOONGGPU_GEM_CREATE_COMPRESSED_MASK));
	WARN_ON_ONCE(lg_tbo_to_mem(&bo->tbo)->start == LOONGGPU_BO_INVALID_OFFSET);
	WARN_ON_ONCE(lg_tbo_to_mem(&bo->tbo)->mem_type == TTM_PL_VRAM &&
		     !(bo->flags & LOONGGPU_GEM_CREATE_VRAM_CONTIGUOUS));

	return lg_loonggpu_ttm_alloc_gart_get_bo_offset(adev, &bo->tbo);
}

/**
 * loonggpu_bo_get_preferred_pin_domain - get preferred domain for scanout
 * @adev: loonggpu device object
 * @domain: allowed :ref:`memory domains <loonggpu_memory_domains>`
 *
 * Returns:
 * Which of the allowed domains is preferred for pinning the BO for scanout.
 */
uint32_t loonggpu_bo_get_preferred_pin_domain(struct loonggpu_device *adev,
					    uint32_t domain)
{
	if (domain == (LOONGGPU_GEM_DOMAIN_VRAM | LOONGGPU_GEM_DOMAIN_GTT)) {
		domain = LOONGGPU_GEM_DOMAIN_VRAM;
		if (adev->gmc.real_vram_size <= LOONGGPU_SG_THRESHOLD)
			domain = LOONGGPU_GEM_DOMAIN_GTT;
	}
	return domain;
}
