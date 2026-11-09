#ifndef LOONGGPU_BO_PIN_HELPER_H
#define LOONGGPU_BO_PIN_HELPER_H

static inline int lg_bo_pin_count(struct loonggpu_bo *bo)
{
	return bo->tbo.pin_count;
}

#define lg_bo_pin_count_ptr bo->tbo.pin_count

static inline void lg_ttm_bo_pin(struct loonggpu_bo *bo)
{
	ttm_bo_pin(&bo->tbo);
}

static inline void lg_bo_pin(struct loonggpu_bo *bo)
{
	ttm_bo_pin(&bo->tbo);
}

static inline void lg_ttm_bo_unpin(struct loonggpu_bo *bo)
{
	ttm_bo_unpin(&bo->tbo);
}

#endif /* LOONGGPU_BO_PIN_HELPER_H */
