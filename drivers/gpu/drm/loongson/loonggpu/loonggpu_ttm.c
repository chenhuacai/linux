#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/hmm.h>
#include <linux/pagemap.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/swiotlb.h>
#include <linux/dma-buf.h>
#include <linux/sizes.h>
#include <linux/debugfs.h>

#include <drm/ttm/ttm_placement.h>
#include <drm/drm_cache.h>
#include <drm/drm_debugfs.h>
#include "loonggpu.h"
#include "loonggpu_drm.h"
#include "loonggpu_object.h"
#include "loonggpu_trace.h"
#include "loonggpu_lgkcd.h"

#include "loonggpu_helper.h"
#include "loonggpu_ttm_helper.h"
#include "loonggpu_gtt_mgr_helper.h"
#include "loonggpu_vram_mgr_helper.h"
#include "loonggpu_bo_pin_helper.h"
#include <drm/ttm/ttm_bo.h>

/**TODO
 *Need fixed this, mmap ops had access handler.
**/
#define mmMM_INDEX         0x0
#define mmMM_INDEX_HI      0x6
#define mmMM_DATA          0x1

static int loonggpu_map_buffer(struct ttm_buffer_object *bo,
			     lg_ttm_mem_t *mem, struct loonggpu_res_cursor *src_mm, unsigned num_pages,
			     uint64_t offset, unsigned window,
			     struct loonggpu_ring *ring,
			     uint64_t *addr);

static int loonggpu_ttm_debugfs_init(struct loonggpu_device *adev);
static void loonggpu_ttm_debugfs_fini(struct loonggpu_device *adev);

/**
 * loonggpu_evict_flags - Compute placement flags
 *
 * @bo: The buffer object to evict
 * @placement: Possible destination(s) for evicted BO
 *
 * Fill in placement data when ttm_bo_evict() is called
 */
static void loonggpu_evict_flags(struct ttm_buffer_object *bo,
				struct ttm_placement *placement)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->bdev);
	struct loonggpu_bo *abo;
	static const struct ttm_place placements = {
		.fpfn = 0,
		.lpfn = 0,
	#if defined(TTM_PL_FLAG_SYSTEM)
	#if defined(TTM_PL_MASK_CACHING)
		.flags = TTM_PL_MASK_CACHING | TTM_PL_FLAG_SYSTEM
	#else
		.flags = TTM_PL_FLAG_SYSTEM
	#endif
	#else
		.mem_type = TTM_PL_SYSTEM,
	#if defined(TTM_PL_MASK_CACHING)
		.flags = TTM_PL_MASK_CACHING
	#endif
	#endif
	};

	/* Don't handle scatter gather BOs */
	if (bo->type == ttm_bo_type_sg) {
		placement->num_placement = 0;
		return;
	}

	/* Object isn't an LOONGGPU object so ignore */
	if (!loonggpu_bo_is_loonggpu_bo(bo)) {
		placement->placement = &placements;
		placement->num_placement = 1;
		return;
	}

	abo = ttm_to_loonggpu_bo(bo);
	if (abo->flags & LOONGGPU_GEM_CREATE_DISCARDABLE) {
		placement->num_placement = 0;
		return;
	}

	switch (lg_tbo_to_mem(bo)->mem_type) {
	case LOONGGPU_PL_DOORBELL:
		placement->num_placement = 0;
		return;
	case TTM_PL_VRAM:
		if (!adev->mman.buffer_funcs_enabled) {
			/* Move to system memory */
			loonggpu_bo_placement_from_domain(abo, LOONGGPU_GEM_DOMAIN_CPU);
		} else if (!loonggpu_gmc_vram_full_visible(&adev->gmc) &&
			   !(abo->flags & LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED) &&
			   loonggpu_bo_in_cpu_visible_vram(abo)) {

			/* Try evicting to the CPU inaccessible part of VRAM
			 * first, but only set GTT as busy placement, so this
			 * BO will be evicted to GTT rather than causing other
			 * BOs to be evicted from VRAM
			 */
			loonggpu_bo_placement_from_domain(abo, LOONGGPU_GEM_DOMAIN_VRAM |
							 LOONGGPU_GEM_DOMAIN_GTT);
			abo->placements[0].fpfn = adev->gmc.visible_vram_size >> PAGE_SHIFT;
			abo->placements[0].lpfn = 0;
		} else {
			/* Move to GTT memory */
			loonggpu_bo_placement_from_domain(abo, LOONGGPU_GEM_DOMAIN_GTT);
		}
		break;
	case TTM_PL_TT:
	default:
		loonggpu_bo_placement_from_domain(abo, LOONGGPU_GEM_DOMAIN_CPU);
	}
	*placement = abo->placement;
}

/**
 * loonggpu_verify_access - Verify access for a mmap call
 *
 * @bo:	The buffer object to map
 * @filp: The file pointer from the process performing the mmap
 *
 * This is called by ttm_bo_mmap() to verify whether a process
 * has the right to mmap a BO to their process space.
 */
static int loonggpu_verify_access(struct ttm_buffer_object *bo, struct file *filp)
{
	struct loonggpu_bo *abo = ttm_to_loonggpu_bo(bo);

	/*
	 * Don't verify access for KCD BOs. They don't have a GEM
	 * object associated with them.
	 */
	if (abo->kcd_bo)
		return 0;

	if (loonggpu_ttm_tt_get_usermm(bo->ttm))
		return -EPERM;
	return drm_vma_node_verify_access(&(lg_gbo_to_gem_obj(abo).vma_node),
					  filp->private_data);
}

/**
 * loonggpu_move_null - Register memory for a buffer object
 *
 * @bo: The bo to assign the memory to
 * @new_mem: The memory to be assigned.
 *
 * Assign the memory from new_mem to the memory of the buffer object bo.
 */
static void loonggpu_move_null(struct ttm_buffer_object *bo,
			     lg_ttm_mem_t *new_mem)
{
	ttm_bo_move_null(bo, new_mem);
}

/**
 * loonggpu_mm_node_addr - Compute the GPU relative offset of a GTT buffer.
 *
 * @bo: The bo to assign the memory to.
 * @mm_node: Memory manager node for drm allocator.
 * @mem: The region where the bo resides.
 *
 */
static uint64_t loonggpu_mm_node_addr(struct ttm_buffer_object *bo,
				    struct drm_mm_node *mm_node,
				    lg_ttm_mem_t *mem)
{
	uint64_t addr = 0;

	if (mem->mem_type != TTM_PL_TT || loonggpu_gtt_mgr_has_gart_addr(mem)) {
		addr = mm_node->start << PAGE_SHIFT;
		addr += lg_bdev_to_gpu_offset(bo->bdev, mem->mem_type);
	}
	return addr;
}

/**
 * loonggpu_find_mm_node - Helper function finds the drm_mm_node corresponding to
 * @offset. It also modifies the offset to be within the drm_mm_node returned
 *
 * @mem: The region where the bo resides.
 * @offset: The offset that drm_mm_node is used for finding.
 *
 */
static struct drm_mm_node *loonggpu_find_mm_node(lg_ttm_mem_t *mem,
					       unsigned long *offset)
{
	struct drm_mm_node *mm_node = lg_res_to_drm_node(mem);

	while (*offset >= (mm_node->size << PAGE_SHIFT)) {
		*offset -= (mm_node->size << PAGE_SHIFT);
		++mm_node;
	}
	return mm_node;
}

/**
 * loonggpu_copy_ttm_mem_to_mem - Helper function for copy
 *
 * The function copies @size bytes from {src->mem + src->offset} to
 * {dst->mem + dst->offset}. src->bo and dst->bo could be same BO for a
 * move and different for a BO to BO copy.
 *
 * @f: Returns the last fence if multiple jobs are submitted.
 */
int loonggpu_ttm_copy_mem_to_mem(struct loonggpu_device *adev,
			       struct loonggpu_copy_mem *src,
			       struct loonggpu_copy_mem *dst,
			       uint64_t size,
			       lg_dma_resv_t *resv,
			       struct dma_fence **f)
{
	struct loonggpu_ring *ring = adev->mman.buffer_funcs_ring;
	struct loonggpu_res_cursor src_mm, dst_mm;
	uint64_t src_node_start, dst_node_start, __maybe_unused dst_node_size,
		 __maybe_unused src_node_size, src_page_offset, dst_page_offset;
	struct dma_fence *fence = NULL;
	int r = 0;
	const uint64_t GTT_MAX_BYTES = (LOONGGPU_GTT_MAX_TRANSFER_SIZE *
					LOONGGPU_GPU_PAGE_SIZE);

	if (!adev->mman.buffer_funcs_enabled) {
		DRM_ERROR("Trying to move memory with ring turned off.\n");
		return -EINVAL;
	}

	loonggpu_res_first(src->mem, src->offset, size, &src_mm);
	loonggpu_res_first(dst->mem, dst->offset, size, &dst_mm);

	mutex_lock(&adev->mman.gtt_window_lock);

	while (src_mm.remaining) {
		unsigned long cur_size;
		uint64_t from = src_node_start, to = dst_node_start;
		struct dma_fence *next;

		/* Copy size cannot exceed GTT_MAX_BYTES. So if src or dst
		 * begins at an offset, then adjust the size accordingly
		 */
		cur_size = min3(src_mm.size, dst_mm.size, GTT_MAX_BYTES);

		/* Map src to window 0 and dst to window 1. */
                r = loonggpu_map_buffer(src->bo, src->mem, &src_mm,
                                        PFN_UP(cur_size + src_page_offset),
                                        src_node_start, 0, ring,
                                        &from);
                if (r)
                        goto error;

		r = loonggpu_map_buffer(dst->bo, dst->mem, &dst_mm,
                                        PFN_UP(cur_size + dst_page_offset),
                                        dst_node_start, 1, ring,
                                        &to);
                if (r)
                        goto error;

		r = loonggpu_copy_buffer(ring, from, to, cur_size,
				       resv, &next, false, true);
		if (r)
			goto error;

		dma_fence_put(fence);
		fence = next;

		loonggpu_res_next(&src_mm, cur_size);
		loonggpu_res_next(&dst_mm, cur_size);
	}
error:
	mutex_unlock(&adev->mman.gtt_window_lock);
	if (f)
		*f = dma_fence_get(fence);
	dma_fence_put(fence);
	return r;
}

