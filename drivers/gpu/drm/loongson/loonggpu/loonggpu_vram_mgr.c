#include "loonggpu.h"
#include "loonggpu_vram_mgr_helper.h"

/**
 * loonggpu_vram_mgr_init - init VRAM manager and DRM MM
 *
 * @man: TTM memory type manager
 * @p_size: maximum size of VRAM
 *
 * Allocate and initialize the VRAM manager.
 */
int loonggpu_vram_mgr_init(lg_ttm_manager_t *man,
				unsigned long p_size)
{
	int r;
	struct loonggpu_vram_mgr *mgr;
	struct loonggpu_device *adev;

	mgr = container_of(man, struct loonggpu_vram_mgr, manager);
	adev = vram_mgr_to_loonggpu_device(mgr);

	lg_vram_init_mgr(mgr);
	r = lg_buddy_init(&mgr->b_mm, p_size, PAGE_SIZE);
	if (r)
		return r;
	lg_vram_mgr_init_drv_ptr_used(man, mgr);
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
int loonggpu_vram_mgr_fini(lg_ttm_manager_t *man)
{
	struct loonggpu_vram_mgr *mgr = lg_man_to_vram_mgr(man);
	int ret;

	ret = lg_vram_mgr_fini_clean(man, mgr);
	if (ret)
		return ret;

	lg_vram_mm_fini(mgr);
	lg_vram_mgr_fini_free(man, mgr);
	return 0;
}

/**
 * loonggpu_vram_mgr_vis_size - Calculate visible node size
 *
 * @adev: loonggpu device structure
 * @node: MM node structure
 *
 * Calculate how many bytes of the MM node are inside visible VRAM
 */
u64 loonggpu_vram_mgr_vis_size(struct loonggpu_device *adev,
				   struct lg_buddy_block *block)
{
	uint64_t start = loonggpu_vram_mgr_block_start(block);
	uint64_t end = start + loonggpu_vram_mgr_block_size(block);

	if (start >= adev->gmc.visible_vram_size)
		return 0;

	return (end > adev->gmc.visible_vram_size ?
		adev->gmc.visible_vram_size : end) - start;
}

/**
 * loonggpu_vram_mgr_bo_visible_size - CPU visible BO size
 *
 * @bo: &loonggpu_bo buffer object (must be in VRAM)
 *
 * Returns:
 * How much of the given &loonggpu_bo buffer object lies in CPU visible VRAM.
 */
u64 loonggpu_vram_mgr_bo_visible_size(struct loonggpu_bo *bo)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);
	lg_ttm_mem_t *mem = lg_bo_to_mem;
	struct loonggpu_vram_mgr_resource *vres = to_loonggpu_vram_mgr_resource(mem);
	struct lg_buddy_block *block;
	u64 usage;

	if (loonggpu_gmc_vram_full_visible(&adev->gmc))
		return loonggpu_bo_size(bo);

	if (mem->start >= adev->gmc.visible_vram_size >> PAGE_SHIFT)
		return 0;

	list_for_each_entry(block, &vres->blocks, link)
		usage += loonggpu_vram_mgr_vis_size(adev, block);

	return usage;
}

/**
 * loonggpu_vram_mgr_usage - how many bytes are used in this domain
 *
 * @man: TTM memory type manager
 *
 * Returns how many bytes are used in this domain.
 */
uint64_t loonggpu_vram_mgr_usage(lg_ttm_manager_t *man)
{
	return ttm_resource_manager_usage(man);
}

/**
 * loonggpu_vram_mgr_vis_usage - how many bytes are used in the visible part
 *
 * @man: TTM memory type manager
 *
 * Returns how many bytes are used in the visible part of VRAM
 */
uint64_t loonggpu_vram_mgr_vis_usage(lg_ttm_manager_t *man)
{
	struct loonggpu_vram_mgr *mgr = lg_man_to_vram_mgr(man);

	return atomic64_read(&mgr->vis_usage);
}

