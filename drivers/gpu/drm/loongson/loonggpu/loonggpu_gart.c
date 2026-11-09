#include <linux/pci.h>
#include "loonggpu.h"
#include "loonggpu_drm.h"
#include "loonggpu_mmu.h"

/*
 * GART
 * The GART (Graphics Aperture Remapping Table) is an aperture
 * in the GPU's address space.  System pages can be mapped into
 * the aperture and look like contiguous pages from the GPU's
 * perspective.  A page table maps the pages in the aperture
 * to the actual backing pages in system memory.
 *
 * LOONGGPU GPUs support both an internal GART, as described above,
 * and AGP.  AGP works similarly, but the GART table is configured
 * and maintained by the northbridge rather than the driver.
 * LOONGGPU hw has a separate AGP aperture that is programmed to
 * point to the AGP aperture provided by the northbridge and the
 * requests are passed through to the northbridge aperture.
 * Both AGP and internal GART can be used at the same time, however
 * that is not currently supported by the driver.
 *
 * This file handles the common internal GART management.
 */

/*
 * Common GART table functions.
 */

/**
 * loonggpu_dummy_page_init - init dummy page used by the driver
 *
 * @adev: loonggpu_device pointer
 *
 * Allocate the dummy page used by the driver (all asics).
 * This dummy page is used by the driver as a filler for gart entries
 * when pages are taken out of the GART
 * Returns 0 on sucess, -ENOMEM on failure.
 */
static int loonggpu_gart_dummy_page_init(struct loonggpu_device *adev)
{
	void *dummy_addr;
	struct page *dummy_page = ttm_glob.dummy_read_page;

	if (adev->dummy_page_addr)
		return 0;

	dummy_addr = page_address(dummy_page);
	memset(dummy_addr, 0xdd, PAGE_SIZE);

	adev->dummy_page_addr = dma_map_page(adev->dev, dummy_page, 0,
					     PAGE_SIZE, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(&adev->pdev->dev, adev->dummy_page_addr)) {
		dev_err(adev->dev, "Failed to DMA MAP the dummy page\n");
		adev->dummy_page_addr = 0;
		return -ENOMEM;
	}

	return 0;
}

/**
 * loonggpu_dummy_page_fini - free dummy page used by the driver
 *
 * @adev: loonggpu_device pointer
 *
 * Frees the dummy page used by the driver (all asics).
 */
static void loonggpu_gart_dummy_page_fini(struct loonggpu_device *adev)
{
	if (!adev->dummy_page_addr)
		return;

	dma_unmap_page(adev->dev, adev->dummy_page_addr, PAGE_SIZE, DMA_BIDIRECTIONAL);

	adev->dummy_page_addr = 0;
}

/**
 * loonggpu_gart_table_vram_alloc - allocate vram for gart page table
 *
 * @adev: loonggpu_device pointer
 *
 * Allocate video memory for GART page table
 * (pcie r4xx, r5xx+).  These asics require the
 * gart table to be in video memory.
 * Returns 0 for success, error for failure.
 */
int loonggpu_gart_table_vram_alloc(struct loonggpu_device *adev)
{
	int r;

	if (adev->gart.robj == NULL) {
		struct loonggpu_bo_param bp;

		memset(&bp, 0, sizeof(bp));
		/*
		 * FIXME: Expand 256M at the end of gart table for bpipe hardware,
		 * it can be optimized into the 256M gart table in the future.
		 */
		bp.size = adev->gart.table_size +
			(LOONGGPU_VRAM_MAX_TRANSFER_SIZE *
			LOONGGPU_VRAM_NUM_TRANSFER_WINDOWS *
			LOONGGPU_MMU_PTE_SIZE);
		bp.byte_align = PAGE_SIZE;
		bp.domain = LOONGGPU_GEM_DOMAIN_VRAM;
		bp.flags = LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED |
			LOONGGPU_GEM_CREATE_VRAM_CONTIGUOUS;
		bp.type = ttm_bo_type_kernel;
		bp.resv = NULL;
		r = loonggpu_bo_create(adev, &bp, &adev->gart.robj);
		if (r) {
			return r;
		}
	}
	return 0;
}

/**
 * loonggpu_gart_table_vram_pin - pin gart page table in vram
 *
 * @adev: loonggpu_device pointer
 *
 * Pin the GART page table in vram so it will not be moved
 * by the memory manager (pcie r4xx, r5xx+).  These asics require the
 * gart table to be in video memory.
 * Returns 0 for success, error for failure.
 */
