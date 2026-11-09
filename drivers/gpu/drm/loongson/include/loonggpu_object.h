#ifndef __LOONGGPU_OBJECT_H__
#define __LOONGGPU_OBJECT_H__

#include "loonggpu_drm.h"
#include "loonggpu.h"
#include "loonggpu_helper.h"
#include <drm/ttm/ttm_range_manager.h>
#include "loonggpu_gtt_mgr_helper.h"

#define LOONGGPU_BO_INVALID_OFFSET	LONG_MAX
#define LOONGGPU_BO_MAX_PLACEMENTS	3

struct loonggpu_bo_param {
	unsigned long			size;
	int				byte_align;
	u32				domain;
	u32				preferred_domain;
	u64				flags;
	enum ttm_bo_type		type;
	lg_dma_resv_t	*resv;
};

/* User space allocated BO in a VM */
struct loonggpu_bo_va {
	struct loonggpu_vm_bo_base	base;

	/* protected by bo being reserved */
	unsigned			ref_count;

	/* all other members protected by the VM PD being reserved */
	struct dma_fence	        *last_pt_update;

	/* mappings for this bo_va */
	struct list_head		invalids;
	struct list_head		valids;

	/* If the mappings are cleared or filled */
	bool				cleared;
};

struct loonggpu_bo {
	/* Protected by tbo.reserved */
	u32				preferred_domains;
	u32				allowed_domains;
	struct ttm_place		placements[LOONGGPU_BO_MAX_PLACEMENTS];
	struct ttm_placement		placement;
	struct ttm_buffer_object	tbo;
	struct ttm_bo_kmap_obj		kmap;
	u64				flags;
	u64				tiling_flags;
	u64				node_offset;
	u64				metadata_flags;
	void				*metadata;
	u32				metadata_size;
	unsigned			prime_shared_count;
	bool                            is_other_gpu_share;
	/* list of all virtual address to which this bo is associated to */
	struct list_head		va;
	/* Constant after initialization */
	struct drm_gem_object		gem_base;
	struct loonggpu_bo		*parent;
	struct loonggpu_bo		*shadow;

	struct ttm_bo_kmap_obj		dma_buf_vmap;
	struct loonggpu_mn		*mn;

	union {
		struct list_head	mn_list;
		struct list_head	shadow_list;
	};

	struct kgd_mem                  *kcd_bo;
};

extern const struct drm_gem_object_funcs loonggpu_gem_object_funcs;

static inline struct loonggpu_bo *ttm_to_loonggpu_bo(struct ttm_buffer_object *tbo)
{
	return container_of(tbo, struct loonggpu_bo, tbo);
}

/**
 * loonggpu_mem_type_to_domain - return domain corresponding to mem_type
 * @mem_type:	ttm memory type
 *
 * Returns corresponding domain of the ttm mem_type
 */
static inline unsigned loonggpu_mem_type_to_domain(u32 mem_type)
{
	switch (mem_type) {
	case TTM_PL_VRAM:
		return LOONGGPU_GEM_DOMAIN_VRAM;
	case TTM_PL_TT:
		return LOONGGPU_GEM_DOMAIN_GTT;
	case TTM_PL_SYSTEM:
		return LOONGGPU_GEM_DOMAIN_CPU;
	case LOONGGPU_PL_DOORBELL:
		return LOONGGPU_GEM_DOMAIN_DOORBELL;
	default:
		break;
	}
	return 0;
}

/**
 * loonggpu_bo_reserve - reserve bo
 * @bo:		bo structure
 * @no_intr:	don't return -ERESTARTSYS on pending signal
 *
 * Returns:
 * -ERESTARTSYS: A wait for the buffer to become unreserved was interrupted by
 * a signal. Release all buffer reservations and return to user-space.
 */
static inline int loonggpu_bo_reserve(struct loonggpu_bo *bo, bool no_intr)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);
	int r;

	r = ttm_bo_reserve(&bo->tbo, !no_intr, false, NULL);
	if (unlikely(r != 0)) {
		if (r != -ERESTARTSYS)
			dev_err(adev->dev, "%p reserve failed\n", bo);
		return r;
	}
	return 0;
}