/**
 * loonggpu_move_blit - Copy an entire buffer to another buffer
 *
 * This is a helper called by loonggpu_bo_move() and loonggpu_move_vram_ram() to
 * help move buffers to and from VRAM.
 */
static int loonggpu_move_blit(struct ttm_buffer_object *bo,
			    bool evict, bool no_wait_gpu,
			    lg_ttm_mem_t *new_mem,
			    lg_ttm_mem_t *old_mem)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->bdev);
	struct loonggpu_copy_mem src, dst;
	struct dma_fence *fence = NULL;
	int r;
	lg_dma_resv_t *resv = lg_tbo_to_resv(bo);

	src.bo = bo;
	dst.bo = bo;
	src.mem = old_mem;
	dst.mem = new_mem;
	src.offset = 0;
	dst.offset = 0;

	r = loonggpu_ttm_copy_mem_to_mem(adev, &src, &dst,
				      lg_mem_to_size(new_mem),
				      resv, &fence);
	if (r)
		goto error;

	r = lg_ttm_bo_pipeline_move(bo, fence, evict, new_mem);
	dma_fence_put(fence);
	return r;

error:
	if (fence)
		dma_fence_wait(fence, false);
	dma_fence_put(fence);
	return r;
}

/**
 * loonggpu_move_vram_ram - Copy VRAM buffer to RAM buffer
 *
 * Called by loonggpu_bo_move().
 */
static int loonggpu_move_vram_ram(struct ttm_buffer_object *bo, bool evict,
				struct ttm_operation_ctx *ctx,
				lg_ttm_mem_t *new_mem)
{
	struct loonggpu_device *adev;
	lg_ttm_mem_t *old_mem = lg_tbo_to_mem(bo);
	lg_ttm_mem_t tmp_mem, *p;
	struct ttm_place placements;
	struct ttm_placement placement;
	int r;

	adev = loonggpu_ttm_adev(bo->bdev);

	/* create space/pages for new_mem in GTT space */
	tmp_mem = *new_mem;
	p = &tmp_mem;
	lg_clear_drm_node(lg_res_to_drm_node(&tmp_mem));
	placement.num_placement = 1;
	placement.placement = &placements;
	placements.fpfn = 0;
	placements.lpfn = 0;
#if defined(TTM_PL_FLAG_TT)
#if defined(TTM_PL_MASK_CACHING)
	placements.flags = TTM_PL_MASK_CACHING | TTM_PL_FLAG_TT;
#else
	placements.flags = TTM_PL_FLAG_TT;
#endif
#else
#if defined(TTM_PL_MASK_CACHING)
	placements.flags = TTM_PL_MASK_CACHING;
#endif
	placements.mem_type = TTM_PL_TT;
#endif
	r = lg_ttm_bo_mem_space(bo, &placement, p, ctx);
	if (unlikely(r)) {
		return r;
	}

#if defined(TTM_PL_FLAG_WC)
	/* set caching flags */
	r = ttm_tt_set_placement_caching(bo->ttm, tmp_mem.placement);
	if (unlikely(r)) {
		goto out_cleanup;
	}
#endif
	/* Bind the memory to the GTT space */
	r = lg_ttm_tt_bind(bo, &tmp_mem, ctx);
	if (unlikely(r)) {
		goto out_cleanup;
	}

	/* blit VRAM to GTT */
	r = loonggpu_move_blit(bo, evict, ctx->no_wait_gpu, &tmp_mem, old_mem);
	if (unlikely(r)) {
		goto out_cleanup;
	}

	/* move BO (in tmp_mem) to new_mem */
	r = lg_ttm_bo_move_ttm(bo, ctx, new_mem);
out_cleanup:
	lg_ttm_bo_mem_put(bo, &tmp_mem);
	return r;
}

/**
 * loonggpu_move_ram_vram - Copy buffer from RAM to VRAM
 *
 * Called by loonggpu_bo_move().
 */
static int loonggpu_move_ram_vram(struct ttm_buffer_object *bo, bool evict,
				struct ttm_operation_ctx *ctx,
				lg_ttm_mem_t *new_mem)
{
	struct loonggpu_device *adev;
	lg_ttm_mem_t *old_mem = lg_tbo_to_mem(bo);
	lg_ttm_mem_t tmp_mem, *p;
	struct ttm_placement placement;
	struct ttm_place placements;
	int r;

	adev = loonggpu_ttm_adev(bo->bdev);

	/* make space in GTT for old_mem buffer */
	tmp_mem = *new_mem;
	p = &tmp_mem;
	lg_clear_drm_node(lg_res_to_drm_node(&tmp_mem));
	placement.num_placement = 1;
	placement.placement = &placements;
	placements.fpfn = 0;
	placements.lpfn = 0;
#if defined(TTM_PL_FLAG_TT)
#if defined(TTM_PL_MASK_CACHING)
	placements.flags = TTM_PL_MASK_CACHING | TTM_PL_FLAG_TT;
#else
	placements.flags = TTM_PL_FLAG_TT;
#endif
#else
	placements.mem_type = TTM_PL_TT;
#if defined(TTM_PL_MASK_CACHING)
	placements.flags = TTM_PL_MASK_CACHING;
#endif
#endif

	r = lg_ttm_bo_mem_space(bo, &placement, p, ctx);
	if (unlikely(r)) {
		return r;
	}

	/* move/bind old memory to GTT space */
	r = lg_ttm_bo_move_ttm(bo, ctx, &tmp_mem);
	if (unlikely(r)) {
		goto out_cleanup;
	}

	/* copy to VRAM */
	r = loonggpu_move_blit(bo, evict, ctx->no_wait_gpu, new_mem, old_mem);
	if (unlikely(r)) {
		goto out_cleanup;
	}
out_cleanup:
	lg_ttm_bo_mem_put(bo, &tmp_mem);
	return r;
}

/**
 * loonggpu_bo_move - Move a buffer object to a new memory location
 *
 * Called by ttm_bo_handle_move_mem()
 */
static int loonggpu_bo_move(struct ttm_buffer_object *bo, bool evict,
			  struct ttm_operation_ctx *ctx,
			  lg_ttm_mem_t *new_mem, struct ttm_place *hop)
{
	struct loonggpu_device *adev;
	struct loonggpu_bo *abo;
	lg_ttm_mem_t *old_mem = lg_tbo_to_mem(bo);
	int r;

	if (new_mem->mem_type == TTM_PL_TT) {
                r = loonggpu_ttm_backend_bind(
			bo->bdev, bo->ttm, new_mem);
                if (r)
                        return r;
        }

	/* Can't move a pinned BO */
	abo = ttm_to_loonggpu_bo(bo);
	if (WARN_ON_ONCE(lg_bo_pin_count(abo) > 0))
		return -EINVAL;
	adev = loonggpu_ttm_adev(bo->bdev);

	if (!old_mem || (old_mem->mem_type == TTM_PL_SYSTEM && bo->ttm == NULL)) {
		loonggpu_move_null(bo, new_mem);
		atomic64_add(lg_gbo_to_gem_obj(abo).size, &adev->num_bytes_moved);
		loonggpu_bo_move_notify(bo, evict, new_mem);
		return 0;
	}

	if (old_mem->mem_type == TTM_PL_SYSTEM && bo->ttm == NULL) {
		loonggpu_move_null(bo, new_mem);
		return 0;
	}

	if ((old_mem->mem_type == TTM_PL_TT &&
	     new_mem->mem_type == TTM_PL_SYSTEM) ||
	    (old_mem->mem_type == TTM_PL_SYSTEM &&
	     new_mem->mem_type == TTM_PL_TT)) {
		/* bind is enough */
		loonggpu_move_null(bo, new_mem);
		atomic64_add(lg_gbo_to_gem_obj(abo).size, &adev->num_bytes_moved);
		loonggpu_bo_move_notify(bo, evict, new_mem);
		return 0;
	}

	if (old_mem->mem_type == LOONGGPU_PL_DOORBELL ||
	    new_mem->mem_type == LOONGGPU_PL_DOORBELL) {
		/* Nothing to save here */
		loonggpu_move_null(bo, new_mem);
		return 0;
	}

	if (!adev->mman.buffer_funcs_enabled)
		goto memcpy;

	if ((old_mem->mem_type == TTM_PL_SYSTEM &&
	    new_mem->mem_type == TTM_PL_VRAM) ||
	    (old_mem->mem_type == TTM_PL_VRAM &&
	    new_mem->mem_type == TTM_PL_SYSTEM)) {
		hop->fpfn = 0;
		hop->lpfn = 0;
		hop->mem_type = TTM_PL_TT;
		hop->flags = TTM_PL_FLAG_TEMPORARY;
		return -EMULTIHOP;
	} else {
		r = loonggpu_move_blit(bo, evict, ctx->no_wait_gpu,
				     new_mem, old_mem);
	}

	if (r) {
memcpy:
		r = ttm_bo_move_memcpy(bo, ctx, new_mem);
		if (r) {
			return r;
		}
	}

	if (bo->type == ttm_bo_type_device &&
	    new_mem->mem_type == TTM_PL_VRAM &&
	    old_mem->mem_type != TTM_PL_VRAM) {
		/* loonggpu_bo_fault_reserve_notify will re-set this if the CPU
		 * accesses the BO after it's moved.
		 */
		abo->flags &= ~LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
	}

	/* update statistics */
	atomic64_add(lg_gbo_to_gem_obj(abo).size, &adev->num_bytes_moved);
	loonggpu_bo_move_notify(bo, evict, new_mem);
	return 0;
}

struct loonggpu_ttm_tt {
	struct ttm_tt		ttm;
	u64			offset;
	uint64_t		userptr;
	struct task_struct	*usertask;
	uint32_t		userflags;
	spinlock_t              guptasklock;
	struct list_head        guptasks;
	atomic_t		mmu_invalidations;
	uint32_t		last_set_pages;
	bool                    is_share_sg;
};

int loonggpu_ttm_tt_get_userptr(const struct ttm_buffer_object *tbo,
		uint64_t *user_addr)
{
	struct loonggpu_ttm_tt *gtt;

	if (!tbo->ttm)
		return -EINVAL;

	gtt = (void *)tbo->ttm;
	*user_addr = gtt->userptr;
	return 0;
}

