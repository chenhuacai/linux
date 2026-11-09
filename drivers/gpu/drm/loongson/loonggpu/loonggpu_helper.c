#include "loonggpu.h"


lg_dma_resv_t *lg_loonggpu_gem_prime_res_obj(struct drm_gem_object *obj)
{
	struct loonggpu_bo *bo = gem_to_loonggpu_bo(obj);

	return to_dma_resv(bo);
}

struct dma_buf *lg_loonggpu_gem_prime_export(
                                          struct drm_gem_object *gobj,
					  int flags)
{
	return loonggpu_gem_prime_export(gobj, flags);
}
