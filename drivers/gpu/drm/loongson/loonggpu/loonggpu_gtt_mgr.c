#include "loonggpu.h"
#include "loonggpu_gtt_mgr_helper.h"

/**
 * loonggpu_gtt_mgr_init - init GTT manager and DRM MM
 *
 * @man: TTM memory type manager
 * @p_size: maximum size of GTT
 *
 * Allocate and initialize the GTT manager.
 */
int loonggpu_gtt_mgr_init(lg_ttm_manager_t *man,
			       unsigned long p_size)
{
	struct loonggpu_device *adev;
	struct loonggpu_gtt_mgr *mgr;
	uint64_t start, size;

	lg_gtt_mgr_init_mgr(man, mgr);

        adev = lg_gtt_mgr_man_to_adev(man, mgr);
	lg_gtt_mgr_func_setting(man, adev, &loonggpu_gtt_mgr_func, p_size);
	start = LOONGGPU_GTT_MAX_TRANSFER_SIZE * LOONGGPU_GTT_NUM_TRANSFER_WINDOWS;
	size = (adev->gmc.gart_size >> PAGE_SHIFT) - start;
	drm_mm_init(&mgr->mm, start, size);
	spin_lock_init(&mgr->lock);
	atomic64_set(&mgr->available, p_size);
	lg_gtt_mgr_init_set_man_and_used(man, &adev->mman.bdev);
	return 0;
}

/**
 * loonggpu_gtt_mgr_fini - free and destroy GTT manager
 *
 * @man: TTM memory type manager
 *
 * Destroy and free the GTT manager, returns -EBUSY if ranges are still
 * allocated inside it.
 */
int loonggpu_gtt_mgr_fini(lg_ttm_manager_t *man)
{
	int ret;
	struct loonggpu_gtt_mgr *mgr = lg_man_to_gtt_mgr(man);
	struct loonggpu_device *adev = lg_gtt_mgr_man_to_adev(man, mgr);

	ret = lg_gtt_mgr_fini_set_used_clean_list(man, &adev->mman.bdev);
	if (ret)
		return ret;

	spin_lock(&mgr->lock);
	drm_mm_takedown(&mgr->mm);
	spin_unlock(&mgr->lock);
	lg_gtt_mgr_fini_clean_up(man, &adev->mman.bdev);
	return 0;
}

/**
 * loonggpu_gtt_mgr_has_gart_addr - Check if mem has address space
 *
 * @mem: the mem object to check
 *
 * Check if a mem object has already address space allocated.
 */
bool loonggpu_gtt_mgr_has_gart_addr(lg_ttm_mem_t *mem)
{
	if (mem->mem_type != TTM_PL_TT)
		return true;

	struct ttm_range_mgr_node *node = to_ttm_range_mgr_node(mem);

        return drm_mm_node_allocated(&node->mm_nodes[0]);
}

/**
 * loonggpu_gtt_mgr_alloc - allocate new ranges
 *
 * @man: TTM memory type manager
 * @tbo: TTM BO we need this range for
 * @place: placement flags and restrictions
 * @mem: the resulting mem object
 *
 * Allocate the address space for a node.
 */
static int loonggpu_gtt_mgr_alloc(lg_ttm_manager_t *man,
				struct ttm_buffer_object *tbo,
				const struct ttm_place *place,
				lg_ttm_mem_t *mem)
{
	struct loonggpu_gtt_mgr *mgr = lg_man_to_gtt_mgr(man);
	struct loonggpu_device *adev = lg_gtt_mgr_man_to_adev(man, mgr);
	lg_loonggpu_gtt_node *node = lg_res_to_gtt_node(mem);
	enum drm_mm_insert_mode mode;
	unsigned long fpfn, lpfn;
	int r;

	if (loonggpu_gtt_mgr_has_gart_addr(mem))
		return 0;

	if (place)
		fpfn = place->fpfn;
	else
		fpfn = 0;

	if (place && place->lpfn)
		lpfn = place->lpfn;
	else
		lpfn = adev->gart.num_cpu_pages;

	mode = DRM_MM_INSERT_BEST;
	if (place && place->flags & TTM_PL_FLAG_TOPDOWN)
		mode = DRM_MM_INSERT_HIGH;

	spin_lock(&mgr->lock);
	r = drm_mm_insert_node_in_range(&mgr->mm, lg_gtt_node_to_drm_node(node), lg_gtt_get_pages(tbo, mem),
					tbo->page_alignment, 0, fpfn, lpfn, mode);
	spin_unlock(&mgr->lock);

	if (!r)
		mem->start = lg_gtt_node_to_drm_node(node)->start;

	return r;
}