/**
 * loonggpu_ttm_io_mem_reserve - Reserve a block of memory during a fault
 *
 * Called by ttm_mem_io_reserve() ultimately via ttm_bo_vm_fault()
 */
static int loonggpu_ttm_io_mem_reserve(lg_ttm_device_t *bdev, lg_ttm_mem_t *mem)
{
	lg_ttm_manager_t __maybe_unused *man = lg_bdev_to_ttm_man(bdev, mem->mem_type);
	struct loonggpu_device *adev = loonggpu_ttm_adev(bdev);

	struct loonggpu_res_cursor cur;
	loonggpu_res_first(mem, 0, mem->size, &cur);
	mem->bus.addr = NULL;
	mem->bus.offset = 0;
	mem->bus.is_iomem = false;
#if defined(TTM_MEMTYPE_FLAG_MAPPABLE)
	if (!(man->flags & TTM_MEMTYPE_FLAG_MAPPABLE))
		return -EINVAL;
#endif
	switch (mem->mem_type) {
	case TTM_PL_SYSTEM:
		/* system memory */
		return 0;
	case TTM_PL_TT:
		break;
	case TTM_PL_VRAM:
		mem->bus.offset = mem->start << PAGE_SHIFT;
		/* check if it's visible */
		if ((mem->bus.offset + ((size_t)lg_mem_to_size(mem))) > adev->gmc.visible_vram_size)
			return -EINVAL;
		/* Only physically contiguous buffers apply. In a contiguous
		 * buffer, size of the first mm_node would match the number of
		 * pages in ttm_mem_reg.
		 */
		if (adev->mman.aper_base_kaddr && cur.size == mem->size)
			mem->bus.addr = (u8 *)adev->mman.aper_base_kaddr +
					mem->bus.offset;

		lg_add_bus_placement_offset(mem, adev->gmc.aper_base);

		mem->bus.is_iomem = true;
		if (adev->wc_support)
			mem->bus.caching = ttm_write_combined;
		else
			mem->bus.caching = ttm_uncached;
		break;
	case LOONGGPU_PL_DOORBELL:
		mem->bus.offset = mem->start << PAGE_SHIFT;
		mem->bus.offset += adev->doorbell.base;
		mem->bus.is_iomem = true;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void loonggpu_ttm_io_mem_free(lg_ttm_device_t *bdev, lg_ttm_mem_t *mem)
{
}

static unsigned long loonggpu_ttm_io_mem_pfn(struct ttm_buffer_object *bo,
					   unsigned long page_offset)
{
	struct drm_mm_node *mm;
	unsigned long offset = (page_offset << PAGE_SHIFT);
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->bdev);

	if (bo->resource->mem_type == LOONGGPU_PL_DOORBELL) {
		struct loonggpu_res_cursor cursor;
		loonggpu_res_first(bo->resource, 0, 0, &cursor);
		return ((uint64_t)(adev->doorbell.base + cursor.start)) >> PAGE_SHIFT;
	}
	mm = loonggpu_find_mm_node(lg_tbo_to_mem(bo), &offset);
	return (lg_get_bus_placement_base(lg_tbo_to_mem(bo), adev) >> PAGE_SHIFT) + mm->start +
		(offset >> PAGE_SHIFT);
}

/*
 * TTM backend functions.
 */
struct loonggpu_ttm_gup_task_list {
	struct list_head	list;
	struct task_struct	*task;
};

static inline struct ttm_tt *lg_loonggputtm_to_ttm(struct loonggpu_ttm_tt *gtt)
{
	return &gtt->ttm;
}

/**
 * loonggpu_ttm_tt_get_user_pages - Pin pages of memory pointed to by a USERPTR
 * pointer to memory
 *
 * Called by loonggpu_gem_userptr_ioctl() and loonggpu_cs_parser_bos().
 * This provides a wrapper around the get_user_pages() call to provide
 * device accessible pages that back user memory.
 */
int loonggpu_ttm_tt_get_user_pages(struct ttm_tt *ttm, struct page **pages)
{
	struct loonggpu_ttm_tt *gtt = (void *)ttm;
	struct mm_struct *mm = gtt->usertask->mm;
	unsigned int flags = 0;
	unsigned pinned = 0;
	int r;

	if (!mm) /* Happens during process shutdown */
		return -ESRCH;

	if (!(gtt->userflags & LOONGGPU_GEM_USERPTR_READONLY))
		flags |= FOLL_WRITE;

	down_read(lg_mm_struct_get_sem(mm));

	if (gtt->userflags & LOONGGPU_GEM_USERPTR_ANONONLY) {
		/*
		 * check that we only use anonymous memory to prevent problems
		 * with writeback
		 */
		unsigned long end = gtt->userptr + ttm->num_pages * PAGE_SIZE;
		struct vm_area_struct *vma;

		vma = find_vma(mm, gtt->userptr);
		if (!vma || vma->vm_file || vma->vm_end < end) {
			up_read(lg_mm_struct_get_sem(mm));
			return -EPERM;
		}
	}

	/* loop enough times using contiguous pages of memory */
	do {
		unsigned num_pages = ttm->num_pages - pinned;
		uint64_t userptr = gtt->userptr + pinned * PAGE_SIZE;
		struct page **p = pages + pinned;
		struct loonggpu_ttm_gup_task_list guptask;

		guptask.task = current;
		spin_lock(&gtt->guptasklock);
		list_add(&guptask.list, &gtt->guptasks);
		spin_unlock(&gtt->guptasklock);

		if (mm == current->mm)
			r = lg_get_user_pages(userptr, num_pages, flags, p, NULL);
		else
			r = lg_get_user_pages_remote(gtt->usertask,
					mm, userptr, num_pages,
					flags, p, NULL, NULL);

		spin_lock(&gtt->guptasklock);
		list_del(&guptask.list);
		spin_unlock(&gtt->guptasklock);

		if (r < 0)
			goto release_pages;

		pinned += r;

	} while (pinned < ttm->num_pages);

	up_read(lg_mm_struct_get_sem(mm));
	return 0;

release_pages:
	release_pages(pages, pinned);
	up_read(lg_mm_struct_get_sem(mm));
	return r;
}

/**
 * loonggpu_ttm_tt_set_user_pages - Copy pages in, putting old pages as necessary.
 *
 * Called by loonggpu_cs_list_validate(). This creates the page list
 * that backs user memory and will ultimately be mapped into the device
 * address space.
 */
void loonggpu_ttm_tt_set_user_pages(struct ttm_tt *ttm, struct page **pages)
{
	struct loonggpu_ttm_tt *gtt = (void *)ttm;
	unsigned i;

	gtt->last_set_pages = atomic_read(&gtt->mmu_invalidations);
	for (i = 0; i < ttm->num_pages; ++i) {
		if (ttm->pages[i])
			put_page(ttm->pages[i]);

		ttm->pages[i] = pages ? pages[i] : NULL;
	}
}

/**
 * loonggpu_ttm_tt_mark_user_page - Mark pages as dirty
 *
 * Called while unpinning userptr pages
 */
void loonggpu_ttm_tt_mark_user_pages(struct ttm_tt *ttm)
{
	struct loonggpu_ttm_tt *gtt = (void *)ttm;
	unsigned i;

	for (i = 0; i < ttm->num_pages; ++i) {
		struct page *page = ttm->pages[i];

		if (!page)
			continue;

		if (!(gtt->userflags & LOONGGPU_GEM_USERPTR_READONLY))
			set_page_dirty(page);

		mark_page_accessed(page);
	}
}

/**
 * loonggpu_ttm_tt_pin_userptr - 	prepare the sg table with the user pages
 *
 * Called by loonggpu_ttm_backend_bind()
 **/
static int loonggpu_ttm_tt_pin_userptr(lg_ttm_device_t *bdev, struct ttm_tt *ttm)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bdev);
	struct loonggpu_ttm_tt *gtt = (void *)ttm;
	unsigned nents;
	int r;

	int write = !(gtt->userflags & LOONGGPU_GEM_USERPTR_READONLY);
	enum dma_data_direction direction = write ?
		DMA_BIDIRECTIONAL : DMA_TO_DEVICE;

	/* Allocate an SG array and squash pages into it */
	r = sg_alloc_table_from_pages(ttm->sg, ttm->pages, ttm->num_pages, 0,
				      ttm->num_pages << PAGE_SHIFT,
				      GFP_KERNEL);
	if (r)
		goto release_sg;

	/* Map SG to device */
	r = -ENOMEM;
	nents = dma_map_sg(adev->dev, ttm->sg->sgl, ttm->sg->nents, direction);
	if (nents != ttm->sg->nents)
		goto release_sg;

	/* convert SG to linear array of pages and dma addresses */
	lg_drm_prime_sg_to_addr_array;

	return 0;

release_sg:
	kfree(ttm->sg);
	return r;
}

/**
 * loonggpu_ttm_tt_unpin_userptr - Unpin and unmap userptr pages
 */
static void loonggpu_ttm_tt_unpin_userptr(lg_ttm_device_t *bdev, struct ttm_tt *ttm)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bdev);
	struct loonggpu_ttm_tt *gtt = (void *)ttm;

	int write = !(gtt->userflags & LOONGGPU_GEM_USERPTR_READONLY);
	enum dma_data_direction direction = write ?
		DMA_BIDIRECTIONAL : DMA_TO_DEVICE;

	/* double check that we don't free the table twice */
	if (!ttm->sg->sgl)
		return;

	/* unmap the pages mapped to the device */
	dma_unmap_sg(adev->dev, ttm->sg->sgl, ttm->sg->nents, direction);

	/* mark the pages as dirty */
	loonggpu_ttm_tt_mark_user_pages(ttm);

	sg_free_table(ttm->sg);
}

int loonggpu_ttm_gart_bind(struct loonggpu_device *adev,
				struct ttm_buffer_object *tbo,
				uint64_t flags)
{
	struct ttm_tt *ttm = tbo->ttm;
	struct loonggpu_ttm_tt *gtt = (void *)ttm;
	int r;

	r = loonggpu_gart_bind(adev, gtt->offset, ttm->num_pages,
			    ttm->pages, gtt->ttm.dma_address, flags);
	if (r)
		DRM_ERROR("failed to bind %lu pages at 0x%08llX\n",
			  (unsigned long)ttm->num_pages, gtt->offset);

	return r;
}

