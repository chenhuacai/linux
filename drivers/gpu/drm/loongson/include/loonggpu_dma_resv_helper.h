#ifndef LOONGGPU_DMA_RESV_HELPER_H
#define LOONGGPU_DMA_RESV_HELPER_H

#include <linux/dma-resv.h>

typedef struct dma_resv lg_dma_resv_t;
typedef struct dma_resv_list lg_dma_resv_list_t;

#define lg_dma_resv_held(resv) dma_resv_held(resv)

static inline lg_dma_resv_list_t *
lg_dma_resv_get_list(lg_dma_resv_t *resv)
{
	return NULL;
}

static inline struct dma_fence *
lg_dma_resv_get_excl(lg_dma_resv_t *obj)
{
	return NULL;
}

static inline int
lg_dma_resv_reserve_shared(lg_dma_resv_t *obj, unsigned int num_fences)
{
	return dma_resv_reserve_fences(obj, num_fences);
}

static inline int lg_dma_resv_lock(lg_dma_resv_t *obj,
                                    struct ww_acquire_ctx *ctx)
{
    return dma_resv_lock(obj, ctx);
}

static inline void lg_dma_resv_unlock(lg_dma_resv_t *obj)
{
    dma_resv_unlock(obj);
}

static inline bool __must_check
lg_dma_resv_trylock(lg_dma_resv_t *obj)
{
    return dma_resv_trylock(obj);
}

static inline long
lg_dma_resv_wait_timeout_rcu(lg_dma_resv_t *obj, int wait_flag, bool wait_all,
			     bool intr, unsigned long timeout)
{
	return dma_resv_wait_timeout(obj, (enum dma_resv_usage)wait_flag, intr, timeout);
}

static inline int
lg_dma_resv_get_fences_rcu(lg_dma_resv_t *obj, struct dma_fence **pfence_excl,
			   unsigned *pshared_count, struct dma_fence ***pshared,
			   int usage)
{
	return dma_resv_get_fences(obj, (enum dma_resv_usage)usage,
					pshared_count, pshared);
}

static inline void
lg_dma_resv_add_shared_fence(lg_dma_resv_t *obj, struct dma_fence *fence)
{
	dma_resv_add_fence(obj, fence, DMA_RESV_USAGE_READ);
}

static inline void
lg_dma_resv_add_excl_fence(lg_dma_resv_t *obj, struct dma_fence *fence)
{
	dma_resv_add_fence(obj, fence, DMA_RESV_USAGE_WRITE);
}

#endif
