#ifndef __LOONGGPU_GTT_MGR_HELPER_H__
#define __LOONGGPU_GTT_MGR_HELPER_H__

struct loonggpu_gtt_node {
	struct drm_mm_node node;
	struct ttm_buffer_object *tbo;
};

static inline lg_ttm_mem_t *lg_tbo_to_mem(struct ttm_buffer_object *tbo)
{
	return tbo->resource;
}

static inline void lg_tbo_set_mem(struct ttm_buffer_object *tbo, lg_ttm_mem_t *new_mem)
{
	tbo->resource = new_mem;
}

static inline struct loonggpu_device *lg_gtt_mgr_man_to_adev(lg_ttm_manager_t *man,
						struct loonggpu_gtt_mgr *gtt_mgr)
{
	return container_of(gtt_mgr, struct loonggpu_device, mman.gtt_mgr);
}

#define lg_gtt_mgr_set_funcs	.alloc = loonggpu_gtt_mgr_new, \
				.free = loonggpu_gtt_mgr_del, \
				.debug = loonggpu_gtt_mgr_debug

static inline lg_ttm_manager_t *lg_bdev_to_ttm_man(lg_ttm_device_t *bdev, int mem_type)
{
	return ttm_manager_type(bdev, mem_type);
}

static inline uint64_t lg_bdev_to_gpu_offset(lg_ttm_device_t *bdev, int mem_type)
{
	return loonggpu_ttm_domain_start(loonggpu_ttm_adev(bdev), mem_type);
}

static inline struct loonggpu_gtt_mgr *lg_man_to_gtt_mgr(lg_ttm_manager_t *man)
{
	return container_of(man, struct loonggpu_gtt_mgr, manager);
}

#define lg_gtt_mgr_init_mgr(man, mgr)  mgr = container_of(man, struct loonggpu_gtt_mgr, manager);

static inline void lg_gtt_mgr_func_setting(lg_ttm_manager_t *man,
					   struct loonggpu_device *adev,
                                           const lg_ttm_mem_func_t *loonggpu_gtt_mgr_func,
					   unsigned long size)
{
	man->use_tt = true;
	man->func = loonggpu_gtt_mgr_func;
	ttm_resource_manager_init(man, &adev->mman.bdev, size);
}

static inline void lg_ttm_resource_manager_init(lg_ttm_manager_t *man,
                                                struct loonggpu_device *adev,
                                                unsigned long size)
{
	ttm_resource_manager_init(man, &adev->mman.bdev, size);
}

static inline void lg_gtt_mgr_init_set_man_and_used(lg_ttm_manager_t *man,
					lg_ttm_device_t *bdev)
{
	ttm_set_driver_manager(bdev, TTM_PL_TT, man);
	ttm_resource_manager_set_used(man, true);
}

static inline int lg_gtt_mgr_init(struct loonggpu_device *adev, uint64_t gtt_size)
{
	return loonggpu_gtt_mgr_init(&adev->mman.gtt_mgr.manager, gtt_size);
}

static inline int lg_gtt_mgr_fini_set_used_clean_list(lg_ttm_manager_t *man,
					lg_ttm_device_t *bdev)
{
	ttm_resource_manager_set_used(man, false);
	return ttm_resource_manager_evict_all(bdev, man);
}

static inline void lg_gtt_mgr_fini_clean_up(lg_ttm_manager_t *man,
					lg_ttm_device_t *bdev)
{
	ttm_resource_manager_cleanup(man);
	ttm_set_driver_manager(bdev, TTM_PL_TT, NULL);
}

static inline void lg_loonggpu_gtt_mgr_fini(struct loonggpu_device *adev)
{
	loonggpu_gtt_mgr_fini(&adev->mman.gtt_mgr.manager);
}

static inline void lg_ttm_bo_clean_mm(struct loonggpu_device *adev, int type)
{
	ttm_range_man_fini(&adev->mman.bdev, type);
}

#define LG_GTT_MGR_NEW_RET_VAL -ENOSPC

static inline phys_addr_t lg_get_bus_placement_base(lg_ttm_mem_t *mem, struct loonggpu_device *adev)
{
	return adev->gmc.aper_base;
}

static inline uint64_t lg_loonggpu_ttm_alloc_gart_get_bo_offset(struct loonggpu_device *adev,
                                                            struct ttm_buffer_object *bo)
{
	return (lg_tbo_to_mem(bo)->start << PAGE_SHIFT) +
		loonggpu_ttm_domain_start(adev, lg_tbo_to_mem(bo)->mem_type);
}

#define lg_loonggpu_gtt_node struct ttm_range_mgr_node

#define lg_gtt_mgr_new_mem_reg lg_ttm_mem_t **mem
#define lg_gtt_mgr_alloc_arg &gtt_node->base
#define lg_gtt_mgr_set_res_ptr *mem = &gtt_node->base;
#define lg_gtt_get_pages(tbo, mem) PFN_UP(tbo->base.size)

#define lg_alloc_gtt_node(gtt_node)\
	gtt_node = kzalloc(struct_size(gtt_node, mm_nodes, 1), GFP_KERNEL);


static inline lg_loonggpu_gtt_node *lg_res_to_gtt_node(lg_ttm_mem_t *mem)
{
	return to_ttm_range_mgr_node(mem);
}

static inline struct drm_mm_node *lg_res_to_drm_node(lg_ttm_mem_t *mem)
{
	return &to_ttm_range_mgr_node(mem)->mm_nodes[0];
}

static inline void lg_clear_drm_node(struct drm_mm_node *node)
{
	memset(node, 0, sizeof(*node));
}

static inline struct drm_mm_node *lg_gtt_node_to_drm_node(lg_loonggpu_gtt_node *gtt_node)
{
	return &gtt_node->mm_nodes[0];
}

#define lg_gtt_node_set_tbo(tbo)
#define lg_gtt_node_set_mm_node(node)
#define lg_gtt_node_set_mm_start(val) gtt_node->base.start = val;
#define lg_gtt_del_mgr_num_pages mm_node->size
#define lg_gtt_mgr_new_check_mem tbo->resource

static inline void lg_ttm_resource_init(struct ttm_buffer_object *bo,
					const struct ttm_place *place,
					lg_loonggpu_gtt_node *node)
{
	ttm_resource_init(bo, place, &node->base);
}

static inline void lg_ttm_resource_fini(lg_ttm_manager_t *man,
					lg_loonggpu_gtt_node *node)
{
	ttm_resource_fini(man, &node->base);
}

static inline lg_loonggpu_gtt_node *lg_drm_node_to_gtt_node(struct drm_mm_node *mm_node)
{
	return container_of(mm_node, struct ttm_range_mgr_node, mm_nodes[0]);
}

static inline struct ttm_buffer_object *lg_gtt_node_to_tbo(lg_loonggpu_gtt_node *node)
{
	return node->base.bo;
}

static inline int lg_ttm_resource_manager_evict_all(struct loonggpu_device *adev, int type)
{
	lg_ttm_manager_t *man = lg_bdev_to_ttm_man(&adev->mman.bdev, type);
	return ttm_resource_manager_evict_all(&adev->mman.bdev, man);
}

#endif /* __LOONGGPU_GTT_MGR_HELPER_H__ */
