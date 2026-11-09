#include "loonggpu.h"
#include "loonggpu_vram_mgr_helper.h"
#include "loonggpu_ttm.h"

struct loonggpu_vram_reservation {
	u64 start;
	u64 size;
	struct list_head allocated;
	struct list_head blocks;
};

/**
 * loonggpu_vram_mgr_init - init VRAM manager and DRM MM
 *
 * @man: TTM memory type manager
 * @p_size: maximum size of VRAM
 *
 * Allocate and initialize the VRAM manager.
 */
int loonggpu_vram_mgr_init_buddy(struct loonggpu_device *adev)
{
	struct loonggpu_vram_mgr *mgr = &adev->mman.vram_mgr;
	struct ttm_resource_manager *man = &mgr->manager;
	int err;

	ttm_resource_manager_init(man, &adev->mman.bdev, adev->gmc.real_vram_size);

	mutex_init(&mgr->m_lock);
	INIT_LIST_HEAD(&mgr->reservations_pending);
	INIT_LIST_HEAD(&mgr->reserved_pages);
	mgr->default_page_size = PAGE_SIZE;

	man->func = &loonggpu_vram_mgr_func;
	err = lg_buddy_init(&mgr->b_mm, man->size, PAGE_SIZE);
	if (err)
		return err;
	ttm_set_driver_manager(&adev->mman.bdev, TTM_PL_VRAM, &mgr->manager);
	ttm_resource_manager_set_used(man, true);

	return 0;
}

/**
 * loonggpu_vram_mgr_fini - free and destroy VRAM manager
 *
 * @man: TTM memory type manager
 *
 * Destroy and free the VRAM manager, returns -EBUSY if ranges are still
 * allocated inside it.
 */
void loonggpu_vram_mgr_fini_buddy(struct loonggpu_device *adev)
{
	struct loonggpu_vram_mgr *mgr = &adev->mman.vram_mgr;
	struct ttm_resource_manager *man = &mgr->manager;
	int ret;
	struct loonggpu_vram_reservation *rsv, *temp;

	ttm_resource_manager_set_used(man, false);

	ret = ttm_resource_manager_evict_all(&adev->mman.bdev, man);
	if (ret)
		return;

	mutex_lock(&mgr->m_lock);
	list_for_each_entry_safe(rsv, temp, &mgr->reservations_pending, blocks)
		kfree(rsv);

	list_for_each_entry_safe(rsv, temp, &mgr->reserved_pages, blocks) {
		lg_drm_buddy_free_list(&mgr->b_mm, &rsv->allocated, 0);
		kfree(rsv);
	}
	lg_buddy_fini(&mgr->b_mm);
	mutex_unlock(&mgr->m_lock);

	ttm_resource_manager_cleanup(man);
	ttm_set_driver_manager(&adev->mman.bdev, TTM_PL_VRAM, NULL);
}

static inline struct lg_buddy_block *
loonggpu_vram_mgr_first_block(struct list_head *list)
{
	return list_first_entry_or_null(list, struct lg_buddy_block, link);
}

static inline bool
loonggpu_is_vram_mgr_blocks_contiguous(struct list_head *head)
{
	struct lg_buddy_block *block;
	u64 start, size;

	block = loonggpu_vram_mgr_first_block(head);
	if (!block)
		return false;

	while (head != block->link.next) {
		start = loonggpu_vram_mgr_block_start(block);
		size = loonggpu_vram_mgr_block_size(block);

		block = list_entry(block->link.next, struct lg_buddy_block, link);
		if (start + size != loonggpu_vram_mgr_block_start(block))
			return false;
	}

	return true;
}

/**
 * loonggpu_vram_mgr_new_buddy - allocate new ranges
 *
 * @man: TTM memory type manager
 * @tbo: TTM BO we need this range for
 * @place: placement flags and restrictions
 * @res: the resulting mem object
 *
 * Allocate VRAM for the given BO.
 */