/**
 * loonggpu_ttm_backend_bind - Bind GTT memory
 *
 * Called by ttm_tt_bind() on behalf of ttm_bo_handle_move_mem().
 * This handles binding GTT memory to the device address space.
 */
int loonggpu_ttm_backend_bind(lg_ttm_backend_func_arg,
				   lg_ttm_mem_t *bo_mem)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(lg_ttm_backend_func_get_bdev);
	struct loonggpu_ttm_tt *gtt = (void *)ttm;
	uint64_t flags;
	int r = 0;

	if (gtt->userptr) {
		r = loonggpu_ttm_tt_pin_userptr(lg_ttm_backend_func_get_bdev, ttm);
		if (r) {
			DRM_ERROR("failed to pin userptr\n");
			return r;
		}
	}
	if (!ttm->num_pages) {
		WARN(1, "nothing to bind %lu pages for mreg %p back %p!\n",
		     (unsigned long)ttm->num_pages, bo_mem, ttm);
	}

	if (bo_mem->mem_type == LOONGGPU_PL_DOORBELL)
		return -EINVAL;

	if (!loonggpu_gtt_mgr_has_gart_addr(bo_mem)) {
		gtt->offset = LOONGGPU_BO_INVALID_OFFSET;
		return 0;
	}

	/* compute PTE flags relevant to this BO memory */
	flags = loonggpu_ttm_tt_pte_flags(adev, ttm, bo_mem);

	/* bind pages into GART page tables */
	gtt->offset = (u64)bo_mem->start << PAGE_SHIFT;
	r = loonggpu_gart_bind(adev, gtt->offset, ttm->num_pages,
		ttm->pages, gtt->ttm.dma_address, flags);

	if (r)
		DRM_ERROR("failed to bind %lu pages at 0x%08llX\n",
			  (unsigned long)ttm->num_pages, gtt->offset);
	return r;
}

/**
 * loonggpu_ttm_alloc_gart - Allocate GART memory for buffer object
 */
int loonggpu_ttm_alloc_gart(struct ttm_buffer_object *bo)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->bdev);
	struct ttm_operation_ctx ctx = { false, false };
	struct loonggpu_ttm_tt *gtt = (void *)bo->ttm;
	lg_ttm_mem_t *tmp;
	struct ttm_placement placement;
	struct ttm_place placements;
	uint64_t flags;
	int r;

	if (lg_tbo_to_mem(bo)->mem_type != TTM_PL_TT ||
	    loonggpu_gtt_mgr_has_gart_addr(lg_tbo_to_mem(bo)))
		return 0;

	/* allocate GTT space */
	tmp = lg_tbo_to_mem(bo);
	placement.num_placement = 1;
	placement.placement = &placements;
	placements.fpfn = 0;
	placements.lpfn = adev->gmc.gart_size >> PAGE_SHIFT;
	placements.flags = lg_tbo_to_mem(bo)->placement;
	placements.mem_type = TTM_PL_TT;

	r = ttm_bo_mem_space(bo, &placement, &tmp, &ctx);

	if (unlikely(r))
		return r;

	/* compute PTE flags for this buffer object */
	flags = loonggpu_ttm_tt_pte_flags(adev, bo->ttm, tmp);

	/* Bind pages */
	gtt->offset = (u64)tmp->start << PAGE_SHIFT;
	r = loonggpu_ttm_gart_bind(adev, bo, flags);
	if (unlikely(r)) {
		lg_ttm_bo_mem_put(bo, tmp);
		return r;
	}

	lg_ttm_bo_mem_put(bo, lg_tbo_to_mem(bo));
	lg_tbo_set_mem(bo, tmp);

	return 0;
}

/**
 * loonggpu_ttm_recover_gart - Rebind GTT pages
 *
 * Called by loonggpu_gtt_mgr_recover() from loonggpu_device_reset() to
 * rebind GTT pages during a GPU reset.
 */
int loonggpu_ttm_recover_gart(struct ttm_buffer_object *tbo)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(tbo->bdev);
	uint64_t flags;
	int r;

	if (!tbo->ttm)
		return 0;

	flags = loonggpu_ttm_tt_pte_flags(adev, tbo->ttm, lg_tbo_to_mem(tbo));
	r = loonggpu_ttm_gart_bind(adev, tbo, flags);

	return r;
}

/**
 * loonggpu_ttm_backend_unbind - Unbind GTT mapped pages
 *
 * Called by ttm_tt_unbind() on behalf of ttm_bo_move_ttm() and
 * ttm_tt_destroy().
 */
static lg_ttm_backend_unbind_ret loonggpu_ttm_backend_unbind(lg_ttm_backend_func_arg)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(lg_ttm_backend_func_get_bdev);
	struct loonggpu_ttm_tt *gtt = (void *)ttm;
	int r;

	/* if the pages have userptr pinning then clear that first */
	if (gtt->userptr)
		loonggpu_ttm_tt_unpin_userptr(lg_ttm_backend_func_get_bdev, ttm);

	if (gtt->offset == LOONGGPU_BO_INVALID_OFFSET)
		lg_ttm_backend_unbind_return(0);

	/* unbind shouldn't be done for GDS/GWS/OA in ttm_bo_clean_mm */
	r = loonggpu_gart_unbind(adev, gtt->offset, ttm->num_pages);
	if (r)
		DRM_ERROR("failed to unbind %lu pages at 0x%08llX\n",
			  (unsigned long)lg_loonggputtm_to_ttm(gtt)->num_pages, gtt->offset);
	lg_ttm_backend_unbind_return(r);
}

static void loonggpu_ttm_backend_destroy(lg_ttm_backend_func_arg)
{
	struct loonggpu_ttm_tt *gtt = (void *)ttm;

	if (gtt->usertask)
		put_task_struct(gtt->usertask);

	lg_ttm_dma_tt_fini(&gtt->ttm);
	kfree(gtt);
}

lg_define_ttm_backend_func

/**
 * loonggpu_ttm_tt_create - Create a ttm_tt object for a given BO
 *
 * @bo: The buffer object to create a GTT ttm_tt object around
 *
 * Called by ttm_tt_create().
 */
static struct ttm_tt *loonggpu_ttm_tt_create(struct ttm_buffer_object *bo,
					   uint32_t page_flags)
{
	struct loonggpu_device *adev;
	struct loonggpu_ttm_tt *gtt;
	struct loonggpu_bo *abo = ttm_to_loonggpu_bo(bo);

	adev = loonggpu_ttm_adev(bo->bdev);

	gtt = kzalloc(sizeof(struct loonggpu_ttm_tt), GFP_KERNEL);
	if (gtt == NULL) {
		return NULL;
	}

	lg_ttm_set_backend_func

	if (page_flags & TTM_TT_FLAG_EXTERNAL) {
		page_flags &= ~TTM_TT_FLAG_EXTERNAL;
		gtt->is_share_sg = true;
	}

	/* allocate space for the uninitialized page entries */
	if (lg_ttm_sg_tt_init(&gtt->ttm, abo->flags, bo, page_flags,
				adev->wc_support)) {
		kfree(gtt);
		return NULL;
	}
	return lg_loonggputtm_to_ttm(gtt);
}

/**
 * loonggpu_ttm_tt_populate - Map GTT pages visible to the device
 *
 * Map the pages of a ttm_tt object to an address space visible
 * to the underlying device.
 */
static int loonggpu_ttm_tt_populate(lg_ttm_tt_populate_arg,
			struct ttm_operation_ctx *ctx)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(lg_ttm_backend_func_get_bdev);
	struct loonggpu_ttm_tt *gtt = (void *)ttm;
	bool slave = 0;

	slave = !!(ttm->page_flags & TTM_TT_FLAG_EXTERNAL);

	/* user pages are bound by loonggpu_ttm_tt_pin_userptr() */
	if (gtt && gtt->userptr) {
		ttm->sg = kzalloc(sizeof(struct sg_table), GFP_KERNEL);
		if (!ttm->sg)
			return -ENOMEM;

		ttm->page_flags |= TTM_TT_FLAG_EXTERNAL;
		return 0;
	}

	if ((slave || (gtt && gtt->is_share_sg)) && ttm->sg) {
		lg_drm_prime_sg_to_addr_array;
		return 0;
	}

	/* fall back to generic helper to populate the page array
	 * and map them to the device */
	return lg_ttm_populate_and_map_pages(adev, &gtt->ttm, ctx);
}

/**
 * loonggpu_ttm_tt_unpopulate - unmap GTT pages and unpopulate page arrays
 *
 * Unmaps pages of a ttm_tt object from the device address space and
 * unpopulates the page array backing it.
 */
static void loonggpu_ttm_tt_unpopulate(lg_ttm_tt_populate_arg)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(lg_ttm_backend_func_get_bdev);
	struct loonggpu_ttm_tt *gtt = (void *)ttm;
	bool slave = 0;

	slave = !!(ttm->page_flags & TTM_TT_FLAG_EXTERNAL);

	if (gtt && gtt->userptr) {
		loonggpu_ttm_tt_set_user_pages(ttm, NULL);
		kfree(ttm->sg);
		ttm->page_flags &= ~TTM_TT_FLAG_EXTERNAL;
		return;
	}

	if (slave || (gtt && gtt->is_share_sg))
		return;

	/* fall back to generic helper to unmap and unpopulate array */
	lg_ttm_unmap_and_unpopulate_pages(adev, &gtt->ttm);
}

/**
 * loonggpu_ttm_tt_set_userptr - Initialize userptr GTT ttm_tt for the current
 * task
 *
 * @ttm: The ttm_tt object to bind this userptr object to
 * @addr:  The address in the current tasks VM space to use
 * @flags: Requirements of userptr object.
 *
 * Called by loonggpu_gem_userptr_ioctl() to bind userptr pages
 * to current task
 */
int loonggpu_ttm_tt_set_userptr(struct ttm_tt *ttm, uint64_t addr,
			      uint32_t flags)
{
	struct loonggpu_ttm_tt *gtt = (void *)ttm;

	if (gtt == NULL)
		return -EINVAL;

	gtt->userptr = addr;
	gtt->userflags = flags;

	if (gtt->usertask)
		put_task_struct(gtt->usertask);
	gtt->usertask = current->group_leader;
	get_task_struct(gtt->usertask);

	spin_lock_init(&gtt->guptasklock);
	INIT_LIST_HEAD(&gtt->guptasks);
	atomic_set(&gtt->mmu_invalidations, 0);
	gtt->last_set_pages = 0;

	return 0;
}

