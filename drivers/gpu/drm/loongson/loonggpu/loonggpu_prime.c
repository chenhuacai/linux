#include "loonggpu.h"
#include "loonggpu_display.h"
#include "loonggpu_shared.h"
#include "loonggpu_drm.h"
#include <linux/dma-buf.h>
#include <linux/dma-fence-array.h>
#include "loonggpu_helper.h"
#include "loonggpu_bo_pin_helper.h"

/**
 * loonggpu_gem_prime_get_sg_table - &drm_driver.gem_prime_get_sg_table
 * implementation
 * @obj: GEM buffer object
 *
 * Returns:
 * A scatter/gather table for the pinned pages of the buffer object's memory.
 */
struct sg_table *loonggpu_gem_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct loonggpu_bo *bo = gem_to_loonggpu_bo(obj);
	int npages = lg_tbo_to_num_pages(&bo->tbo);

	return lg_drm_prime_pages_to_sg(obj->dev, bo->tbo.ttm->pages, npages);
}

/**
 * loonggpu_gem_prime_vmap - &dma_buf_ops.vmap implementation
 * @obj: GEM buffer object
 *
 * Sets up an in-kernel virtual mapping of the buffer object's memory.
 *
 * Returns:
 * The virtual address of the mapping or an error pointer.
 */
void *loonggpu_gem_prime_vmap(struct drm_gem_object *obj)
{
	struct loonggpu_bo *bo = gem_to_loonggpu_bo(obj);
	int ret;

	ret = ttm_bo_kmap(&bo->tbo, 0, lg_tbo_to_num_pages(&bo->tbo),
			  &bo->dma_buf_vmap);
	if (ret)
		return ERR_PTR(ret);

	return bo->dma_buf_vmap.virtual;
}

/**
 * loonggpu_gem_prime_vunmap - &dma_buf_ops.vunmap implementation
 * @obj: GEM buffer object
 * @vaddr: virtual address (unused)
 *
 * Tears down the in-kernel virtual mapping of the buffer object's memory.
 */
void loonggpu_gem_prime_vunmap(struct drm_gem_object *obj, void *vaddr)
{
	struct loonggpu_bo *bo = gem_to_loonggpu_bo(obj);

	ttm_bo_kunmap(&bo->dma_buf_vmap);
}

/**
 * loonggpu_gem_prime_import_sg_table - &drm_driver.gem_prime_import_sg_table
 * implementation
 * @dev: DRM device
 * @attach: DMA-buf attachment
 * @sg: Scatter/gather table
 *
 * Import shared DMA buffer memory exported by another device.
 *
 * Returns:
 * A new GEM buffer object of the given DRM device, representing the memory
 * described by the given DMA-buf attachment and scatter/gather table.
 */
struct drm_gem_object *
loonggpu_gem_prime_import_sg_table(struct drm_device *dev,
				 struct dma_buf_attachment *attach,
				 struct sg_table *sg)
{
	lg_dma_resv_t *resv = attach->dmabuf->resv;
	struct loonggpu_device *adev = dev->dev_private;
	struct drm_gem_object *gobj;
	struct loonggpu_bo *bo;
	int ret;

	ww_mutex_lock(&resv->lock, NULL);

	ret = loonggpu_gem_object_create(adev, attach->dmabuf->size,
						PAGE_SIZE, LOONGGPU_GEM_DOMAIN_CPU, 0,
						ttm_bo_type_sg, resv, &gobj);
	if (ret)
		goto error;

	bo = gem_to_loonggpu_bo(gobj);
	bo->tbo.sg = sg;
	bo->tbo.ttm->sg = sg;
	bo->allowed_domains = LOONGGPU_GEM_DOMAIN_GTT;
	bo->preferred_domains = LOONGGPU_GEM_DOMAIN_GTT;

	if (attach->dmabuf->ops != &loonggpu_dmabuf_ops)
		bo->prime_shared_count = 1;

	ww_mutex_unlock(&resv->lock);

	return gobj;
error:
	ww_mutex_unlock(&resv->lock);
	return ERR_PTR(ret);
}