int loonggpu_vram_mgr_new_buddy(struct ttm_resource_manager *man,
			     struct ttm_buffer_object *tbo,
			     const struct ttm_place *place,
			     struct ttm_resource **res)
{
	u64 vis_usage = 0, max_bytes, cur_size, min_block_size;
	struct loonggpu_vram_mgr *mgr = lg_man_to_vram_mgr(man);
	struct loonggpu_device *adev = vram_mgr_to_loonggpu_device(mgr);
	struct loonggpu_vram_mgr_resource *vres;
	u64 size, remaining_size, lpfn, fpfn;
	struct lg_buddy *mm = &mgr->b_mm;
	struct lg_buddy_block *block;
	unsigned long pages_per_block;
	int r;

	lpfn = (u64)place->lpfn << PAGE_SHIFT;
	if (!lpfn)
		lpfn = man->size;

	fpfn = (u64)place->fpfn << PAGE_SHIFT;

	max_bytes = adev->gmc.mc_vram_size;
	if (tbo->type != ttm_bo_type_kernel)
		max_bytes -= LOONGGPU_VA_RESERVED_SIZE;

	if (place->flags & TTM_PL_FLAG_CONTIGUOUS ||
	    loonggpu_vram_page_split == -1) {
		pages_per_block = ~0ul;
	} else {
		pages_per_block = max((uint32_t)loonggpu_vram_page_split,
				     tbo->page_alignment);
	}

	vres = kzalloc(sizeof(*vres), GFP_KERNEL);
	if (!vres)
		return -ENOMEM;

	ttm_resource_init(tbo, place, &vres->base);

	/* bail out quickly if there's likely not enough VRAM for this BO */

	if (ttm_resource_manager_usage(man) > max_bytes) {
		r = -ENOSPC; /* No space left on device */
		goto error_fini;
	}

	INIT_LIST_HEAD(&vres->blocks);

	if (place->flags & TTM_PL_FLAG_TOPDOWN)
		vres->flags = LG_BUDDY_TOPDOWN_ALLOCATION;

	if (fpfn || lpfn != mgr->b_mm.size)
		vres->flags |= LG_BUDDY_RANGE_ALLOCATION;

	remaining_size = (u64)vres->base.size;

	mutex_lock(&mgr->m_lock);
	while (remaining_size) {
		if (tbo->page_alignment)
			min_block_size = (u64)tbo->page_alignment << PAGE_SHIFT;
		else
			min_block_size = mgr->default_page_size;

		BUG_ON(min_block_size < mm->chunk_size);

		size = remaining_size;

		if ((size >= (u64)pages_per_block << PAGE_SHIFT) &&
		    !(size & (((u64)pages_per_block << PAGE_SHIFT) - 1)))
			min_block_size = (u64)pages_per_block << PAGE_SHIFT;

		cur_size = size;

		if (fpfn + size != (u64)place->lpfn << PAGE_SHIFT) {
			/*
			 * Except for actual range allocation, modify the size and
			 * min_block_size conforming to continuous flag enablement
			 */
			if (place->flags & TTM_PL_FLAG_CONTIGUOUS) {
				size = roundup_pow_of_two(size);
				min_block_size = size;
			/*
			 * Modify the size value if size is not
			 * aligned with min_block_size
			 */
			} else if (!IS_ALIGNED(size, min_block_size)) {
				size = round_up(size, min_block_size);
			}
		}

		r = lg_buddy_alloc_blocks(mm, fpfn,
					   lpfn,
					   size,
					   min_block_size,
					   &vres->blocks,
					   vres->flags);
		if (unlikely(r))
			goto error_free_blocks;

		if (size > remaining_size)
			remaining_size = 0;
		else
			remaining_size -= size;
	}
	mutex_unlock(&mgr->m_lock);