static inline void loonggpu_bo_unreserve(struct loonggpu_bo *bo)
{
	ttm_bo_unreserve(&bo->tbo);
}

static inline unsigned long loonggpu_bo_size(struct loonggpu_bo *bo)
{
	return bo->tbo.base.size;
}

static inline unsigned long lg_tbo_to_num_pages(struct ttm_buffer_object *tbo)
{
	return PFN_UP(tbo->base.size);
}

static inline unsigned loonggpu_bo_ngpu_pages(struct loonggpu_bo *bo)
{
	return bo->tbo.base.size / LOONGGPU_GPU_PAGE_SIZE;
}

#define to_dma_resv(bo) ((bo)->tbo.base.resv)

/**
 * loonggpu_bo_mmap_offset - return mmap offset of bo
 * @bo:	loonggpu object for which we query the offset
 *
 * Returns mmap offset of the object.
 */
static inline u64 loonggpu_bo_mmap_offset(struct loonggpu_bo *bo)
{
	return drm_vma_node_offset_addr(&bo->tbo.base.vma_node);
}

/**
 * loonggpu_bo_gpu_accessible - return whether the bo is currently in memory that
 * is accessible to the GPU.
 */
static inline bool loonggpu_bo_gpu_accessible(struct loonggpu_bo *bo)
{
	switch (lg_get_bo_mem_type(bo)) {
	case TTM_PL_TT:
		return loonggpu_gtt_mgr_has_gart_addr(bo->tbo.resource);
	case TTM_PL_VRAM: return true;
	default: return false;
	}
}

/**
 * loonggpu_bo_in_cpu_visible_vram - check if BO is (partly) in visible VRAM
 */
static inline bool loonggpu_bo_in_cpu_visible_vram(struct loonggpu_bo *bo)
{
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);
	struct loonggpu_res_cursor cursor;

	if (!lg_tbo_to_mem(&bo->tbo) || lg_get_bo_mem_type(bo) != TTM_PL_VRAM)
		return false;

	loonggpu_res_first(bo->tbo.resource, 0, bo->tbo.base.size, &cursor);

	while (cursor.remaining) {
		if (cursor.start < adev->gmc.visible_vram_size)
			return true;

		loonggpu_res_next(&cursor, cursor.size);
	}
	return false;
}

/**
 * loonggpu_bo_explicit_sync - return whether the bo is explicitly synced
 */
static inline bool loonggpu_bo_explicit_sync(struct loonggpu_bo *bo)
{
	return bo->flags & LOONGGPU_GEM_CREATE_EXPLICIT_SYNC;
}

bool loonggpu_bo_is_loonggpu_bo(struct ttm_buffer_object *bo);
void loonggpu_bo_placement_from_domain(struct loonggpu_bo *abo, u32 domain);

int loonggpu_bo_create(struct loonggpu_device *adev,
		     struct loonggpu_bo_param *bp,
		     struct loonggpu_bo **bo_ptr);
int loonggpu_bo_create_reserved(struct loonggpu_device *adev,
			      unsigned long size, int align,
			      u32 domain, struct loonggpu_bo **bo_ptr,
			      u64 *gpu_addr, void **cpu_addr);
int loonggpu_bo_create_kernel(struct loonggpu_device *adev,
			    unsigned long size, int align,
			    u32 domain, struct loonggpu_bo **bo_ptr,
			    u64 *gpu_addr, void **cpu_addr);
void loonggpu_bo_free_kernel(struct loonggpu_bo **bo, u64 *gpu_addr,
			   void **cpu_addr);
int loonggpu_bo_kmap(struct loonggpu_bo *bo, void **ptr);
void *loonggpu_bo_kptr(struct loonggpu_bo *bo);
void loonggpu_bo_kunmap(struct loonggpu_bo *bo);
struct loonggpu_bo *loonggpu_bo_ref(struct loonggpu_bo *bo);
void loonggpu_bo_unref(struct loonggpu_bo **bo);
int loonggpu_bo_pin(struct loonggpu_bo *bo, u32 domain);
int loonggpu_bo_pin_restricted(struct loonggpu_bo *bo, u32 domain,
			     u64 min_offset, u64 max_offset);