/**
 * loonggpu_ttm_tt_get_usermm - Return memory manager for ttm_tt object
 */
struct mm_struct *loonggpu_ttm_tt_get_usermm(struct ttm_tt *ttm)
{
	struct loonggpu_ttm_tt *gtt = (void *)ttm;

	if (gtt == NULL)
		return NULL;

	if (gtt->usertask == NULL)
		return NULL;

	return gtt->usertask->mm;
}

/**
 * loonggpu_ttm_tt_affect_userptr - Determine if a ttm_tt object lays inside an
 * address range for the current task.
 *
 */
bool loonggpu_ttm_tt_affect_userptr(struct ttm_tt *ttm, unsigned long start,
				  unsigned long end)
{
	struct loonggpu_ttm_tt *gtt = (void *)ttm;
	struct loonggpu_ttm_gup_task_list *entry;
	unsigned long size;

	if (gtt == NULL || !gtt->userptr)
		return false;

	/* Return false if no part of the ttm_tt object lies within
	 * the range
	 */
	size = (unsigned long)(lg_loonggputtm_to_ttm(gtt)->num_pages) * PAGE_SIZE;
	if (gtt->userptr > end || gtt->userptr + size <= start)
		return false;

	/* Search the lists of tasks that hold this mapping and see
	 * if current is one of them.  If it is return false.
	 */
	spin_lock(&gtt->guptasklock);
	list_for_each_entry(entry, &gtt->guptasks, list) {
		if (entry->task == current) {
			spin_unlock(&gtt->guptasklock);
			return false;
		}
	}
	spin_unlock(&gtt->guptasklock);

	atomic_inc(&gtt->mmu_invalidations);

	return true;
}

/**
 * loonggpu_ttm_tt_userptr_invalidated - Has the ttm_tt object been invalidated?
 */
bool loonggpu_ttm_tt_userptr_invalidated(struct ttm_tt *ttm,
				       int *last_invalidated)
{
	struct loonggpu_ttm_tt *gtt = (void *)ttm;
	int prev_invalidated = *last_invalidated;

	*last_invalidated = atomic_read(&gtt->mmu_invalidations);
	return prev_invalidated != *last_invalidated;
}

/**
 * loonggpu_ttm_tt_userptr_needs_pages - Have the pages backing this ttm_tt object
 * been invalidated since the last time they've been set?
 */
bool loonggpu_ttm_tt_userptr_needs_pages(struct ttm_tt *ttm)
{
	struct loonggpu_ttm_tt *gtt = (void *)ttm;

	if (gtt == NULL || !gtt->userptr)
		return false;

	return atomic_read(&gtt->mmu_invalidations) != gtt->last_set_pages;
}

/**
 * loonggpu_ttm_tt_is_readonly - Is the ttm_tt object read only?
 */
bool loonggpu_ttm_tt_is_readonly(struct ttm_tt *ttm)
{
	struct loonggpu_ttm_tt *gtt = (void *)ttm;

	if (gtt == NULL)
		return false;

	return !!(gtt->userflags & LOONGGPU_GEM_USERPTR_READONLY);
}

/**
 * loonggpu_ttm_tt_pte_flags - Compute PTE flags for ttm_tt object
 *
 * @ttm: The ttm_tt object to compute the flags for
 * @mem: The memory registry backing this ttm_tt object
 */
uint64_t loonggpu_ttm_tt_pte_flags(struct loonggpu_device *adev, struct ttm_tt *ttm,
				 lg_ttm_mem_t *mem)
{
	uint64_t flags = 0;

	if (mem && mem->mem_type != TTM_PL_SYSTEM)
		flags |= LOONGGPU_PTE_PRESENT;

	flags |= adev->gart.gart_pte_flags;

	if (!loonggpu_ttm_tt_is_readonly(ttm))
		flags |= LOONGGPU_PTE_WRITEABLE;

	return flags;
}

/**
 * loonggpu_ttm_bo_eviction_valuable - Check to see if we can evict a buffer
 * object.
 *
 * Return true if eviction is sensible. Called by ttm_mem_evict_first() on
 * behalf of ttm_bo_mem_force_space() which tries to evict buffer objects until
 * it can find space for a new object and by ttm_bo_force_list_clean() which is
 * used to clean out a memory space.
 */
static bool loonggpu_ttm_bo_eviction_valuable(struct ttm_buffer_object *bo,
					    const struct ttm_place *place)
{
	struct loonggpu_bo *gbo = ttm_to_loonggpu_bo(bo);
	struct dma_fence *f;
	int __maybe_unused i;

	/* If bo is a KCD BO, check if the bo belongs to the current process.
	 * If true, then return false as any KCD process needs all its BOs to
	 * be resident to run successfully
	 */
#if defined(dma_resv_for_each_fence)
	struct dma_resv_iter resv_cursor;
	dma_resv_for_each_fence(&resv_cursor, bo->base.resv,
				DMA_RESV_USAGE_BOOKKEEP, f) {
		if (lgkcd_fence_check_mm(f, current->mm))
			return false;
	}
#else
	lg_dma_resv_list_t *flist;
	flist = lg_dma_resv_get_list(lg_tbo_to_resv(bo));
	if (flist) {
		for (i = 0; i < flist->shared_count; ++i) {
			f = rcu_dereference_protected(flist->shared[i],
				reservation_object_held(bo->resv));
			if (lgkcd_fence_check_mm(f, current->mm))
				return false;
		}
	}
#endif
	if (!loonggpu_bo_is_loonggpu_bo(bo))
                return ttm_bo_eviction_valuable(bo, place);

	if (gbo->flags & LOONGGPU_GEM_CREATE_PREEMPTIBLE)
		return false;

        if (lg_tbo_to_mem(bo)->mem_type == LOONGGPU_PL_DOORBELL)
                return false;

        /* Swapout? */
        if (lg_tbo_to_mem(bo)->mem_type == TTM_PL_SYSTEM)
                return true;

        return ttm_bo_eviction_valuable(bo, place);
}

/**
 * loonggpu_ttm_access_memory - Read or Write memory that backs a buffer object.
 *
 * @bo:  The buffer object to read/write
 * @offset:  Offset into buffer object
 * @buf:  Secondary buffer to write/read from
 * @len: Length in bytes of access
 * @write:  true if writing
 *
 * This is used to access VRAM that backs a buffer object via MMIO
 * access for debugging purposes.
 */
static int loonggpu_ttm_access_memory(struct ttm_buffer_object *bo,
				    unsigned long offset,
				    void *buf, int len, int write)
{
	struct loonggpu_bo *abo = ttm_to_loonggpu_bo(bo);
	struct loonggpu_device *adev = loonggpu_ttm_adev(abo->tbo.bdev);
	struct drm_mm_node __maybe_unused *nodes;
	struct loonggpu_res_cursor __maybe_unused cursor;
	uint32_t value = 0;
	int ret = 0;
	uint64_t pos;
	unsigned long flags;

	if (lg_tbo_to_mem(bo)->mem_type != TTM_PL_VRAM)
		return -EIO;

	if (adev->family_type < CHIP_LG200)
		return -EPERM;

	loonggpu_res_first(bo->resource, offset, len, &cursor);
	pos = cursor.start;

	while (len && pos < adev->gmc.mc_vram_size) {
		uint32_t bytes = 4 - (pos & 3);
		uint32_t shift = (pos & 3) * 8;
		uint32_t mask = 0xffffffff << shift;
		uint64_t tmp;

		if (len < bytes) {
			mask &= 0xffffffff >> (bytes - len) * 8;
			bytes = len;
		}

		spin_lock_irqsave(&adev->mmio_idx_lock, flags);
		loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_VRAM, LG2XX_ICMD32_SOP_VRAM_BINDADDR),
			lower_32_bits(pos + adev->gmc.vram_start),
			upper_32_bits(pos + adev->gmc.vram_start));
		if (!write || mask != 0xffffffff) {
			tmp = loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_VRAM,
				LG2XX_ICMD32_SOP_VRAM_READ), 4, 0);
			value = lower_32_bits(tmp);
		}
		if (write) {
			value &= ~mask;
			value |= (*(uint32_t *)buf << shift) & mask;
			loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_VRAM,
				LG2XX_ICMD32_SOP_VRAM_WRITE), 4, value);
		}
		spin_unlock_irqrestore(&adev->mmio_idx_lock, flags);
		if (!write) {
			value = (value & mask) >> shift;
			memcpy(buf, &value, bytes);
		}

		ret += bytes;
		buf = (uint8_t *)buf + bytes;
		pos += bytes;
		len -= bytes;
		if (pos >= (cursor.start + cursor.size)) {
			loonggpu_res_next(&cursor, cursor.size);
			pos = cursor.start;
		}
	}

	return ret;
}

static struct ttm_device_funcs loonggpu_bo_driver = {
	.ttm_tt_create = &loonggpu_ttm_tt_create,
	.ttm_tt_populate = &loonggpu_ttm_tt_populate,
	.ttm_tt_unpopulate = &loonggpu_ttm_tt_unpopulate,
	.ttm_tt_destroy = &loonggpu_ttm_backend_destroy,
	.eviction_valuable = &loonggpu_ttm_bo_eviction_valuable,
	.evict_flags = &loonggpu_evict_flags,
	.move = &loonggpu_bo_move,
	.io_mem_reserve = &loonggpu_ttm_io_mem_reserve,
	.io_mem_pfn = loonggpu_ttm_io_mem_pfn_buddy,
	.access_memory = &loonggpu_ttm_access_memory,
};


/*
 * Firmware Reservation functions
 */
/**
 * loonggpu_ttm_fw_reserve_vram_fini - free fw reserved vram
 *
 * @adev: loonggpu_device pointer
 *
 * free fw reserved vram if it has been reserved.
 */
static void loonggpu_ttm_fw_reserve_vram_fini(struct loonggpu_device *adev)
{
	loonggpu_bo_free_kernel(&adev->fw_vram_usage.reserved_bo,
		NULL, &adev->fw_vram_usage.va);
}

/**
 * loonggpu_ttm_fw_reserve_vram_init - create bo vram reservation from fw
 *
 * @adev: loonggpu_device pointer
 *
 * create bo vram reservation from fw.
 */