	if (cur_size != size) {
		struct lg_buddy_block *block;
		struct list_head *trim_list;
		u64 original_size;
		LIST_HEAD(temp);

		trim_list = &vres->blocks;
		original_size = (u64)vres->base.size;

		/*
		 * If size value is rounded up to min_block_size, trim the last
		 * block to the required size
		 */
		if (!list_is_singular(&vres->blocks)) {
			block = list_last_entry(&vres->blocks, typeof(*block), link);
			list_move_tail(&block->link, &temp);
			trim_list = &temp;
			/*
			 * Compute the original_size value by subtracting the
			 * last block size with (aligned size - original size)
			 */
			original_size = loonggpu_vram_mgr_block_size(block) - (size - cur_size);
		}

		mutex_lock(&mgr->m_lock);
		lg_drm_buddy_block_trim(mm, NULL, original_size, trim_list);
		mutex_unlock(&mgr->m_lock);

		if (!list_empty(&temp))
			list_splice_tail(trim_list, &vres->blocks);
	}

	vres->base.start = 0;
	list_for_each_entry(block, &vres->blocks, link) {
		unsigned long start;

		start = loonggpu_vram_mgr_block_start(block) +
			loonggpu_vram_mgr_block_size(block);
		start >>= PAGE_SHIFT;

		if (start > PFN_UP(vres->base.size))
			start -= PFN_UP(vres->base.size);
		else
			start = 0;
		vres->base.start = max(vres->base.start, start);

		vis_usage += loonggpu_vram_mgr_vis_size(adev, block);
	}

	if (loonggpu_is_vram_mgr_blocks_contiguous(&vres->blocks))
		vres->base.placement |= TTM_PL_FLAG_CONTIGUOUS;

	atomic64_add(vis_usage, &mgr->vis_usage);
	*res = &vres->base;
	return 0;

error_free_blocks:
	lg_drm_buddy_free_list(mm, &vres->blocks, 0);
	mutex_unlock(&mgr->m_lock);
error_fini:
	ttm_resource_fini(man, &vres->base);
	kfree(vres);

	return r;
}

/* Commit the reservation of VRAM pages */
static void loonggpu_vram_mgr_do_reserve(struct ttm_resource_manager *man)
{
	struct loonggpu_vram_mgr *mgr = lg_man_to_vram_mgr(man);
	struct loonggpu_device *adev = vram_mgr_to_loonggpu_device(mgr);
	struct loonggpu_vram_reservation *rsv, *tmp;
	struct lg_buddy *mm = &mgr->b_mm;
	struct lg_buddy_block *block;
	uint64_t vis_usage;
	int ret;

	list_for_each_entry_safe(rsv, tmp, &mgr->reservations_pending, blocks) {
		ret = lg_buddy_alloc_blocks(mm, rsv->start,
					     rsv->start + rsv->size,
					     rsv->size, mm->chunk_size,
					     &rsv->allocated,
					     LG_BUDDY_RANGE_ALLOCATION);
		if (ret)
			continue;

		block = loonggpu_vram_mgr_first_block(&rsv->allocated);
		if (!block)
			continue;

		dev_dbg(adev->dev, "Reservation 0x%llx - %lld, Succeeded\n",
			rsv->start, rsv->size);

		vis_usage = loonggpu_vram_mgr_vis_size(adev, block);
		atomic64_add(vis_usage, &mgr->vis_usage);

		spin_lock(&man->bdev->lru_lock);
		man->usage += rsv->size;
		spin_unlock(&man->bdev->lru_lock);

		list_move(&rsv->blocks, &mgr->reserved_pages);
	}
}

/**
 * loonggpu_vram_mgr_del_buddy - free ranges
 *
 * @man: TTM memory type manager
 * @tbo: TTM BO we need this range for
 * @place: placement flags and restrictions
 * @mem: TTM memory object
 *
 * Free the allocated VRAM again.
 */