/**
 * loonggpu_gtt_mgr_new - allocate a new node
 *
 * @man: TTM memory type manager
 * @tbo: TTM BO we need this range for
 * @place: placement flags and restrictions
 * @mem: the resulting mem object
 *
 * Dummy, allocate the node but no space for it yet.
 */
static int loonggpu_gtt_mgr_new(lg_ttm_manager_t *man,
			      struct ttm_buffer_object *tbo,
			      const struct ttm_place *place,
			      lg_gtt_mgr_new_mem_reg)
{
	struct loonggpu_gtt_mgr *mgr = container_of(man, struct loonggpu_gtt_mgr, manager);
	uint32_t num_pages = PFN_UP(tbo->base.size);
	struct ttm_range_mgr_node *node;
	int r;

	node = kzalloc(struct_size(node, mm_nodes, 1), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	ttm_resource_init(tbo, place, &node->base);
	if (!(place->flags & TTM_PL_FLAG_TEMPORARY) &&
	   ttm_resource_manager_usage(man) > man->size) {
		r = -ENOSPC;
		goto err_free;
	}

	if (place->lpfn) {
		spin_lock(&mgr->lock);
		r = drm_mm_insert_node_in_range(&mgr->mm, &node->mm_nodes[0],
						num_pages, tbo->page_alignment,
						0, place->fpfn, place->lpfn,
						DRM_MM_INSERT_BEST);
		spin_unlock(&mgr->lock);
		if (unlikely(r))
			goto err_free;

		node->base.start = node->mm_nodes[0].start;
	} else {
		node->mm_nodes[0].start = 0;
		node->mm_nodes[0].size = PFN_UP(node->base.size);
		node->base.start = LOONGGPU_BO_INVALID_OFFSET;
	}

	*mem = &node->base;
	return 0;

err_free:
	ttm_resource_fini(man, &node->base);
	kfree(node);
	return r;
}

/**
 * loonggpu_gtt_mgr_del - free ranges
 *
 * @man: TTM memory type manager
 * @tbo: TTM BO we need this range for
 * @place: placement flags and restrictions
 * @mem: TTM memory object
 *
 * Free the allocated GTT again.
 */
static void loonggpu_gtt_mgr_del(lg_ttm_manager_t *man,
			       lg_ttm_mem_t *mem)
{
	struct loonggpu_gtt_mgr *mgr = lg_man_to_gtt_mgr(man);
	lg_loonggpu_gtt_node *node = lg_res_to_gtt_node(mem);
	struct drm_mm_node __maybe_unused *mm_node;

	if (!node)
		return;
	spin_lock(&mgr->lock);
	if (drm_mm_node_allocated(&node->mm_nodes[0]))
		drm_mm_remove_node(&node->mm_nodes[0]);
	spin_unlock(&mgr->lock);

	ttm_resource_fini(man, mem);
	kfree(node);
}

/**
 * loonggpu_gtt_mgr_usage - return usage of GTT domain
 *
 * @man: TTM memory type manager
 *
 * Return how many bytes are used in the GTT domain
 */
uint64_t loonggpu_gtt_mgr_usage(lg_ttm_manager_t *man)
{
	return ttm_resource_manager_usage(man);
}

int loonggpu_gtt_mgr_recover(lg_ttm_manager_t *man)
{
	struct loonggpu_gtt_mgr *mgr = lg_man_to_gtt_mgr(man);
	lg_loonggpu_gtt_node *node;
	struct drm_mm_node *mm_node;
	int r = 0;

	spin_lock(&mgr->lock);
	drm_mm_for_each_node(mm_node, &mgr->mm) {
		node = lg_drm_node_to_gtt_node(mm_node);
		r = loonggpu_ttm_recover_gart(lg_gtt_node_to_tbo(node));
		if (r)
			break;
	}
	spin_unlock(&mgr->lock);

	return r;
}

/**
 * loonggpu_gtt_mgr_debug - dump VRAM table
 *
 * @man: TTM memory type manager
 * @printer: DRM printer to use
 *
 * Dump the table content using printk.
 */
static void loonggpu_gtt_mgr_debug(lg_ttm_manager_t *man,
				 struct drm_printer *printer)
{
	struct loonggpu_gtt_mgr *mgr = lg_man_to_gtt_mgr(man);

	spin_lock(&mgr->lock);
	drm_mm_print(&mgr->mm, printer);
	spin_unlock(&mgr->lock);

	drm_printf(printer, "man size:%llu pages, gtt available:%lld pages, usage:%lluMB\n",
		   man->size, (u64)atomic64_read(&mgr->available),
		   loonggpu_gtt_mgr_usage(man) >> 20);
}

const lg_ttm_mem_func_t loonggpu_gtt_mgr_func = {
	lg_gtt_mgr_set_funcs
};