static int loonggpu_ttm_fw_reserve_vram_init(struct loonggpu_device *adev)
{
	struct ttm_operation_ctx ctx = { false, false };
	struct loonggpu_bo_param bp;
	int r = 0;
	int i;
	u64 vram_size = adev->gmc.visible_vram_size;
	u64 offset = adev->fw_vram_usage.start_offset;
	u64 size = adev->fw_vram_usage.size;
	struct loonggpu_bo *bo;

	memset(&bp, 0, sizeof(bp));
	bp.size = adev->fw_vram_usage.size;
	bp.byte_align = PAGE_SIZE;
	bp.domain = LOONGGPU_GEM_DOMAIN_VRAM;
	bp.flags = LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED |
		LOONGGPU_GEM_CREATE_VRAM_CONTIGUOUS;
	bp.type = ttm_bo_type_kernel;
	bp.resv = NULL;
	adev->fw_vram_usage.va = NULL;
	adev->fw_vram_usage.reserved_bo = NULL;

	if (adev->fw_vram_usage.size > 0 &&
		adev->fw_vram_usage.size <= vram_size) {

		r = loonggpu_bo_create(adev, &bp,
				     &adev->fw_vram_usage.reserved_bo);
		if (r)
			goto error_create;

		r = loonggpu_bo_reserve(adev->fw_vram_usage.reserved_bo, false);
		if (r)
			goto error_reserve;

		/* remove the original mem node and create a new one at the
		 * request position
		 */
		bo = adev->fw_vram_usage.reserved_bo;
		offset = ALIGN(offset, PAGE_SIZE);
		for (i = 0; i < bo->placement.num_placement; ++i) {
			bo->placements[i].fpfn = offset >> PAGE_SHIFT;
			bo->placements[i].lpfn = (offset + size) >> PAGE_SHIFT;
		}

		lg_ttm_bo_mem_put(&bo->tbo, lg_tbo_to_mem(&bo->tbo));
		r = lg_ttm_bo_mem_space(&bo->tbo, &bo->placement,
					lg_tbo_to_mem(&bo->tbo),
					&ctx);
		if (r)
			goto error_pin;

		r = loonggpu_bo_pin_restricted(adev->fw_vram_usage.reserved_bo,
			LOONGGPU_GEM_DOMAIN_VRAM,
			adev->fw_vram_usage.start_offset,
			(adev->fw_vram_usage.start_offset +
			adev->fw_vram_usage.size));
		if (r)
			goto error_pin;
		r = loonggpu_bo_kmap(adev->fw_vram_usage.reserved_bo,
			&adev->fw_vram_usage.va);
		if (r)
			goto error_kmap;

		loonggpu_bo_unreserve(adev->fw_vram_usage.reserved_bo);
	}
	return r;

error_kmap:
	loonggpu_bo_unpin(adev->fw_vram_usage.reserved_bo);
error_pin:
	loonggpu_bo_unreserve(adev->fw_vram_usage.reserved_bo);
error_reserve:
	loonggpu_bo_unref(&adev->fw_vram_usage.reserved_bo);
error_create:
	adev->fw_vram_usage.va = NULL;
	adev->fw_vram_usage.reserved_bo = NULL;
	return r;
}

/**
 * loonggpu_ttm_init - Init the memory management (ttm) as well as various
 * gtt/vram related fields.
 *
 * This initializes all of the memory space pools that the TTM layer
 * will need such as the GTT space (system memory mapped to the device),
 * VRAM (on-board memory), and on-chip memories (GDS, GWS, OA) which
 * can be mapped per VMID.
 */
int loonggpu_ttm_init(struct loonggpu_device *adev)
{
	uint64_t gtt_size;
	int r;
	u64 vis_vram_limit;

	/* doorbell bar mapping and doorbell index init*/
	loonggpu_doorbell_init(adev);

	mutex_init(&adev->mman.gtt_window_lock);

	/* No others user of address space so set it to 0 */
	r = lg_loonggpu_ttm_bo_device_init(adev, &loonggpu_bo_driver);
	if (r) {
		DRM_ERROR("failed initializing buffer object driver(%d).\n", r);
		return r;
	}
	adev->mman.initialized = true;

	/* Initialize VRAM pool with all of VRAM divided into pages */
	r = loonggpu_vram_mgr_init_buddy(adev);
	if (r) {
		DRM_ERROR("Failed initializing VRAM heap.\n");
		return r;
	}

	/* Reduce size of CPU-visible VRAM if requested */
	vis_vram_limit = (u64)loonggpu_vis_vram_limit * 1024 * 1024;
	if (loonggpu_vis_vram_limit > 0 &&
	    vis_vram_limit <= adev->gmc.visible_vram_size)
		adev->gmc.visible_vram_size = vis_vram_limit;

	/* Change the size here instead of the init above so only lpfn is affected */
	loonggpu_ttm_set_buffer_funcs_status(adev, false);
#ifdef CONFIG_64BIT
	if (loonggpu_using_ram)
		adev->mman.aper_base_kaddr =
			(void *)TO_CACHE(adev->gmc.aper_base);
	else
		adev->mman.aper_base_kaddr = ioremap(adev->gmc.aper_base,
						adev->gmc.visible_vram_size);
#endif

	/*
	 *The reserved vram for firmware must be pinned to the specified
	 *place on the VRAM, so reserve it early.
	 */
	r = loonggpu_ttm_fw_reserve_vram_init(adev);
	if (r) {
		return r;
	}

	/* allocate memory as required for VGA
	 * This is used for VGA emulation and pre-OS scanout buffers to
	 * avoid display artifacts while transitioning between pre-OS
	 * and driver.  */
	if (adev->gmc.stolen_size) {
		r = loonggpu_bo_create_kernel(adev, adev->gmc.stolen_size, PAGE_SIZE,
					    LOONGGPU_GEM_DOMAIN_VRAM,
					    &adev->stolen_vga_memory,
					    NULL, NULL);
		if (r)
			return r;
	}
	DRM_DEBUG("loonggpu: %uM of VRAM memory ready\n",
		 (unsigned) (adev->gmc.real_vram_size / (1024 * 1024)));

	if (adev->doorbell.size) {
		/* Initialize DOORBELL pool with all of DOORBELL divided into pages */
		r = ttm_range_man_init(&adev->mman.bdev, LOONGGPU_PL_DOORBELL,
					false, adev->doorbell.size >> PAGE_SHIFT);
		if (r) {
			DRM_ERROR("Failed initializing DOORBELL heap.\n");
			return r;
		}
		DRM_INFO("loonggpu: %uM of DOORBELL memory ready\n",
			(unsigned) (adev->doorbell.size / (1024 * 1024)));

		/* Create a boorbell page for kernel usages */
		r = loonggpu_doorbell_create_kernel_doorbells(adev);
		if (r) {
			DRM_ERROR("Failed to initialize kernel doorbells.\n");
			return r;
		}
	}

	/* Compute GTT size, either bsaed on 3/4th the size of RAM size
	 * or whatever the user passed on module init */
	if (loonggpu_gtt_size == -1) {
		struct sysinfo si;

		si_meminfo(&si);
		gtt_size = min(max((LOONGGPU_DEFAULT_GTT_SIZE_MB << 20),
			       adev->gmc.mc_vram_size),
			       ((uint64_t)si.totalram * si.mem_unit * 3/4));
	} else
		gtt_size = (uint64_t)loonggpu_gtt_size << 20;

	/* Initialize GTT memory pool */
	r = lg_gtt_mgr_init(adev, gtt_size);
	if (r) {
		DRM_ERROR("Failed initializing GTT heap.\n");
		return r;
	}
	DRM_INFO("loonggpu: %uM of GTT memory ready.\n",
		 (unsigned)(gtt_size / (1024 * 1024)));

	/* Register debugfs entries for loonggpu_ttm */
	r = loonggpu_ttm_debugfs_init(adev);
	if (r) {
		DRM_ERROR("Failed to init debugfs\n");
		return r;
	}
	return 0;
}

/**
 * loonggpu_ttm_late_init - Handle any late initialization for loonggpu_ttm
 */
void loonggpu_ttm_late_init(struct loonggpu_device *adev)
{
	/* return the VGA stolen memory (if any) back to VRAM */
	loonggpu_bo_free_kernel(&adev->stolen_vga_memory, NULL, NULL);
}

/**
 * loonggpu_ttm_fini - De-initialize the TTM memory pools
 */
void loonggpu_ttm_fini(struct loonggpu_device *adev)
{
	if (!adev->mman.initialized)
		return;

	if (adev->doorbell.size)
		loonggpu_doorbell_fini(adev);

	loonggpu_ttm_debugfs_fini(adev);
	loonggpu_ttm_fw_reserve_vram_fini(adev);
	if (adev->mman.aper_base_kaddr)
		iounmap(adev->mman.aper_base_kaddr);
	adev->mman.aper_base_kaddr = NULL;

	lg_loonggpu_vram_mgr_fini(adev);
	lg_loonggpu_gtt_mgr_fini(adev);
	lg_ttm_bo_clean_mm(adev, TTM_PL_VRAM);
	if (adev->doorbell.size)
		lg_ttm_bo_clean_mm(adev, LOONGGPU_PL_DOORBELL);
	lg_ttm_bo_clean_mm(adev, TTM_PL_TT);
	ttm_device_fini(&adev->mman.bdev);
	mutex_destroy(&adev->mman.gtt_window_lock);
	adev->mman.initialized = false;
	DRM_INFO("loonggpu: ttm finalized\n");
}

/**
 * loonggpu_ttm_set_buffer_funcs_status - enable/disable use of buffer functions
 *
 * @adev: loonggpu_device pointer
 * @enable: true when we can use buffer functions.
 *
 * Enable/disable use of buffer functions during suspend/resume. This should
 * only be called at bootup or when userspace isn't running.
 */
