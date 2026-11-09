#include "loonggpu.h"
#include "loonggpu_ttm_helper.h"
#include "loonggpu_gtt_mgr_helper.h"
#include <drm/ttm/ttm_bo.h>

void loonggpu_res_first(struct ttm_resource *res,
		     u64 start, u64 size,
		     struct loonggpu_res_cursor *cur)
{
	struct lg_buddy_block *block;
	struct list_head *head, *next;
	struct drm_mm_node *node;

	if (!res)
		goto fallback;

	BUG_ON(start + size > res->size);

	cur->mem_type = res->mem_type;

	switch (cur->mem_type) {
	case TTM_PL_VRAM:
		head = &to_loonggpu_vram_mgr_resource(res)->blocks;

		block = list_first_entry_or_null(head,
						 struct lg_buddy_block,
						 link);
		if (!block)
			goto fallback;

		while (start >= loonggpu_vram_mgr_block_size(block)) {
			start -= loonggpu_vram_mgr_block_size(block);

			next = block->link.next;
			if (next != head)
				block = list_entry(next, struct lg_buddy_block, link);
		}

		cur->start = loonggpu_vram_mgr_block_start(block) + start;
		cur->size = min(loonggpu_vram_mgr_block_size(block) - start, size);
		cur->remaining = size;
		cur->node = block;
		break;
	case TTM_PL_TT:
	case LOONGGPU_PL_DOORBELL:
		node = to_ttm_range_mgr_node(res)->mm_nodes;
		while (start >= node->size << PAGE_SHIFT)
			start -= node++->size << PAGE_SHIFT;

		cur->start = (node->start << PAGE_SHIFT) + start;
		cur->size = min((node->size << PAGE_SHIFT) - start, size);
		cur->remaining = size;
		cur->node = node;
		break;
	default:
		goto fallback;
	}

	return;

fallback:
	cur->start = start;
	cur->size = size;
	cur->remaining = size;
	cur->node = NULL;
	WARN_ON(res && start + size > res->size);
	return;
}

void loonggpu_res_next(struct loonggpu_res_cursor *cur, u64 size)
{
	struct lg_buddy_block *block;
	struct drm_mm_node *node;
	struct list_head *next;

	BUG_ON(size > cur->remaining);

	cur->remaining -= size;
	if (!cur->remaining)
		return;

	cur->size -= size;
	if (cur->size) {
		cur->start += size;
		return;
	}

	switch (cur->mem_type) {
	case TTM_PL_VRAM:
		block = cur->node;

		next = block->link.next;
		block = list_entry(next, struct lg_buddy_block, link);

		cur->node = block;
		cur->start = loonggpu_vram_mgr_block_start(block);
		cur->size = min(loonggpu_vram_mgr_block_size(block), cur->remaining);
		break;
	case TTM_PL_TT:
	case LOONGGPU_PL_DOORBELL:
		node = cur->node;

		cur->node = ++node;
		cur->start = node->start << PAGE_SHIFT;
		cur->size = min(node->size << PAGE_SHIFT, cur->remaining);
		break;
	default:
		return;
	}
}

unsigned long loonggpu_ttm_io_mem_pfn_buddy(struct ttm_buffer_object *bo,
					unsigned long page_offset)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->bdev);
	struct loonggpu_res_cursor cursor;

	loonggpu_res_first(bo->resource, page_offset << PAGE_SHIFT, 0, &cursor);

	return (adev->gmc.aper_base + cursor.start) >> PAGE_SHIFT;
}

static int loonggpu_ttm_map_buffer(struct ttm_buffer_object *bo,
				 struct ttm_resource *mem,
				 struct loonggpu_res_cursor *mm_cur,
				 unsigned int window, struct loonggpu_ring *ring,
				 bool tmz, u64 *size, u64 *addr)
{
	struct loonggpu_device *adev = ring->adev;
	unsigned int offset, num_pages, num_dw, num_bytes;
	u64 src_addr, dst_addr;
	struct loonggpu_job *job;
	struct dma_fence *fence;
	void *cpu_addr;
	u64 flags;
	unsigned int i;
	int r;

	BUG_ON(adev->mman.buffer_funcs->copy_max_bytes <
	       LOONGGPU_GTT_MAX_TRANSFER_SIZE * 8);

	/* Map only what can't be accessed directly */
	if (!tmz && mem->start != LOONGGPU_BO_INVALID_OFFSET) {
		*addr = loonggpu_ttm_domain_start(adev, mem->mem_type) +
			mm_cur->start;
		return 0;
	}

	/*
	 * If start begins at an offset inside the page, then adjust the size
	 * and addr accordingly
	 */
	offset = mm_cur->start & ~PAGE_MASK;

	num_pages = PFN_UP(*size + offset);
	num_pages = min_t(uint32_t, num_pages, LOONGGPU_GTT_MAX_TRANSFER_SIZE);

	*size = min(*size, (u64)num_pages * PAGE_SIZE - offset);

	*addr = adev->gmc.gart_start;
	*addr += (u64)window * LOONGGPU_GTT_MAX_TRANSFER_SIZE *
		LOONGGPU_GPU_PAGE_SIZE;
	*addr += offset;

	num_dw = ALIGN(adev->mman.buffer_funcs->copy_num_dw, 8);
	num_bytes = num_pages * 8 * LOONGGPU_GPU_PAGES_IN_CPU_PAGE;

	r = loonggpu_job_alloc_with_ib(adev, num_dw * 4 + num_bytes, &job);
	if (r)
		return r;

	src_addr = num_dw * 4;
	src_addr += job->ibs[0].gpu_addr;

	dst_addr = loonggpu_bo_gpu_offset(adev->gart.robj);
	dst_addr += window * LOONGGPU_GTT_MAX_TRANSFER_SIZE * 8;
	loonggpu_emit_copy_buffer(adev, &job->ibs[0], src_addr,
				dst_addr, num_bytes);

	loonggpu_ring_pad_ib(ring, &job->ibs[0]);
	WARN_ON(job->ibs[0].length_dw > num_dw);

	flags = loonggpu_ttm_tt_pte_flags(adev, bo->ttm, mem);

	cpu_addr = &job->ibs[0].ptr[num_dw];

	if (mem->mem_type == TTM_PL_TT) {
		dma_addr_t *dma_addr;

		dma_addr = &bo->ttm->dma_address[mm_cur->start >> PAGE_SHIFT];
		loonggpu_gart_map(adev, 0, num_pages, dma_addr, flags, cpu_addr);
	} else {
		dma_addr_t dma_address;

		dma_address = mm_cur->start;
		dma_address += adev->vm_manager.vram_base_offset;

		for (i = 0; i < num_pages; ++i) {
			loonggpu_gart_map(adev, i << PAGE_SHIFT, 1, &dma_address,
					flags, cpu_addr);
			dma_address += PAGE_SIZE;
		}
	}

	r = loonggpu_job_submit(job, &adev->mman.entity,
			     LOONGGPU_FENCE_OWNER_UNDEFINED, &fence);
	if (r)
		loonggpu_job_free(job);

	dma_fence_put(fence);

	return 0;
}