static int
__lg_dma_resv_make_exclusive(lg_dma_resv_t *obj)
{
	struct dma_fence **fences;
	unsigned int __maybe_unused count, i, n = 0;
	int r;

	if (!lg_dma_resv_get_list(obj)) /* no shared fences to convert */
		return 0;

	r = lg_dma_resv_get_fences_rcu(obj, NULL, &count, &fences, 0);
	if (r)
		return r;

	/* Drop the KFD eviction fence: it is a bookkeeping fence and must not
	 * be promoted to an exclusive fence, otherwise waiting on it (e.g. in
	 * loonggpu_bo_kmap) would trigger a self-eviction.
	 */
#if !defined(LG_DMA_RESV_ADD_FENCE)
	for (i = 0; i < count; i++) {
		if (to_loonggpu_lgkcd_fence(fences[i])) {
			dma_fence_put(fences[i]);
			continue;
		}
		fences[n++] = fences[i];
	}
	count = n;
#endif
	if (count == 0) {
		/* Now that was unexpected. */
	} else if (count == 1) {
		lg_dma_resv_add_excl_fence(obj, fences[0]);
		dma_fence_put(fences[0]);
		kfree(fences);
	} else {
		struct dma_fence_array *array;

		array = lg_dma_fence_array_create(count, fences,
					       dma_fence_context_alloc(1), 0,
					       false);
		if (!array)
			goto err_fences_put;

		lg_dma_resv_add_excl_fence(obj, &array->base);
		dma_fence_put(&array->base);
	}

	return 0;

err_fences_put:
	while (count--)
		dma_fence_put(fences[count]);
	kfree(fences);
	return -ENOMEM;
}

/**
 * loonggpu_gem_map_attach - &dma_buf_ops.attach implementation
 * @dma_buf: shared DMA buffer
 * @attach: DMA-buf attachment
 *
 * Makes sure that the shared DMA buffer can be accessed by the target device.
 * For now, simply pins it to the GTT domain, where it should be accessible by
 * all DMA devices.
 *
 * Returns:
 * 0 on success or negative error code.
 */
static int loonggpu_gem_map_attach(struct dma_buf *dma_buf,
				 struct dma_buf_attachment *attach)
{
	struct drm_gem_object *obj = dma_buf->priv;
	struct loonggpu_bo *bo = gem_to_loonggpu_bo(obj);
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);
	unsigned int domains = LOONGGPU_GEM_DOMAIN_GTT;
	lg_dma_resv_t *resv = to_dma_resv(bo);
	bool __maybe_unused peer2peer = true;
	long r;

#ifdef CONFIG_VDD_LOONGSON_P2P
	if (pci_p2pdma_distance(adev->pdev, attach->dev, false) < 0)
		peer2peer = false;
#endif

	r = drm_gem_map_attach(dma_buf, attach);
	if (r)
		return r;

	r = loonggpu_bo_reserve(bo, false);
	if (unlikely(r != 0))
		goto error_detach;


	if (attach->dev->driver != adev->dev->driver) {
		/*
		 * We only create shared fences for internal use, but importers
		 * of the dmabuf rely on exclusive fences for implicitly
		 * tracking write hazards. As any of the current fences may
		 * correspond to a write, we need to convert all existing
		 * fences on the reservation object into a single exclusive
		 * fence.
		 */
		r = __lg_dma_resv_make_exclusive(resv);
		if (r)
			goto error_unreserve;
	}

#ifdef CONFIG_VDD_LOONGSON_P2P
	/* pin buffer into preferred domains */
	if (peer2peer && loonggpu_pcie_p2p)
		domains = bo->preferred_domains;
#endif
	r = loonggpu_bo_pin(bo, domains);
	if (r)
		goto error_unreserve;

	if (attach->dev->driver != adev->dev->driver)
		bo->prime_shared_count++;

error_unreserve:
	loonggpu_bo_unreserve(bo);

error_detach:
	if (r)
		drm_gem_map_detach(dma_buf, attach);
	return r;
}

