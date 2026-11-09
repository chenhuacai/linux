#ifndef __LOONGGPU_ZIP_META_H__
#define __LOONGGPU_ZIP_META_H__

#include <linux/types.h>

/*
 * ZIP META structures, functions & helpers
 */
struct loonggpu_device;
struct loonggpu_bo;


#define LOONGGPU_GEM_META_ALIGN_SHIFT 7
#define LOONGGPU_GEM_COMPRESSED_SIZE (PAGE_SIZE<<LOONGGPU_GEM_META_ALIGN_SHIFT)

struct loonggpu_zip_meta {
	u64 table_addr;
	u64 mask;
	struct loonggpu_bo *robj;
	void *ptr;
	unsigned num_gpu_pages;
	unsigned num_cpu_pages;
	unsigned table_size;
#ifdef CONFIG_DRM_LOONGGPU_ZIP_DEBUGFS
	struct page **pages;
#endif
	bool ready;
	u64 pte_flags;
};

int loonggpu_zip_meta_vram_alloc(struct loonggpu_device *adev);
void loonggpu_zip_meta_vram_free(struct loonggpu_device *adev);
int loonggpu_zip_meta_vram_pin(struct loonggpu_device *adev);
void loonggpu_zip_meta_vram_unpin(struct loonggpu_device *adev);
int loonggpu_zip_meta_init(struct loonggpu_device *adev);
void loonggpu_zip_meta_fini(struct loonggpu_device *adev);
int loonggpu_zip_meta_unbind(struct loonggpu_device *adev, uint64_t offset,
		       int pages);
uint64_t loonggpu_zip_meta_map(struct loonggpu_device *adev, uint64_t start);
int loonggpu_zip_meta_bind(struct loonggpu_device *adev, uint64_t offset,
		     int pages, struct page **pagelist,
		     dma_addr_t *dma_addr, uint64_t flags);

#endif /* __LOONGGPU_ZIP_META_H__ */