int loonggpu_gart_table_vram_pin(struct loonggpu_device *adev)
{
	int r;

	r = loonggpu_bo_reserve(adev->gart.robj, false);
	if (unlikely(r != 0))
		return r;
	r = loonggpu_bo_pin(adev->gart.robj, LOONGGPU_GEM_DOMAIN_VRAM);
	if (r) {
		loonggpu_bo_unreserve(adev->gart.robj);
		return r;
	}
	r = loonggpu_bo_kmap(adev->gart.robj, &adev->gart.ptr);
	if (r)
		loonggpu_bo_unpin(adev->gart.robj);
	loonggpu_bo_unreserve(adev->gart.robj);
	adev->gart.table_addr = loonggpu_bo_gpu_offset(adev->gart.robj);
	return r;
}

/**
 * loonggpu_gart_table_vram_unpin - unpin gart page table in vram
 *
 * @adev: loonggpu_device pointer
 *
 * Unpin the GART page table in vram (pcie r4xx, r5xx+).
 * These asics require the gart table to be in video memory.
 */
void loonggpu_gart_table_vram_unpin(struct loonggpu_device *adev)
{
	int r;

	if (adev->gart.robj == NULL) {
		return;
	}
	r = loonggpu_bo_reserve(adev->gart.robj, true);
	if (likely(r == 0)) {
		loonggpu_bo_kunmap(adev->gart.robj);
		loonggpu_bo_unpin(adev->gart.robj);
		loonggpu_bo_unreserve(adev->gart.robj);
		adev->gart.ptr = NULL;
	}
}

/**
 * loonggpu_gart_table_vram_free - free gart page table vram
 *
 * @adev: loonggpu_device pointer
 *
 * Free the video memory used for the GART page table
 * (pcie r4xx, r5xx+).  These asics require the gart table to
 * be in video memory.
 */
void loonggpu_gart_table_vram_free(struct loonggpu_device *adev)
{
	if (adev->gart.robj == NULL) {
		return;
	}
	loonggpu_bo_unref(&adev->gart.robj);
}

/*
 * Common gart functions.
 */
/**
 * loonggpu_gart_unbind - unbind pages from the gart page table
 *
 * @adev: loonggpu_device pointer
 * @offset: offset into the GPU's gart aperture
 * @pages: number of pages to unbind
 *
 * Unbinds the requested pages from the gart page table and
 * replaces them with the dummy page (all asics).
 * Returns 0 for success, -EINVAL for failure.
 */
int loonggpu_gart_unbind(struct loonggpu_device *adev, uint64_t offset,
			int pages)
{
	unsigned t;
	unsigned p;
	int i, j;
	u64 page_base;
	/* Starting from VEGA10, system bit must be 0 to mean invalid. */
	uint64_t flags = 0;

	if (!adev->gart.ready) {
		WARN(1, "trying to unbind memory from uninitialized GART !\n");
		return -EINVAL;
	}

	t = offset / LOONGGPU_GPU_PAGE_SIZE;
	p = t / LOONGGPU_GPU_PAGES_IN_CPU_PAGE;
	for (i = 0; i < pages; i++, p++) {
#ifdef CONFIG_DRM_LOONGGPU_GART_DEBUGFS
		adev->gart.pages[p] = NULL;
#endif
		page_base = adev->dummy_page_addr;
		if (!adev->gart.ptr)
			continue;

		for (j = 0; j < LOONGGPU_GPU_PAGES_IN_CPU_PAGE; j++, t++) {
			loonggpu_gmc_set_pte_pde(adev, adev->gart.ptr,
					       t, page_base, flags);
			page_base += LOONGGPU_GPU_PAGE_SIZE;
		}
	}
	mb();
	loonggpu_gmc_flush_gpu_tlb(adev, 0);
	return 0;
}

/**
 * loonggpu_gart_map - map dma_addresses into GART entries
 *
 * @adev: loonggpu_device pointer
 * @offset: offset into the GPU's gart aperture
 * @pages: number of pages to bind
 * @dma_addr: DMA addresses of pages
 *
 * Map the dma_addresses into GART entries (all asics).
 * Returns 0 for success, -EINVAL for failure.
 */