/**
 * loonggpu_gem_map_detach - &dma_buf_ops.detach implementation
 * @dma_buf: shared DMA buffer
 * @attach: DMA-buf attachment
 *
 * This is called when a shared DMA buffer no longer needs to be accessible by
 * the other device. For now, simply unpins the buffer from GTT.
 */
static void loonggpu_gem_map_detach(struct dma_buf *dma_buf,
				  struct dma_buf_attachment *attach)
{
	struct drm_gem_object *obj = dma_buf->priv;
	struct loonggpu_bo *bo = gem_to_loonggpu_bo(obj);
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);
	int ret = 0;

	ret = loonggpu_bo_reserve(bo, true);
	if (unlikely(ret != 0))
		goto error;

	loonggpu_bo_unpin(bo);
	if (attach->dev->driver != adev->dev->driver && bo->prime_shared_count)
		bo->prime_shared_count--;
	loonggpu_bo_unreserve(bo);

error:
	drm_gem_map_detach(dma_buf, attach);
}

#ifdef CONFIG_VDD_LOONGSON_P2P
/**
 * loonggpu_gem_map_dma_buf - &dma_buf_ops.map_dma_buf implementation
 * @attach: DMA-buf attachment
 * @dir: DMA direction
 *
 * Makes sure that the shared DMA buffer can be accessed by the target device.
 * If peer-to-peer is possible, the buffer is kept in VRAM and exported with
 * PCIe bus addresses, otherwise it is moved to the GTT domain.
 *
 * Returns:
 * sg_table filled with the DMA addresses to use or ERR_PTR with negative
 * error code.
 */
static struct sg_table *loonggpu_gem_map_dma_buf(
	struct dma_buf_attachment *attach, enum dma_data_direction dir)
{
	struct dma_buf *dma_buf = attach->dmabuf;
	struct drm_gem_object *obj = dma_buf->priv;
	struct loonggpu_bo *bo = gem_to_loonggpu_bo(obj);
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);
	struct sg_table *sgt;
	long r;

	switch (lg_get_bo_mem_type(bo)) {
	case TTM_PL_TT:
		sgt = lg_drm_prime_pages_to_sg(obj->dev, bo->tbo.ttm->pages,
						lg_tbo_to_num_pages(&bo->tbo));
		if (IS_ERR(sgt))
			return sgt;

		if (dma_map_sgtable(attach->dev, sgt, dir,
				    DMA_ATTR_SKIP_CPU_SYNC))
			goto error_free;
		break;

	case TTM_PL_VRAM:
		r = loonggpu_vram_mgr_alloc_sgt(adev, bo->tbo.resource, 0,
					loonggpu_bo_size(bo), attach->dev,
					dir, &sgt);
		if (r)
			return ERR_PTR(r);
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	return sgt;

error_free:
	sg_free_table(sgt);
	kfree(sgt);
	return ERR_PTR(-EBUSY);
}

/**
 * loonggpu_gem_unmap_dma_buf - &dma_buf_ops.unmap_dma_buf implementation
 * @attach: DMA-buf attachment
 * @sgt: sg_table to unmap
 * @dir: DMA direction
 */
static void loonggpu_gem_unmap_dma_buf(struct dma_buf_attachment *attach,
				       struct sg_table *sgt,
				       enum dma_data_direction dir)
{
	if (sgt->sgl->page_link) {
		dma_unmap_sgtable(attach->dev, sgt, dir, 0);
		sg_free_table(sgt);
		kfree(sgt);
	} else {
		loonggpu_vram_mgr_free_sgt(attach->dev, dir, sgt);
	}
}
#endif

/**
 * loonggpu_gem_begin_cpu_access - &dma_buf_ops.begin_cpu_access implementation
 * @dma_buf: shared DMA buffer
 * @direction: direction of DMA transfer
 *
 * This is called before CPU access to the shared DMA buffer's memory. If it's
 * a read access, the buffer is moved to the GTT domain if possible, for optimal
 * CPU read performance.
 *
 * Returns:
 * 0 on success or negative error code.
 */
