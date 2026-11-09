#ifndef __LOONGGPU_VRAM_MGR_HELPER_H__
#define __LOONGGPU_VRAM_MGR_HELPER_H__

static inline void lg_drm_buddy_free_list(struct lg_buddy *mm,
					struct list_head *objects,
					unsigned int flags)
{
	gpu_buddy_free_list(mm, objects, flags);
}
static inline void lg_drm_buddy_block_trim(struct lg_buddy *mm,
					   u64 *start,
					   u64 new_size,
					   struct list_head *blocks)
{
	gpu_buddy_block_trim(mm, start, new_size, blocks);
}
int loonggpu_vram_mgr_new_buddy(struct ttm_resource_manager *man,
			     struct ttm_buffer_object *tbo,
			     const struct ttm_place *place,
			     struct ttm_resource **res);
void loonggpu_vram_mgr_del_buddy(struct ttm_resource_manager *man,
				struct ttm_resource *res);
void loonggpu_vram_mgr_debug_buddy(struct ttm_resource_manager *man,
				struct drm_printer *printer);
bool loonggpu_vram_mgr_intersects(struct ttm_resource_manager *man,
				struct ttm_resource *res,
				const struct ttm_place *place,
				size_t size);
bool loonggpu_vram_mgr_compatible(struct ttm_resource_manager *man,
				struct ttm_resource *res,
				const struct ttm_place *place,
				size_t size);

static inline struct loonggpu_vram_mgr *lg_man_to_vram_mgr(lg_ttm_manager_t *man)
{
	return container_of(man, struct loonggpu_vram_mgr, manager);
}

static inline struct loonggpu_device *vram_mgr_to_loonggpu_device(struct loonggpu_vram_mgr *mgr)
{
	return container_of(mgr, struct loonggpu_device, mman.vram_mgr);
}

static inline void lg_vram_mgr_init_drv_ptr_used(lg_ttm_manager_t *man, struct loonggpu_vram_mgr *mgr)
{
	ttm_set_driver_manager(&container_of(mgr, struct loonggpu_device, mman.vram_mgr)->mman.bdev,
				TTM_PL_VRAM, &mgr->manager);
	ttm_resource_manager_set_used(man, true);
}

static inline void lg_loonggpu_vram_mgr_fini(struct loonggpu_device *adev)
{
	loonggpu_vram_mgr_fini_buddy(adev);
}

static inline int lg_vram_mgr_fini_clean(lg_ttm_manager_t *man, struct loonggpu_vram_mgr *mgr)
{
	ttm_resource_manager_set_used(man, false);
	return ttm_resource_manager_evict_all(&vram_mgr_to_loonggpu_device(mgr)->mman.bdev, man);
}

static inline void lg_vram_mgr_fini_free(lg_ttm_manager_t *man, struct loonggpu_vram_mgr *mgr)
{
	ttm_resource_manager_cleanup(man);
	ttm_set_driver_manager(&vram_mgr_to_loonggpu_device(mgr)->mman.bdev, TTM_PL_VRAM, NULL);
}

static inline void lg_vram_init_mgr(struct loonggpu_vram_mgr *mgr)
{
	mutex_init(&mgr->m_lock);
	INIT_LIST_HEAD(&mgr->reservations_pending);
	INIT_LIST_HEAD(&mgr->reserved_pages);
	mgr->default_page_size = PAGE_SIZE;
}

static inline void lg_vram_mm_fini(struct loonggpu_vram_mgr *mgr)
{
	mutex_lock(&mgr->m_lock);
	lg_buddy_fini(&mgr->b_mm);
	mutex_unlock(&mgr->m_lock);
}

#define lg_bo_to_mem bo->tbo.resource
u64 loonggpu_vram_mgr_vis_size(struct loonggpu_device *adev,
				struct lg_buddy_block *block);
#endif