void loonggpu_vram_mgr_del_buddy(struct ttm_resource_manager *man,
			      struct ttm_resource *res)
{
	struct loonggpu_vram_mgr_resource *vres = to_loonggpu_vram_mgr_resource(res);
	struct loonggpu_vram_mgr *mgr = lg_man_to_vram_mgr(man);
	struct loonggpu_device *adev = vram_mgr_to_loonggpu_device(mgr);
	struct lg_buddy *mm = &mgr->b_mm;
	struct lg_buddy_block *block;
	uint64_t vis_usage = 0;

	mutex_lock(&mgr->m_lock);
	list_for_each_entry(block, &vres->blocks, link)
		vis_usage += loonggpu_vram_mgr_vis_size(adev, block);

	loonggpu_vram_mgr_do_reserve(man);

	lg_drm_buddy_free_list(mm, &vres->blocks, vres->flags);
	mutex_unlock(&mgr->m_lock);

	atomic64_sub(vis_usage, &mgr->vis_usage);

	ttm_resource_fini(man, res);
	kfree(vres);
}

/**
 * loonggpu_vram_mgr_debug - dump VRAM table
 *
 * @man: TTM memory type manager
 * @printer: DRM printer to use
 *
 * Dump the table content using printk.
 */
void loonggpu_vram_mgr_debug_buddy(struct ttm_resource_manager *man,
			  	struct drm_printer *printer)
{
	struct loonggpu_vram_mgr *mgr = lg_man_to_vram_mgr(man);
	struct lg_buddy *mm = &mgr->b_mm;

	mutex_lock(&mgr->m_lock);
	drm_buddy_print(mm, printer);
	mutex_unlock(&mgr->m_lock);

	drm_printf(printer, "man size:%llu pages, ram usage:%lluMB, vis usage:%lluMB\n",
		   (man->size)>>PAGE_SHIFT, loonggpu_vram_mgr_usage(man) >> 20,
		   loonggpu_vram_mgr_vis_usage(man) >> 20);
}

/**
 * loonggpu_vram_mgr_intersects - test each drm buddy block for intersection
 *
 * @man: TTM memory type manager
 * @res: The resource to test
 * @place: The place to test against
 * @size: Size of the new allocation
 *
 * Test each drm buddy block for intersection for eviction decision.
 */
bool loonggpu_vram_mgr_intersects(struct ttm_resource_manager *man,
				struct ttm_resource *res,
				const struct ttm_place *place,
				size_t size)
{
	struct loonggpu_vram_mgr_resource *mgr = to_loonggpu_vram_mgr_resource(res);
	struct lg_buddy_block *block;

	/* Check each drm buddy block individually */
	list_for_each_entry(block, &mgr->blocks, link) {
		unsigned long fpfn =
			loonggpu_vram_mgr_block_start(block) >> PAGE_SHIFT;
		unsigned long lpfn = fpfn +
			(loonggpu_vram_mgr_block_size(block) >> PAGE_SHIFT);

		if (place->fpfn < lpfn &&
		    (!place->lpfn || place->lpfn > fpfn))
			return true;
	}

	return false;
}

/**
 * loonggpu_vram_mgr_compatible - test each drm buddy block for compatibility
 *
 * @man: TTM memory type manager
 * @res: The resource to test
 * @place: The place to test against
 * @size: Size of the new allocation
 *
 * Test each drm buddy block for placement compatibility.
 */
bool loonggpu_vram_mgr_compatible(struct ttm_resource_manager *man,
				struct ttm_resource *res,
				const struct ttm_place *place,
				size_t size)
{
	struct loonggpu_vram_mgr_resource *mgr = to_loonggpu_vram_mgr_resource(res);
	struct lg_buddy_block *block;

	/* Check each drm buddy block individually */
	list_for_each_entry(block, &mgr->blocks, link) {
		unsigned long fpfn =
			loonggpu_vram_mgr_block_start(block) >> PAGE_SHIFT;
		unsigned long lpfn = fpfn +
			(loonggpu_vram_mgr_block_size(block) >> PAGE_SHIFT);

		if (fpfn < place->fpfn ||
		    (place->lpfn && lpfn > place->lpfn))
			return false;
	}

	return true;
}