static int loonggpu_gem_begin_cpu_access(struct dma_buf *dma_buf,
				       enum dma_data_direction direction)
{
	struct loonggpu_bo *bo = gem_to_loonggpu_bo(dma_buf->priv);
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);
	struct ttm_operation_ctx ctx = { true, false };
	u32 domain = loonggpu_display_supported_domains(adev);
	int ret;
	bool reads = (direction == DMA_BIDIRECTIONAL ||
		      direction == DMA_FROM_DEVICE);

	if (!reads || !(domain & LOONGGPU_GEM_DOMAIN_GTT))
		return 0;

	/* move to gtt */
	ret = loonggpu_bo_reserve(bo, false);
	if (unlikely(ret != 0))
		return ret;

	if (!lg_bo_pin_count(bo) && (bo->allowed_domains & LOONGGPU_GEM_DOMAIN_GTT)) {
		loonggpu_bo_placement_from_domain(bo, LOONGGPU_GEM_DOMAIN_GTT);
		ret = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
	}

	loonggpu_bo_unreserve(bo);
	return ret;
}

const struct dma_buf_ops loonggpu_dmabuf_ops = {
	.attach = loonggpu_gem_map_attach,
	.detach = loonggpu_gem_map_detach,
#ifdef CONFIG_VDD_LOONGSON_P2P
	.map_dma_buf = loonggpu_gem_map_dma_buf,
	.unmap_dma_buf = loonggpu_gem_unmap_dma_buf,
#else
	.map_dma_buf = drm_gem_map_dma_buf,
	.unmap_dma_buf = drm_gem_unmap_dma_buf,
#endif
	.release = drm_gem_dmabuf_release,
	.begin_cpu_access = loonggpu_gem_begin_cpu_access,
#if defined(LG_DRM_GEM_DMABUF_KMAP_PRESENT)
	.map = drm_gem_dmabuf_kmap,
	.unmap = drm_gem_dmabuf_kunmap,
#endif
	.mmap = drm_gem_dmabuf_mmap,
	.vmap = drm_gem_dmabuf_vmap,
	.vunmap = drm_gem_dmabuf_vunmap,
};

/**
 * loonggpu_gem_prime_export - &drm_driver.gem_prime_export implementation
 * @dev: DRM device
 * @gobj: GEM buffer object
 * @flags: flags like DRM_CLOEXEC and DRM_RDWR
 *
 * The main work is done by the &drm_gem_prime_export helper, which in turn
 * uses &loonggpu_gem_prime_res_obj.
 *
 * Returns:
 * Shared DMA buffer representing the GEM buffer object from the given device.
 */
struct dma_buf *loonggpu_gem_prime_export(struct drm_gem_object *gobj, int flags)
{
	struct loonggpu_bo *bo = gem_to_loonggpu_bo(gobj);
	struct dma_buf *buf;

	if (loonggpu_ttm_tt_get_usermm(bo->tbo.ttm) ||
	    bo->flags & LOONGGPU_GEM_CREATE_VM_ALWAYS_VALID)
		return ERR_PTR(-EPERM);

	buf = lg_drm_gem_prime_export(gobj, flags);
	if (!IS_ERR(buf)) {
		buf->file->f_mapping = gobj->dev->anon_inode->i_mapping;
		buf->ops = &loonggpu_dmabuf_ops;
	}

	return buf;
}

/**
 * loonggpu_gem_prime_import - &drm_driver.gem_prime_import implementation
 * @dev: DRM device
 * @dma_buf: Shared DMA buffer
 *
 * The main work is done by the &drm_gem_prime_import helper, which in turn
 * uses &loonggpu_gem_prime_import_sg_table.
 *
 * Returns:
 * GEM buffer object representing the shared DMA buffer for the given device.
 */
struct drm_gem_object *loonggpu_gem_prime_import(struct drm_device *dev,
					    struct dma_buf *dma_buf)
{
	struct drm_gem_object *obj;

	if (dma_buf->ops == &loonggpu_dmabuf_ops) {
		obj = dma_buf->priv;
		if (obj->dev == dev) {
			/*
			 * Importing dmabuf exported from out own gem increases
			 * refcount on gem itself instead of f_count of dmabuf.
			 */
			drm_gem_object_get(obj);
			return obj;
		}
	}

	return drm_gem_prime_import(dev, dma_buf);
}
