#include "loonggpu.h"
#include "loonggpu_drm.h"

/*
 * TODO write some description
 */

/*
 * Common zip meta table functions.
 */

/**
 * loonggpu_zip_meta_vram_alloc - allocate vram for zip meta page table
 *
 * @adev: loonggpu_device pointer
 *
 * Allocate video memory for zip meta page table
 * Returns 0 for success, error for failure.
 */
int loonggpu_zip_meta_vram_alloc(struct loonggpu_device *adev)
{
	int r;
	struct loonggpu_bo_param bp;

	if (adev->zip_meta.robj)
		return 0;

	memset(&bp, 0, sizeof(bp));
	bp.size = adev->zip_meta.table_size;
	bp.byte_align = adev->zip_meta.table_size;
	bp.domain = LOONGGPU_GEM_DOMAIN_VRAM;
	bp.flags = LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED |
		LOONGGPU_GEM_CREATE_VRAM_CONTIGUOUS;

	if (adev->family_type < CHIP_LG210)
		bp.type = ttm_bo_type_kernel;
	else
		bp.type = ttm_bo_type_device;

	bp.resv = NULL;
	r = loonggpu_bo_create(adev, &bp, &adev->zip_meta.robj);
	if (r) {
		return r;
	}

	return 0;
}

/**
 * loonggpu_zip_meta_vram_pin - pin zip meta page table in vram
 *
 * @adev: loonggpu_device pointer
 *
 * Pin the zip meta page table in vram so it will not be moved
 * Returns 0 for success, error for failure.
 */
int loonggpu_zip_meta_vram_pin(struct loonggpu_device *adev)
{
	int r;

	r = loonggpu_bo_reserve(adev->zip_meta.robj, false);
	if (unlikely(r != 0))
		return r;
	r = loonggpu_bo_pin(adev->zip_meta.robj, LOONGGPU_GEM_DOMAIN_VRAM);
	if (r) {
		loonggpu_bo_unreserve(adev->zip_meta.robj);
		return r;
	}
	r = loonggpu_bo_kmap(adev->zip_meta.robj, &adev->zip_meta.ptr);
	if (r)
		loonggpu_bo_unpin(adev->zip_meta.robj);
	loonggpu_bo_unreserve(adev->zip_meta.robj);

	adev->zip_meta.table_addr = loonggpu_bo_gpu_offset(adev->zip_meta.robj);
	return r;
}

/**
 * loonggpu_zip_meta_vram_unpin - unpin zip meta page table in vram
 *
 * @adev: loonggpu_device pointer
 *
 * Unpin the ZIP META page table in vram (pcie r4xx, r5xx+).
 * These asics require the zip meta table to be in video memory.
 */
void loonggpu_zip_meta_vram_unpin(struct loonggpu_device *adev)
{
	int r;

	if (adev->zip_meta.robj == NULL) {
		return;
	}
	r = loonggpu_bo_reserve(adev->zip_meta.robj, true);
	if (likely(r == 0)) {
		loonggpu_bo_kunmap(adev->zip_meta.robj);
		loonggpu_bo_unpin(adev->zip_meta.robj);
		loonggpu_bo_unreserve(adev->zip_meta.robj);
		adev->zip_meta.ptr = NULL;
	}
}

/**
 * loonggpu_zip_meta_vram_free - free zip meta page table vram
 *
 * @adev: loonggpu_device pointer
 *
 */
void loonggpu_zip_meta_vram_free(struct loonggpu_device *adev)
{
	if (adev->zip_meta.robj == NULL) {
		return;
	}
	loonggpu_bo_unref(&adev->zip_meta.robj);
}

/*
 * Common zip meta functions.
 */
/**
 * loonggpu_zip_meta_unbind - unbind pages from the zip meta page table
 *
 * @adev: loonggpu_device pointer
 * @offset: offset into the GPU's zip meta aperture
 * @pages: number of pages to unbind
 *
 * Unbinds the requested pages from the zip meta page table and
 * Returns 0 for success, -EINVAL for failure.
 */
int loonggpu_zip_meta_unbind(struct loonggpu_device *adev, uint64_t offset,
			int pages)
{
	return 0;
}

/**
 * loonggpu_zip_meta_map - map dma_addresses into zip meta entries
 *
 * @adev: loonggpu_device pointer
 * @start: start into the GPU's memery aperture
 *
 * Map the dma_addresses into zip meta entries (all asics).
 * Returns 0 for success, -EINVAL for failure.
 */
uint64_t loonggpu_zip_meta_map(struct loonggpu_device *adev, uint64_t start)
{
	return 0;
}