void loonggpu_ttm_set_buffer_funcs_status(struct loonggpu_device *adev, bool enable)
{
	lg_ttm_manager_t *man = lg_bdev_to_ttm_man(&adev->mman.bdev, TTM_PL_VRAM);
	uint64_t size;
	int r;

	if (!adev->mman.initialized || adev->in_gpu_reset ||
	    adev->mman.buffer_funcs_enabled == enable)
		return;

	if (enable) {
		struct loonggpu_ring *ring;
		struct drm_sched_rq *rq;
		struct drm_gpu_scheduler *sched;

		ring = adev->mman.buffer_funcs_ring;
		rq = sched->sched_rq[DRM_SCHED_PRIORITY_KERNEL];
		sched = &ring->sched;
		r = lg_drm_sched_entity_init(&adev->mman.entity, DRM_SCHED_PRIORITY_KERNEL,
					     &sched, 1, &rq, 1, NULL);
		if (r) {
			DRM_ERROR("Failed setting up TTM BO move entity (%d)\n",
				  r);
			return;
		}
	} else {
		drm_sched_entity_destroy(&adev->mman.entity);
		ttm_resource_manager_cleanup(man);
	}

	/* this just adjusts TTM size idea, which sets lpfn to the correct value */
	if (enable)
		size = adev->gmc.real_vram_size;
	else
		size = adev->gmc.visible_vram_size;
	man->size = size;
	adev->mman.buffer_funcs_enabled = enable;
}

static int loonggpu_map_buffer(struct ttm_buffer_object *bo,
			     lg_ttm_mem_t *mem, struct loonggpu_res_cursor *mm_cur, unsigned num_pages,
			     uint64_t offset, unsigned window,
			     struct loonggpu_ring *ring,
			     uint64_t *addr)
{
	struct loonggpu_ttm_tt *gtt = (void *)bo->ttm;
	struct loonggpu_device *adev = ring->adev;
	struct ttm_tt *ttm = bo->ttm;
	struct loonggpu_job *job;
	unsigned num_dw, num_bytes;
	dma_addr_t *dma_address;
	struct dma_fence *fence;
	uint64_t src_addr, dst_addr;
	uint64_t flags, dma_offset;
	int r;

	BUG_ON(adev->mman.buffer_funcs->copy_max_bytes <
	       LOONGGPU_GTT_MAX_TRANSFER_SIZE * 8);

	if (mem->start != LOONGGPU_BO_INVALID_OFFSET) {
		*addr = loonggpu_ttm_domain_start(adev, mem->mem_type) + mm_cur->start;
		return 0;
	}
	dma_offset = mm_cur->start >> PAGE_SHIFT;
	*addr = adev->gmc.gart_start;
	*addr += (u64)window * LOONGGPU_GTT_MAX_TRANSFER_SIZE *
		LOONGGPU_GPU_PAGE_SIZE;

	num_dw = adev->mman.buffer_funcs->copy_num_dw;
	while (num_dw & 0x7)
		num_dw++;

	num_bytes = num_pages * 8;

	r = loonggpu_job_alloc_with_ib(adev, num_dw * 4 + num_bytes, &job);
	if (r)
		return r;

	src_addr = num_dw * 4;
	src_addr += job->ibs[0].gpu_addr;

	dst_addr = adev->gart.table_addr;
	dst_addr += window * LOONGGPU_GTT_MAX_TRANSFER_SIZE * 8;
	loonggpu_emit_copy_buffer(adev, &job->ibs[0], src_addr,
				dst_addr, num_bytes);

	loonggpu_ring_pad_ib(ring, &job->ibs[0]);
	WARN_ON(job->ibs[0].length_dw > num_dw);

	dma_address = &gtt->ttm.dma_address[dma_offset];
	flags = loonggpu_ttm_tt_pte_flags(adev, ttm, mem);
	r = loonggpu_gart_map(adev, 0, num_pages, dma_address, flags,
			    &job->ibs[0].ptr[num_dw]);
	if (r)
		goto error_free;

	r = loonggpu_job_submit(job, &adev->mman.entity,
			      LOONGGPU_FENCE_OWNER_UNDEFINED, &fence);
	if (r)
		goto error_free;

	dma_fence_put(fence);

	return r;

error_free:
	loonggpu_job_free(job);
	return r;
}

int loonggpu_copy_buffer(struct loonggpu_ring *ring, uint64_t src_offset,
		       uint64_t dst_offset, uint32_t byte_count,
		       lg_dma_resv_t *resv, struct dma_fence **fence,
		       bool direct_submit, bool vm_needs_flush)
{
	struct loonggpu_device *adev = ring->adev;
	struct loonggpu_job *job;

	uint32_t max_bytes;
	unsigned num_loops, num_dw;
	unsigned i;
	int r;

	if (direct_submit && !ring->ready) {
		DRM_ERROR("Trying to move memory with ring turned off.\n");
		return -EINVAL;
	}

	max_bytes = adev->mman.buffer_funcs->copy_max_bytes;
	num_loops = DIV_ROUND_UP(byte_count, max_bytes);
	num_dw = num_loops * adev->mman.buffer_funcs->copy_num_dw;

	/* for IB padding */
	while (num_dw & 0x7)
		num_dw++;

	r = loonggpu_job_alloc_with_ib(adev, num_dw * 4, &job);
	if (r)
		return r;

	job->vm_needs_flush = vm_needs_flush;
	if (resv) {
		r = loonggpu_sync_resv(adev, &job->sync, resv,
				     LOONGGPU_SYNC_ALWAYS,
				     LOONGGPU_FENCE_OWNER_UNDEFINED,
				     false);
		if (r) {
			DRM_ERROR("sync failed (%d).\n", r);
			goto error_free;
		}
	}

	for (i = 0; i < num_loops; i++) {
		uint32_t cur_size_in_bytes = min(byte_count, max_bytes);

		loonggpu_emit_copy_buffer(adev, &job->ibs[0], src_offset,
					dst_offset, cur_size_in_bytes);

		src_offset += cur_size_in_bytes;
		dst_offset += cur_size_in_bytes;
		byte_count -= cur_size_in_bytes;
	}

	loonggpu_ring_pad_ib(ring, &job->ibs[0]);
	WARN_ON(job->ibs[0].length_dw > num_dw);
	if (direct_submit)
		r = loonggpu_job_submit_direct(job, ring, fence);
	else
		r = loonggpu_job_submit(job, &adev->mman.entity,
				      LOONGGPU_FENCE_OWNER_UNDEFINED, fence);
	if (r)
		goto error_free;

	return r;

error_free:
	loonggpu_job_free(job);
	DRM_ERROR("Error scheduling IBs (%d)\n", r);
	return r;
}

int loonggpu_fill_buffer(struct loonggpu_bo *bo,
		       uint32_t src_data,
		       lg_dma_resv_t *resv,
		       struct dma_fence **fence)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);
	uint32_t max_bytes = adev->mman.buffer_funcs->fill_max_bytes;
	struct loonggpu_ring *ring = adev->mman.buffer_funcs_ring;

	struct drm_mm_node *mm_node;
	unsigned long num_pages;
	unsigned int num_loops, num_dw;

	struct loonggpu_job *job;
	int r;

	if (!adev->mman.buffer_funcs_enabled) {
		DRM_ERROR("Trying to clear memory with ring turned off.\n");
		return -EINVAL;
	}

	if (lg_get_bo_mem_type(bo) == TTM_PL_TT) {
		r = loonggpu_ttm_alloc_gart(&bo->tbo);
		if (r)
			return r;
	}

	num_pages = lg_tbo_to_num_pages(&bo->tbo);
	mm_node = lg_res_to_drm_node(lg_tbo_to_mem(&bo->tbo));
	num_loops = 0;
	while (num_pages) {
		uint32_t byte_count = mm_node->size << PAGE_SHIFT;

		num_loops += DIV_ROUND_UP(byte_count, max_bytes);
		num_pages -= mm_node->size;
		++mm_node;
	}
	num_dw = num_loops * adev->mman.buffer_funcs->fill_num_dw;

	/* for IB padding */
	num_dw += 64;

	r = loonggpu_job_alloc_with_ib(adev, num_dw * 4, &job);
	if (r)
		return r;

	if (resv) {
		r = loonggpu_sync_resv(adev, &job->sync, resv, LOONGGPU_SYNC_ALWAYS,
				     LOONGGPU_FENCE_OWNER_UNDEFINED, false);
		if (r) {
			DRM_ERROR("sync failed (%d).\n", r);
			goto error_free;
		}
	}

	num_pages = lg_tbo_to_num_pages(&bo->tbo);
	mm_node = lg_res_to_drm_node(lg_tbo_to_mem(&bo->tbo));

	while (num_pages) {
		uint32_t byte_count = mm_node->size << PAGE_SHIFT;
		uint64_t dst_addr;

		dst_addr = loonggpu_mm_node_addr(&bo->tbo, mm_node, lg_tbo_to_mem(&bo->tbo));
		while (byte_count) {
			uint32_t cur_size_in_bytes = min(byte_count, max_bytes);

			loonggpu_emit_fill_buffer(adev, &job->ibs[0], src_data,
						dst_addr, cur_size_in_bytes);

			dst_addr += cur_size_in_bytes;
			byte_count -= cur_size_in_bytes;
		}

		num_pages -= mm_node->size;
		++mm_node;
	}

	loonggpu_ring_pad_ib(ring, &job->ibs[0]);
	WARN_ON(job->ibs[0].length_dw > num_dw);
	r = loonggpu_job_submit(job, &adev->mman.entity,
			      LOONGGPU_FENCE_OWNER_UNDEFINED, fence);
	if (r)
		goto error_free;

	return 0;

error_free:
	loonggpu_job_free(job);
	return r;
}

#if defined(CONFIG_DEBUG_FS)

static int loonggpu_mm_dump_table(struct seq_file *m, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	unsigned ttm_pl = *(int *)node->info_ent->data;
	struct drm_device *dev = node->minor->dev;
	struct loonggpu_device *adev = dev->dev_private;
	lg_ttm_manager_t *man = lg_bdev_to_ttm_man(&adev->mman.bdev, ttm_pl);
	struct drm_printer p = drm_seq_file_printer(m);

	man->func->debug(man, &p);
	return 0;
}

static int ttm_pl_vram = TTM_PL_VRAM;
static int ttm_pl_tt = TTM_PL_TT;

static const struct drm_info_list loonggpu_ttm_debugfs_list[] = {
	{"loonggpu_vram_mm", loonggpu_mm_dump_table, 0, &ttm_pl_vram},
	{"loonggpu_gtt_mm", loonggpu_mm_dump_table, 0, &ttm_pl_tt},
};

