#ifndef __LOONGGPU_TTM_HELPER_H__
#define __LOONGGPU_TTM_HELPER_H__
#include "loonggpu_ttm_resource_helper.h"
#include <drm/ttm/ttm_tt.h>
#include <drm/ttm/ttm_device.h>

unsigned long loonggpu_ttm_io_mem_pfn_buddy(struct ttm_buffer_object *bo,
					unsigned long page_offset);

struct loonggpu_vram_mgr_resource {
	lg_ttm_mem_t base;
	struct list_head blocks;
	unsigned long flags;
};

struct loonggpu_res_cursor {
	uint64_t	start;
	uint64_t	size;
	uint64_t	remaining;
	void		*node;
	uint32_t	mem_type;
};

struct ttm_bo_global_ref;
struct drm_global_reference;
struct loonggpu_device;

typedef struct ttm_device lg_ttm_device_t;

#define lg_define_ttm_backend_func
#define lg_ttm_backend_func_arg lg_ttm_device_t *bdev, struct ttm_tt *ttm
#define lg_ttm_backend_func_pass_param bdev, ttm
#define lg_ttm_backend_func_get_bdev bdev
#define lg_ttm_backend_unbind_ret void
#define lg_ttm_backend_unbind_return(ret) return
#define lg_ttm_set_backend_func
#define lg_loonggpu_ttm_backend_unbind loonggpu_ttm_backend_unbind(bdev, ttm);

static inline void lg_ttm_dma_tt_fini(struct ttm_tt *ttm)
{
	ttm_tt_fini(ttm);
}

int loonggpu_ttm_backend_bind(lg_ttm_backend_func_arg, lg_ttm_mem_t *bo_mem);

/*
 * fix case 1/10/13
 */
#define DRM_FILE_PAGE_OFFSET (0x100000000ULL >> PAGE_SHIFT)

static inline int lg_ttm_bo_pipeline_move(struct ttm_buffer_object *bo,
					  struct dma_fence *fence,
					  bool evict, lg_ttm_mem_t *new_mem)
{
	int r;

	if (bo->type == ttm_bo_type_kernel)
		r = ttm_bo_move_accel_cleanup(bo, fence, true, false, new_mem);
	else
		r = ttm_bo_move_accel_cleanup(bo, fence, evict, true, new_mem);

	return r;
}

static inline int lg_ttm_tt_bind(struct ttm_buffer_object *bo, lg_ttm_mem_t *mem,
				 struct ttm_operation_ctx *ctx)
{
	int r;
	r = ttm_bo_populate(bo, ctx);
	if (r)
		return r;
	r = loonggpu_ttm_backend_bind(bo->bdev, bo->ttm, mem);
	if (r)
		return r;
	return r;
}

static inline void lg_ttm_bo_mem_put(struct ttm_buffer_object *bo, lg_ttm_mem_t *mem)
{
	ttm_resource_free(bo, &mem);
}

static inline void lg_set_bus_placement_size(lg_ttm_mem_t *mem, unsigned long size)
{
}

static inline void lg_add_bus_placement_offset(lg_ttm_mem_t *mem, phys_addr_t offset)
{
	mem->bus.offset += offset;
}

static inline long lg_get_user_pages_remote(struct task_struct *tsk, struct mm_struct *mm,
					unsigned long start, unsigned long nr_pages,
					unsigned int gup_flags, struct page **pages,
					struct vm_area_struct **vmas, int *locked)
{
	return get_user_pages_remote(mm, start, nr_pages, gup_flags, pages, locked);
}

#define lg_ttm_tt_populate_arg lg_ttm_device_t *bdev, struct ttm_tt *ttm

#define lg_ttm_bo_driver_set_bind_unbind_destroy	.ttm_tt_bind = &loonggpu_ttm_backend_bind,     \
							.ttm_tt_unbind = &loonggpu_ttm_backend_unbind, \
							.ttm_tt_destroy = &loonggpu_ttm_backend_destroy,

#define lg_set_io_mem_pfn .io_mem_pfn = loonggpu_ttm_io_mem_pfn_buddy,
#define lg_fill_buffer(bo, src_data, resv, f, delayed) loonggpu_fill_buffer_buddy(bo, src_data, resv, f, delayed);

#define lg_buddy gpu_buddy
#define lg_buddy_block gpu_buddy_block
#define lg_buddy_block_offset gpu_buddy_block_offset
#define lg_buddy_block_order gpu_buddy_block_order
#define lg_buddy_alloc_blocks gpu_buddy_alloc_blocks
#define lg_buddy_fini gpu_buddy_fini
#define lg_buddy_init gpu_buddy_init
#define LG_BUDDY_TOPDOWN_ALLOCATION GPU_BUDDY_TOPDOWN_ALLOCATION
#define LG_BUDDY_RANGE_ALLOCATION GPU_BUDDY_RANGE_ALLOCATION

void loonggpu_res_first(struct ttm_resource *res,
		     uint64_t start, uint64_t size,
		     struct loonggpu_res_cursor *cur);
void loonggpu_res_next(struct loonggpu_res_cursor *cur, uint64_t size);

static inline u64
loonggpu_vram_mgr_block_start(struct lg_buddy_block *block)
{
	return lg_buddy_block_offset(block);
}

static inline u64
loonggpu_vram_mgr_block_size(struct lg_buddy_block *block)
{
	return (u64)PAGE_SIZE << lg_buddy_block_order(block);
}

static inline struct loonggpu_vram_mgr_resource *
to_loonggpu_vram_mgr_resource(struct ttm_resource *res)
{
	return container_of(res, struct loonggpu_vram_mgr_resource, base);
}

static inline int lg_ttm_sg_tt_init(
				    struct ttm_tt *ttm,
				    u64	abo_flags,
				    struct ttm_buffer_object *bo,
				    uint32_t page_flags, bool wc_support)
{
	enum ttm_caching caching;
	if (abo_flags & LOONGGPU_GEM_CREATE_CPU_GTT_USWC && wc_support)
		caching = ttm_write_combined;
	else
		caching = ttm_cached;

	/* allocate space for the uninitialized page entries */
	return ttm_sg_tt_init(ttm, bo, page_flags, caching);
}

#define lg_drm_prime_sg_to_addr_array	drm_prime_sg_to_dma_addr_array(ttm->sg, \
					gtt->ttm.dma_address, \
					ttm->num_pages); \
					drm_prime_sg_to_page_array(ttm->sg, \
					ttm->pages, \
					ttm->num_pages)

static inline int lg_ttm_bo_evict_mm(lg_ttm_device_t *bdev, int type)
{
	return ttm_resource_manager_evict_all(bdev, ttm_manager_type(bdev, type));
}

static inline int lg_ttm_bo_mem_space(struct ttm_buffer_object *bo, struct ttm_placement *placement,
				     lg_ttm_mem_t *mem, struct ttm_operation_ctx *ctx)
{
	return ttm_bo_mem_space(bo, placement, &mem, ctx);
}

#endif /* __LOONGGPU_TTM_HELPER_H__ */