/**
 * loonggpu_zip_meta_bind - bind pages into the zip meta page table
 *
 * @adev: loonggpu_device pointer
 * @offset: offset into the GPU's zip meta aperture
 * @pages: number of pages to bind
 * @pagelist: pages to bind
 * @dma_addr: DMA addresses of pages
 *
 * Binds the requested pages to the zip meta page table
 * (all asics).
 * Returns 0 for success, -EINVAL for failure.
 */
int loonggpu_zip_meta_bind(struct loonggpu_device *adev, uint64_t offset,
		     int pages, struct page **pagelist, dma_addr_t *dma_addr,
		     uint64_t flags)
{
#ifdef CONFIG_DRM_LOONGGPU_ZIP_DEBUGFS
	unsigned i, t, p;
#endif

#ifdef CONFIG_DRM_LOONGGPU_ZIP_DEBUGFS
	t = offset / LOONGGPU_GPU_PAGE_SIZE;
	p = t / LOONGGPU_GPU_PAGES_IN_CPU_PAGE;
	for (i = 0; i < pages; i++, p++)
		adev->zip_meta.pages[p] = pagelist ? pagelist[i] : NULL;
#endif

	return 0;
}

/**
 * loonggpu_zip_meta_init - init the driver info for managing the zip meta
 *
 * @adev: loonggpu_device pointer
 *
 * Allocate the dummy page and init the zip meta driver info (all asics).
 * Returns 0 for success, error for failure.
 */
int loonggpu_zip_meta_init(struct loonggpu_device *adev)
{
	/* We need PAGE_SIZE >= LOONGGPU_GPU_PAGE_SIZE */
	if (PAGE_SIZE < LOONGGPU_GPU_PAGE_SIZE) {
		DRM_ERROR("Page size is smaller than GPU page size!\n");
		return -EINVAL;
	}

	/* Compute table size */
	adev->zip_meta.num_cpu_pages = (adev->gmc.real_vram_size >> 7) / PAGE_SIZE;
	adev->zip_meta.num_gpu_pages = (adev->gmc.real_vram_size >> 7) / LOONGGPU_GPU_PAGE_SIZE;
	DRM_INFO("ZIP META: num cpu pages %u, num gpu pages %u\n",
		 adev->zip_meta.num_cpu_pages, adev->zip_meta.num_gpu_pages);

#ifdef CONFIG_DRM_LOONGGPU_ZIP_DEBUGFS
	/* Allocate pages table */
	adev->zip_meta.pages = vzalloc(array_size(sizeof(void *),
					      adev->zip_meta.num_cpu_pages));
	if (adev->zip_meta.pages == NULL)
		return -ENOMEM;
#endif

	return 0;
}

/**
 * loonggpu_zip_meta_fini - tear down the driver info for managing the zip meta
 *
 * @adev: loonggpu_device pointer
 *
 */
void loonggpu_zip_meta_fini(struct loonggpu_device *adev)
{
#ifdef CONFIG_DRM_LOONGGPU_ZIP_DEBUGFS
	vfree(adev->zip_meta.pages);
	adev->zip_meta.pages = NULL;
#endif
}

int loonggpu_get_meta_info_ioctl(struct drm_device *dev, void *data,
				 struct drm_file *filp)
{
	struct loonggpu_device *adev = dev->dev_private;
	struct drm_loonggpu_meta_info *args = data;
	struct drm_gem_object *gobj;
	struct loonggpu_bo *abo;
	int ret = 0;

	gobj = drm_gem_object_lookup(filp, args->handle);
	if (!gobj)
		return -ENOENT;

	abo = gem_to_loonggpu_bo(gobj);

	if (!(abo->flags & LOONGGPU_GEM_CREATE_COMPRESSED_MASK)) {
		ret = -EINVAL;
		goto out;
	}

	args->bo_meta_offset =
		(loonggpu_bo_gpu_offset(abo) - adev->gmc.vram_start) / 128;

out:
	lg_drm_gem_object_put(gobj);

	return ret;
}

int loonggpu_get_meta_bo_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *filp)
{
	struct loonggpu_device *adev = dev->dev_private;
	struct drm_loonggpu_meta_bo *args = data;
	struct drm_gem_object *meta_gobj;
	int ret = 0;

	if (!adev->zip_meta.robj)
		return -EINVAL;

	meta_gobj = &lg_gbo_to_gem_obj(adev->zip_meta.robj);
	meta_gobj->funcs = &loonggpu_gem_object_funcs;

	ret = drm_gem_handle_create(filp, meta_gobj, &args->meta_bo_handle);
	if (ret) {
		DRM_INFO("can not create meta bo handle!\n");
		return ret;
	}

	args->meta_bo_size = adev->zip_meta.table_size;

	return ret;
}