int loonggpu_bo_unpin(struct loonggpu_bo *bo);
int loonggpu_bo_evict_vram(struct loonggpu_device *adev);
int loonggpu_bo_init(struct loonggpu_device *adev);
int loonggpu_bo_late_init(struct loonggpu_device *adev);
void loonggpu_bo_fini(struct loonggpu_device *adev);
int loonggpu_bo_fbdev_mmap(struct loonggpu_bo *bo,
				struct vm_area_struct *vma);
int loonggpu_bo_set_tiling_flags(struct loonggpu_bo *bo, u64 tiling_flags);
void loonggpu_bo_get_tiling_flags(struct loonggpu_bo *bo, u64 *tiling_flags);
int loonggpu_bo_set_metadata (struct loonggpu_bo *bo, void *metadata,
			    uint32_t metadata_size, uint64_t flags);
int loonggpu_bo_get_metadata(struct loonggpu_bo *bo, void *buffer,
			   size_t buffer_size, uint32_t *metadata_size,
			   uint64_t *flags);
void loonggpu_bo_move_notify(struct ttm_buffer_object *bo,
			   bool evict,
			   lg_ttm_mem_t *new_mem);
int loonggpu_bo_fault_reserve_notify(struct ttm_buffer_object *bo);
void loonggpu_bo_fence(struct loonggpu_bo *bo, struct dma_fence *fence,
		     bool shared);
int loonggpu_bo_sync_wait(struct loonggpu_bo *bo, enum loonggpu_sync_mode sync_mode,
                        void *owner, bool intr, bool explicit);
u64 loonggpu_bo_gpu_offset(struct loonggpu_bo *bo);
int loonggpu_bo_backup_to_shadow(struct loonggpu_device *adev,
			       struct loonggpu_ring *ring,
			       struct loonggpu_bo *bo,
			       lg_dma_resv_t *resv,
			       struct dma_fence **fence, bool direct);
int loonggpu_bo_validate(struct loonggpu_bo *bo);
int loonggpu_bo_restore_from_shadow(struct loonggpu_device *adev,
				  struct loonggpu_ring *ring,
				  struct loonggpu_bo *bo,
				  lg_dma_resv_t *resv,
				  struct dma_fence **fence,
				  bool direct);
uint32_t loonggpu_bo_get_preferred_pin_domain(struct loonggpu_device *adev,
					    uint32_t domain);

/*
 * sub allocation
 */

static inline uint64_t loonggpu_sa_bo_gpu_addr(struct loonggpu_sa_bo *sa_bo)
{
	return sa_bo->manager->gpu_addr + sa_bo->soffset;
}

static inline void *loonggpu_sa_bo_cpu_addr(struct loonggpu_sa_bo *sa_bo)
{
	return sa_bo->manager->cpu_ptr + sa_bo->soffset;
}

int loonggpu_sa_bo_manager_init(struct loonggpu_device *adev,
				     struct loonggpu_sa_manager *sa_manager,
				     unsigned size, u32 align, u32 domain);
void loonggpu_sa_bo_manager_fini(struct loonggpu_device *adev,
				      struct loonggpu_sa_manager *sa_manager);
int loonggpu_sa_bo_manager_start(struct loonggpu_device *adev,
				      struct loonggpu_sa_manager *sa_manager);
int loonggpu_sa_bo_new(struct loonggpu_sa_manager *sa_manager,
		     struct loonggpu_sa_bo **sa_bo,
		     unsigned size, unsigned align);
void loonggpu_sa_bo_free(struct loonggpu_device *adev,
			      struct loonggpu_sa_bo **sa_bo,
			      struct dma_fence *fence);
#if defined(CONFIG_DEBUG_FS)
void loonggpu_sa_bo_dump_debug_info(struct loonggpu_sa_manager *sa_manager,
					 struct seq_file *m);
#endif

#endif /* __LOONGGPU_OBJECT_H__ */