static int loonggpu_ttm_prepare_job(struct loonggpu_device *adev,
				  bool direct_submit,
				  unsigned int num_dw,
				  struct dma_resv *resv,
				  bool vm_needs_flush,
				  struct loonggpu_job **job,
				  bool delayed)
{
	int r;

	r = loonggpu_job_alloc_with_ib(adev, num_dw * 4, job);
	if (r)
		return r;

	if (!resv)
		return 0;

	return drm_sched_job_add_resv_dependencies(&(*job)->base, resv,
						   DMA_RESV_USAGE_BOOKKEEP);
}

static int loonggpu_ttm_fill_mem(struct loonggpu_ring *ring, uint32_t src_data,
			       u64 dst_addr, uint32_t byte_count,
			       struct dma_resv *resv,
			       struct dma_fence **fence,
			       bool vm_needs_flush, bool delayed)
{
	struct loonggpu_device *adev = ring->adev;
	unsigned int num_loops, num_dw;
	struct loonggpu_job *job;
	uint32_t max_bytes;
	unsigned int i;
	int r;

	max_bytes = adev->mman.buffer_funcs->fill_max_bytes;
	num_loops = DIV_ROUND_UP_ULL(byte_count, max_bytes);
	num_dw = ALIGN(num_loops * adev->mman.buffer_funcs->fill_num_dw, 8);
	r = loonggpu_ttm_prepare_job(adev, false, num_dw, resv, vm_needs_flush,
				   &job, delayed);
	if (r)
		return r;

	for (i = 0; i < num_loops; i++) {
		uint32_t cur_size = min(byte_count, max_bytes);


		loonggpu_emit_fill_buffer(adev, &job->ibs[0], src_data, dst_addr,
					cur_size);

		dst_addr += cur_size;
		byte_count -= cur_size;
	}

	loonggpu_ring_pad_ib(ring, &job->ibs[0]);
	WARN_ON(job->ibs[0].length_dw > num_dw);
	r = loonggpu_job_submit(job, &adev->mman.entity,
			     LOONGGPU_FENCE_OWNER_UNDEFINED, fence);
	if (r)
		loonggpu_job_free(job);

	return 0;
}

int loonggpu_fill_buffer_buddy(struct loonggpu_bo *bo,
			uint32_t src_data,
			struct dma_resv *resv,
			struct dma_fence **f,
			bool delayed)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);
	struct loonggpu_ring *ring = adev->mman.buffer_funcs_ring;
	struct dma_fence *fence = NULL;
	struct loonggpu_res_cursor dst;
	int r;

	if (!adev->mman.buffer_funcs_enabled) {
		DRM_ERROR("Trying to clear memory with ring turned off.\n");
		return -EINVAL;
	}

	loonggpu_res_first(bo->tbo.resource, 0, loonggpu_bo_size(bo), &dst);

	mutex_lock(&adev->mman.gtt_window_lock);
	while (dst.remaining) {
		struct dma_fence *next;
		u64 cur_size, to;

		/* Never fill more than 256MiB at once to avoid timeouts */
		cur_size = min(dst.size, 256ULL << 20);

		r = loonggpu_ttm_map_buffer(&bo->tbo, bo->tbo.resource, &dst,
					  1, ring, false, &cur_size, &to);
		if (r)
			goto error;

		r = loonggpu_ttm_fill_mem(ring, src_data, to, cur_size, resv,
					&next, true, delayed);
		if (r)
			goto error;

		dma_fence_put(fence);
		fence = next;

		loonggpu_res_next(&dst, cur_size);
	}
error:
	mutex_unlock(&adev->mman.gtt_window_lock);
	if (f)
		*f = dma_fence_get(fence);
	dma_fence_put(fence);
	return r;
}