int loonggpu_gart_map(struct loonggpu_device *adev, uint64_t offset,
		    int pages, dma_addr_t *dma_addr, uint64_t flags,
		    void *dst)
{
	uint64_t page_base;
	unsigned i, j, t;

	if (!adev->gart.ready) {
		WARN(1, "trying to bind memory to uninitialized GART !\n");
		return -EINVAL;
	}

	t = offset / LOONGGPU_GPU_PAGE_SIZE;

	for (i = 0; i < pages; i++) {
		page_base = dma_addr[i];
		for (j = 0; j < LOONGGPU_GPU_PAGES_IN_CPU_PAGE; j++, t++) {
			loonggpu_gmc_set_pte_pde(adev, dst, t, page_base, flags);
			page_base += LOONGGPU_GPU_PAGE_SIZE;
		}
	}
	return 0;
}

/**
 * loonggpu_gart_bind - bind pages into the gart page table
 *
 * @adev: loonggpu_device pointer
 * @offset: offset into the GPU's gart aperture
 * @pages: number of pages to bind
 * @pagelist: pages to bind
 * @dma_addr: DMA addresses of pages
 *
 * Binds the requested pages to the gart page table
 * (all asics).
 * Returns 0 for success, -EINVAL for failure.
 */
int loonggpu_gart_bind(struct loonggpu_device *adev, uint64_t offset,
		     int pages, struct page **pagelist, dma_addr_t *dma_addr,
		     uint64_t flags)
{
#ifdef CONFIG_DRM_LOONGGPU_GART_DEBUGFS
	unsigned i, t, p;
#endif
	int r;

	if (!adev->gart.ready) {
		WARN(1, "trying to bind memory to uninitialized GART !\n");
		return -EINVAL;
	}

#ifdef CONFIG_DRM_LOONGGPU_GART_DEBUGFS
	t = offset / LOONGGPU_GPU_PAGE_SIZE;
	p = t / LOONGGPU_GPU_PAGES_IN_CPU_PAGE;
	for (i = 0; i < pages; i++, p++)
		adev->gart.pages[p] = pagelist ? pagelist[i] : NULL;
#endif

	if (!adev->gart.ptr)
		return 0;

	r = loonggpu_gart_map(adev, offset, pages, dma_addr, flags,
		    adev->gart.ptr);
	if (r)
		return r;

	mb();
	loonggpu_gmc_flush_gpu_tlb(adev, 0);
	return 0;
}

/**
 * loonggpu_gart_init - init the driver info for managing the gart
 *
 * @adev: loonggpu_device pointer
 *
 * Allocate the dummy page and init the gart driver info (all asics).
 * Returns 0 for success, error for failure.
 */
int loonggpu_gart_init(struct loonggpu_device *adev)
{
	int r;

	if (adev->dummy_page_addr)
		return 0;

	/* We need PAGE_SIZE >= LOONGGPU_GPU_PAGE_SIZE */
	if (PAGE_SIZE < LOONGGPU_GPU_PAGE_SIZE) {
		DRM_ERROR("Page size is smaller than GPU page size!\n");
		return -EINVAL;
	}
	r = loonggpu_gart_dummy_page_init(adev);
	if (r)
		return r;
	/* Compute table size */
	adev->gart.num_cpu_pages = adev->gmc.gart_size / PAGE_SIZE;
	adev->gart.num_gpu_pages = adev->gmc.gart_size / LOONGGPU_GPU_PAGE_SIZE;
	DRM_INFO("GART: num cpu pages %u, num gpu pages %u\n",
		 adev->gart.num_cpu_pages, adev->gart.num_gpu_pages);

#ifdef CONFIG_DRM_LOONGGPU_GART_DEBUGFS
	/* Allocate pages table */
	adev->gart.pages = vzalloc(array_size(sizeof(void *),
					      adev->gart.num_cpu_pages));
	if (adev->gart.pages == NULL)
		return -ENOMEM;
#endif

	return 0;
}

/**
 * loonggpu_gart_fini - tear down the driver info for managing the gart
 *
 * @adev: loonggpu_device pointer
 *
 * Tear down the gart driver info and free the dummy page (all asics).
 */
void loonggpu_gart_fini(struct loonggpu_device *adev)
{
#ifdef CONFIG_DRM_LOONGGPU_GART_DEBUGFS
	vfree(adev->gart.pages);
	adev->gart.pages = NULL;
#endif
	loonggpu_gart_dummy_page_fini(adev);
}