#ifdef CONFIG_VDD_LOONGSON_P2P
/**
 * loonggpu_vram_mgr_alloc_sgt - allocate and fill a sg table
 *
 * @adev: loonggpu device pointer
 * @res: TTM memory object
 * @offset: byte offset from the base of VRAM BO
 * @length: number of bytes to export in sg_table
 * @dev: the other device
 * @dir: dma direction
 * @sgt: resulting sg table
 *
 * Allocate and fill a sg table from a VRAM allocation.
 */
int loonggpu_vram_mgr_alloc_sgt(struct loonggpu_device *adev,
				struct ttm_resource *res,
				u64 offset, u64 length,
				struct device *dev,
				enum dma_data_direction dir,
				struct sg_table **sgt)
{
	struct loonggpu_res_cursor cursor;
	struct scatterlist *sg;
	int num_entries = 0;
	int i, r;

	*sgt = kmalloc(sizeof(**sgt), GFP_KERNEL);
	if (!*sgt)
		return -ENOMEM;

	/* Determine the number of VRAM blocks to export */
	loonggpu_res_first(res, offset, length, &cursor);
	while (cursor.remaining) {
		num_entries++;
		loonggpu_res_next(&cursor, cursor.size);
	}

	r = sg_alloc_table(*sgt, num_entries, GFP_KERNEL);
	if (r)
		goto error_free;

	/* Initialize scatterlist nodes of sg_table */
	for_each_sgtable_sg((*sgt), sg, i)
		sg->length = 0;

	/*
	 * Walk down VRAM blocks to populate scatterlist nodes
	 * @note: Use iterator api to get first the VRAM block
	 * and the number of bytes from it. Access the following
	 * VRAM block(s) if more buffer needs to exported
	 */
	loonggpu_res_first(res, offset, length, &cursor);
	for_each_sgtable_sg((*sgt), sg, i) {
		/* FIXME: The PCIe switch needs to erase the node number
		 * bit[40-47]
		 */
		phys_addr_t phys = cursor.start + (adev->gmc.aper_base &
		0xffffffffffull);
		size_t size = cursor.size;
		dma_addr_t addr;

		addr = dma_map_resource(dev, phys, size, dir,
					DMA_ATTR_SKIP_CPU_SYNC);
		r = dma_mapping_error(dev, addr);
		if (r)
			goto error_unmap;

		sg_set_page(sg, NULL, size, 0);
		sg_dma_address(sg) = addr;
		sg_dma_len(sg) = size;

		loonggpu_res_next(&cursor, cursor.size);
	}

	return 0;

error_unmap:
	for_each_sgtable_sg((*sgt), sg, i) {
		if (!sg->length)
			continue;

		dma_unmap_resource(dev, sg->dma_address,
				   sg->length, dir,
				   DMA_ATTR_SKIP_CPU_SYNC);
	}
	sg_free_table(*sgt);

error_free:
	kfree(*sgt);
	return r;
}

/**
 * loonggpu_vram_mgr_free_sgt - free a previously allocated sg table
 *
 * @dev: device pointer
 * @dir: data direction of resource to unmap
 * @sgt: sg table to free
 */
void loonggpu_vram_mgr_free_sgt(struct device *dev,
				enum dma_data_direction dir,
				struct sg_table *sgt)
{
	struct scatterlist *sg;
	int i;

	for_each_sgtable_sg(sgt, sg, i)
		dma_unmap_resource(dev, sg->dma_address,
				   sg->length, dir,
				   DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(sgt);
	kfree(sgt);
}
#endif

const lg_ttm_mem_func_t loonggpu_vram_mgr_func = {
	.alloc = loonggpu_vram_mgr_new_buddy,
	.free  = loonggpu_vram_mgr_del_buddy,
	.intersects = loonggpu_vram_mgr_intersects,
	.compatible = loonggpu_vram_mgr_compatible,
	.debug = loonggpu_vram_mgr_debug_buddy
};
