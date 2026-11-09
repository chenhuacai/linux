#ifndef __LOONGGPU_TTM_H__
#define __LOONGGPU_TTM_H__

#include <drm/gpu_scheduler.h>
#include "loonggpu_ttm_helper.h"

#include "loonggpu_dma_resv_helper.h"
#include "loonggpu_ttm_resource_helper.h"
#include "loonggpu_lgkcd.h"

#define LOONGGPU_PL_DOORBELL	(TTM_PL_PRIV + 0)

#define LOONGGPU_PL_FLAG_DOORBELL		(TTM_PL_FLAG_PRIV << 0)
#define LOONGGPU_GTT_MAX_TRANSFER_SIZE	512
#define LOONGGPU_GTT_NUM_TRANSFER_WINDOWS	2

#define LOONGGPU_VRAM_MAX_TRANSFER_SIZE    4096
#define LOONGGPU_VRAM_NUM_TRANSFER_WINDOWS     2
uint64_t loonggpu_ttm_domain_start(struct loonggpu_device *adev, uint32_t type);

struct loonggpu_vram_mgr {
        lg_loonggpu_vram_mgr_man
	struct lg_buddy b_mm;
	struct mutex m_lock;
        struct drm_mm mm;
        spinlock_t lock;
        atomic64_t usage;
        atomic64_t vis_usage;
	struct list_head reservations_pending;
	struct list_head reserved_pages;
	u64 default_page_size;
};

struct loonggpu_gtt_mgr {
        lg_loonggpu_gtt_mgr_man
        struct drm_mm mm;
        spinlock_t lock;
        atomic64_t available;
};

struct loonggpu_mman {
	struct ttm_device               bdev;
	bool				mem_global_referenced;
	bool				initialized;
	void __iomem			*aper_base_kaddr;

#if defined(CONFIG_DEBUG_FS)
	struct dentry			*debugfs_entries[8];
#endif

	/* buffer handling */
	const struct loonggpu_buffer_funcs	*buffer_funcs;
	struct loonggpu_ring			*buffer_funcs_ring;
	bool					buffer_funcs_enabled;

	struct mutex				gtt_window_lock;
	/* Scheduler entity for buffer moves */
	struct drm_sched_entity			entity;

	/* members for compatible */
	LG_LOONGGPU_VRAM_GTT_MGR_COMPAT_MEMBERS
};

struct loonggpu_copy_mem {
	struct ttm_buffer_object	*bo;
	lg_ttm_mem_t			*mem;
	unsigned long			offset;
};

extern const lg_ttm_mem_func_t loonggpu_gtt_mgr_func;
extern const lg_ttm_mem_func_t loonggpu_vram_mgr_func;

bool loonggpu_gtt_mgr_has_gart_addr(lg_ttm_mem_t *mem);
uint64_t loonggpu_gtt_mgr_usage(lg_ttm_manager_t *man);
int loonggpu_gtt_mgr_recover(lg_ttm_manager_t *man);

u64 loonggpu_vram_mgr_bo_visible_size(struct loonggpu_bo *bo);
uint64_t loonggpu_vram_mgr_usage(lg_ttm_manager_t *man);
uint64_t loonggpu_vram_mgr_vis_usage(lg_ttm_manager_t *man);

#ifdef CONFIG_VDD_LOONGSON_P2P
int loonggpu_vram_mgr_alloc_sgt(struct loonggpu_device *adev,
				struct ttm_resource *res,
				u64 offset, u64 length,
				struct device *dev,
				enum dma_data_direction dir,
				struct sg_table **sgt);
void loonggpu_vram_mgr_free_sgt(struct device *dev,
				enum dma_data_direction dir,
				struct sg_table *sgt);
#endif

int loonggpu_ttm_init(struct loonggpu_device *adev);
void loonggpu_ttm_late_init(struct loonggpu_device *adev);
void loonggpu_ttm_fini(struct loonggpu_device *adev);
void loonggpu_ttm_set_buffer_funcs_status(struct loonggpu_device *adev,
					bool enable);

int loonggpu_copy_buffer(struct loonggpu_ring *ring, uint64_t src_offset,
		       uint64_t dst_offset, uint32_t byte_count,
		       lg_dma_resv_t *resv,
		       struct dma_fence **fence, bool direct_submit,
		       bool vm_needs_flush);
int loonggpu_ttm_copy_mem_to_mem(struct loonggpu_device *adev,
			       struct loonggpu_copy_mem *src,
			       struct loonggpu_copy_mem *dst,
			       uint64_t size,
			       lg_dma_resv_t *resv,
			       struct dma_fence **f);
int loonggpu_fill_buffer(struct loonggpu_bo *bo,
			uint32_t src_data,
			lg_dma_resv_t *resv,
			struct dma_fence **fence);

int loonggpu_mmap(struct file *filp, struct vm_area_struct *vma);
int loonggpu_ttm_alloc_gart(struct ttm_buffer_object *bo);
int loonggpu_ttm_gart_bind(struct loonggpu_device *adev,
			struct ttm_buffer_object *tbo,
			uint64_t flags);
int loonggpu_ttm_recover_gart(struct ttm_buffer_object *tbo);

int loonggpu_ttm_tt_get_user_pages(struct ttm_tt *ttm, struct page **pages);
void loonggpu_ttm_tt_set_user_pages(struct ttm_tt *ttm, struct page **pages);
void loonggpu_ttm_tt_mark_user_pages(struct ttm_tt *ttm);
int loonggpu_ttm_tt_set_userptr(struct ttm_tt *ttm, uint64_t addr,
				     uint32_t flags);
bool loonggpu_ttm_tt_has_userptr(struct ttm_tt *ttm);
struct mm_struct *loonggpu_ttm_tt_get_usermm(struct ttm_tt *ttm);
bool loonggpu_ttm_tt_affect_userptr(struct ttm_tt *ttm, unsigned long start,
				  unsigned long end);
bool loonggpu_ttm_tt_userptr_invalidated(struct ttm_tt *ttm,
				       int *last_invalidated);
bool loonggpu_ttm_tt_userptr_needs_pages(struct ttm_tt *ttm);
bool loonggpu_ttm_tt_is_readonly(struct ttm_tt *ttm);
uint64_t loonggpu_ttm_tt_pte_flags(struct loonggpu_device *adev, struct ttm_tt *ttm,
				 lg_ttm_mem_t *mem);
int loonggpu_ttm_tt_get_userptr(const struct ttm_buffer_object *tbo,
		uint64_t *user_addr);
int loonggpu_vram_mgr_init(lg_ttm_manager_t *man, unsigned long p_size);
int loonggpu_gtt_mgr_init(lg_ttm_manager_t *man, unsigned long p_size);
int loonggpu_vram_mgr_fini(lg_ttm_manager_t *man);
int loonggpu_gtt_mgr_fini(lg_ttm_manager_t *man);
int loonggpu_fill_buffer_buddy(struct loonggpu_bo *bo,
			    uint32_t src_data,
			    struct dma_resv *resv,
			    struct dma_fence **f,
			    bool delayed);
int loonggpu_vram_mgr_init_buddy(struct loonggpu_device *adev);
void loonggpu_vram_mgr_fini_buddy(struct loonggpu_device *adev);
#endif /* __LOONGGPU_TTM_H__ */
