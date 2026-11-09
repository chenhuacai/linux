#ifndef __LOONGGPU_TTM_RESOURCE_HELPER_H__
#define __LOONGGPU_TTM_RESOURCE_HELPER_H__

typedef struct ttm_resource_manager lg_ttm_manager_t;
typedef struct ttm_resource lg_ttm_mem_t;
typedef struct ttm_resource_manager_func lg_ttm_mem_func_t;
#define LG_LOONGGPU_VRAM_GTT_MGR_COMPAT_MEMBERS struct loonggpu_vram_mgr vram_mgr; \
                                             struct loonggpu_gtt_mgr gtt_mgr;
#define lg_loonggpu_vram_mgr_man struct ttm_resource_manager manager;
#define lg_loonggpu_gtt_mgr_man struct ttm_resource_manager manager;

#endif /* __LOONGGPU_TTM_RESOURCE_HELPER_H__ */
