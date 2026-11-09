#ifndef __LOONGGPU_GART_H__
#define __LOONGGPU_GART_H__

#include <linux/types.h>

/*
 * GART structures, functions & helpers
 */
struct loonggpu_device;
struct loonggpu_bo;

#define LOONGGPU_GPU_PAGE_SIZE PAGE_SIZE
#define LOONGGPU_GPU_PAGE_MASK (LOONGGPU_GPU_PAGE_SIZE - 1)
#define LOONGGPU_GPU_PAGE_SHIFT PAGE_SHIFT

#define LOONGGPU_PAGE_PTE_SHIFT (LOONGGPU_GPU_PAGE_SHIFT - 3)
#define LOONGGPU_GPU_PAGE_ALIGN(a) (((a) + LOONGGPU_GPU_PAGE_MASK) & ~LOONGGPU_GPU_PAGE_MASK)

#define LOONGGPU_GPU_PAGES_IN_CPU_PAGE (PAGE_SIZE / LOONGGPU_GPU_PAGE_SIZE)

struct loonggpu_gart {
	u64				table_addr;
	struct loonggpu_bo			*robj;
	void				*ptr;
	unsigned			num_gpu_pages;
	unsigned			num_cpu_pages;
	unsigned			table_size;
#ifdef CONFIG_DRM_LOONGGPU_GART_DEBUGFS
	struct page			**pages;
#endif
	bool				ready;

	/* Asic default pte flags */
	uint64_t			gart_pte_flags;
};

int loonggpu_gart_table_vram_alloc(struct loonggpu_device *adev);
void loonggpu_gart_table_vram_free(struct loonggpu_device *adev);
int loonggpu_gart_table_vram_pin(struct loonggpu_device *adev);
void loonggpu_gart_table_vram_unpin(struct loonggpu_device *adev);
int loonggpu_gart_init(struct loonggpu_device *adev);
void loonggpu_gart_fini(struct loonggpu_device *adev);
int loonggpu_gart_unbind(struct loonggpu_device *adev, uint64_t offset,
		       int pages);
int loonggpu_gart_map(struct loonggpu_device *adev, uint64_t offset,
		    int pages, dma_addr_t *dma_addr, uint64_t flags,
		    void *dst);
int loonggpu_gart_bind(struct loonggpu_device *adev, uint64_t offset,
		     int pages, struct page **pagelist,
		     dma_addr_t *dma_addr, uint64_t flags);

#endif /* __LOONGGPU_GART_H__ */