/**
 * loonggpu_ttm_vram_read - Linear read access to VRAM
 *
 * Accesses VRAM via MMIO for debugging purposes.
 */
static ssize_t loonggpu_ttm_vram_read(struct file *f, char __user *buf,
				    size_t size, loff_t *pos)
{
	struct loonggpu_device *adev = file_inode(f)->i_private;
	ssize_t result = 0;
	int r;

	if (size & 0x3 || *pos & 0x3)
		return -EINVAL;

	if (*pos >= adev->gmc.mc_vram_size)
		return -ENXIO;

	if (adev->family_type < CHIP_LG200)
		return -EPERM;

	while (size) {
		unsigned long flags;
		uint32_t value;
		uint64_t tmp;

		if (*pos >= adev->gmc.mc_vram_size)
			return result;

		spin_lock_irqsave(&adev->mmio_idx_lock, flags);
		loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_VRAM, LG2XX_ICMD32_SOP_VRAM_BINDADDR),
			lower_32_bits(*pos + adev->gmc.vram_start),
			upper_32_bits(*pos + adev->gmc.vram_start));
		tmp = loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_VRAM,
			LG2XX_ICMD32_SOP_VRAM_READ), 4, 0);
		value = lower_32_bits(tmp);
		spin_unlock_irqrestore(&adev->mmio_idx_lock, flags);

		r = put_user(value, (uint32_t *)buf);
		if (r)
			return r;

		result += 4;
		buf += 4;
		*pos += 4;
		size -= 4;
	}

	return result;
}

/**
 * loonggpu_ttm_vram_write - Linear write access to VRAM
 *
 * Accesses VRAM via MMIO for debugging purposes.
 */
static ssize_t loonggpu_ttm_vram_write(struct file *f, const char __user *buf,
				    size_t size, loff_t *pos)
{
	struct loonggpu_device *adev = file_inode(f)->i_private;
	ssize_t result = 0;
	int r;

	if (size & 0x3 || *pos & 0x3)
		return -EINVAL;

	if (*pos >= adev->gmc.mc_vram_size)
		return -ENXIO;

	if (adev->family_type < CHIP_LG200)
		return -EPERM;

	while (size) {
		unsigned long flags;
		uint32_t value;

		if (*pos >= adev->gmc.mc_vram_size)
			return result;

		r = get_user(value, (uint32_t *)buf);
		if (r)
			return r;

		spin_lock_irqsave(&adev->mmio_idx_lock, flags);
		loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_VRAM, LG2XX_ICMD32_SOP_VRAM_BINDADDR),
			lower_32_bits(*pos + adev->gmc.vram_start),
			upper_32_bits(*pos + adev->gmc.vram_start));
		loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_VRAM,
			LG2XX_ICMD32_SOP_VRAM_WRITE), 4, value);
		spin_unlock_irqrestore(&adev->mmio_idx_lock, flags);

		result += 4;
		buf += 4;
		*pos += 4;
		size -= 4;
	}

	return result;
}

static const struct file_operations loonggpu_ttm_vram_fops = {
	.owner = THIS_MODULE,
	.read = loonggpu_ttm_vram_read,
	.write = loonggpu_ttm_vram_write,
	.llseek = default_llseek,
};

#ifdef CONFIG_DRM_LOONGGPU_GART_DEBUGFS

/**
 * loonggpu_ttm_gtt_read - Linear read access to GTT memory
 */
static ssize_t loonggpu_ttm_gtt_read(struct file *f, char __user *buf,
				   size_t size, loff_t *pos)
{
	struct loonggpu_device *adev = file_inode(f)->i_private;
	ssize_t result = 0;
	int r;

	while (size) {
		loff_t p = *pos / PAGE_SIZE;
		unsigned off = *pos & ~PAGE_MASK;
		size_t cur_size = min_t(size_t, size, PAGE_SIZE - off);
		struct page *page;
		void *ptr;

		if (p >= adev->gart.num_cpu_pages)
			return result;

		page = adev->gart.pages[p];
		if (page) {
			ptr = kmap(page);
			ptr += off;

			r = copy_to_user(buf, ptr, cur_size);
			kunmap(adev->gart.pages[p]);
		} else
			r = clear_user(buf, cur_size);

		if (r)
			return -EFAULT;

		result += cur_size;
		buf += cur_size;
		*pos += cur_size;
		size -= cur_size;
	}

	return result;
}

static const struct file_operations loonggpu_ttm_gtt_fops = {
	.owner = THIS_MODULE,
	.read = loonggpu_ttm_gtt_read,
	.llseek = default_llseek
};

#endif

/**
 * loonggpu_iomem_read - Virtual read access to GPU mapped memory
 *
 * This function is used to read memory that has been mapped to the
 * GPU and the known addresses are not physical addresses but instead
 * bus addresses (e.g., what you'd put in an IB or ring buffer).
 */
static ssize_t loonggpu_iomem_read(struct file *f, char __user *buf,
				 size_t size, loff_t *pos)
{
	struct loonggpu_device *adev = file_inode(f)->i_private;
	struct iommu_domain *dom;
	ssize_t result = 0;
	int r;

	/* retrieve the IOMMU domain if any for this device */
	dom = iommu_get_domain_for_dev(adev->dev);

	while (size) {
		phys_addr_t addr = *pos & PAGE_MASK;
		loff_t off = *pos & ~PAGE_MASK;
		size_t bytes = PAGE_SIZE - off;
		unsigned long pfn;
		struct page *p;
		void *ptr;

		bytes = bytes < size ? bytes : size;

		/* Translate the bus address to a physical address.  If
		 * the domain is NULL it means there is no IOMMU active
		 * and the address translation is the identity
		 */
		addr = dom ? iommu_iova_to_phys(dom, addr) : addr;

		pfn = addr >> PAGE_SHIFT;
		if (!pfn_valid(pfn))
			return -EPERM;

		p = pfn_to_page(pfn);
		if (p->mapping != adev->mman.bdev.dev_mapping)
			return -EPERM;

		ptr = kmap(p);
		r = copy_to_user(buf, ptr + off, bytes);
		kunmap(p);
		if (r)
			return -EFAULT;

		size -= bytes;
		*pos += bytes;
		result += bytes;
	}

	return result;
}

/**
 * loonggpu_iomem_write - Virtual write access to GPU mapped memory
 *
 * This function is used to write memory that has been mapped to the
 * GPU and the known addresses are not physical addresses but instead
 * bus addresses (e.g., what you'd put in an IB or ring buffer).
 */
static ssize_t loonggpu_iomem_write(struct file *f, const char __user *buf,
				 size_t size, loff_t *pos)
{
	struct loonggpu_device *adev = file_inode(f)->i_private;
	struct iommu_domain *dom;
	ssize_t result = 0;
	int r;

	dom = iommu_get_domain_for_dev(adev->dev);

	while (size) {
		phys_addr_t addr = *pos & PAGE_MASK;
		loff_t off = *pos & ~PAGE_MASK;
		size_t bytes = PAGE_SIZE - off;
		unsigned long pfn;
		struct page *p;
		void *ptr;

		bytes = bytes < size ? bytes : size;

		addr = dom ? iommu_iova_to_phys(dom, addr) : addr;

		pfn = addr >> PAGE_SHIFT;
		if (!pfn_valid(pfn))
			return -EPERM;

		p = pfn_to_page(pfn);
		if (p->mapping != adev->mman.bdev.dev_mapping)
			return -EPERM;

		ptr = kmap(p);
		r = copy_from_user(ptr + off, buf, bytes);
		kunmap(p);
		if (r)
			return -EFAULT;

		size -= bytes;
		*pos += bytes;
		result += bytes;
	}

	return result;
}

static const struct file_operations loonggpu_ttm_iomem_fops = {
	.owner = THIS_MODULE,
	.read = loonggpu_iomem_read,
	.write = loonggpu_iomem_write,
	.llseek = default_llseek
};

static const struct {
	char *name;
	const struct file_operations *fops;
	int domain;
} ttm_debugfs_entries[] = {
	{ "loonggpu_vram", &loonggpu_ttm_vram_fops, TTM_PL_VRAM },
#ifdef CONFIG_DRM_LOONGGPU_GART_DEBUGFS
	{ "loonggpu_gtt", &loonggpu_ttm_gtt_fops, TTM_PL_TT },
#endif
	{ "loonggpu_iomem", &loonggpu_ttm_iomem_fops, TTM_PL_SYSTEM },
};

#endif

static int loonggpu_ttm_debugfs_init(struct loonggpu_device *adev)
{
#if defined(CONFIG_DEBUG_FS)
	unsigned count;

	struct drm_minor *minor = adev->ddev->primary;
	struct dentry *ent, *root = minor->debugfs_root;

	for (count = 0; count < ARRAY_SIZE(ttm_debugfs_entries); count++) {
		ent = debugfs_create_file(
				ttm_debugfs_entries[count].name,
				S_IFREG | S_IRUGO, root,
				adev,
				ttm_debugfs_entries[count].fops);
		if (IS_ERR(ent))
			return PTR_ERR(ent);
		if (ttm_debugfs_entries[count].domain == TTM_PL_VRAM)
			i_size_write(ent->d_inode, adev->gmc.mc_vram_size);
		else if (ttm_debugfs_entries[count].domain == TTM_PL_TT)
			i_size_write(ent->d_inode, adev->gmc.gart_size);
		adev->mman.debugfs_entries[count] = ent;
	}

	count = ARRAY_SIZE(loonggpu_ttm_debugfs_list);

	return loonggpu_debugfs_add_files(adev, loonggpu_ttm_debugfs_list, count);
#else
	return 0;
#endif
}

static void loonggpu_ttm_debugfs_fini(struct loonggpu_device *adev)
{
#if defined(CONFIG_DEBUG_FS)
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(ttm_debugfs_entries); i++)
		debugfs_remove(adev->mman.debugfs_entries[i]);
#endif
}

uint64_t loonggpu_ttm_domain_start(struct loonggpu_device *adev, uint32_t type)
{
	switch (type) {
	case TTM_PL_TT:
		return adev->gmc.gart_start;
	case TTM_PL_VRAM:
		return adev->gmc.vram_start;
	case LOONGGPU_PL_DOORBELL:
		return adev->doorbell.base;
	}

	return 0;
}
