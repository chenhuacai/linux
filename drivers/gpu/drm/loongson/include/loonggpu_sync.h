#ifndef __LOONGGPU_SYNC_H__
#define __LOONGGPU_SYNC_H__

#include <linux/hashtable.h>

#include "loonggpu_dma_resv_helper.h"

struct dma_fence;
struct loonggpu_device;
struct loonggpu_ring;
struct dma_resv;
struct loonggpu_job;

enum loonggpu_sync_mode {
	LOONGGPU_SYNC_ALWAYS,
	LOONGGPU_SYNC_NE_OWNER,
	LOONGGPU_SYNC_EQ_OWNER,
	LOONGGPU_SYNC_EXPLICIT
};

/*
 * Container for fences used to sync command submissions.
 */
struct loonggpu_sync {
	DECLARE_HASHTABLE(fences, 4);
	struct dma_fence	*last_vm_update;
};

void loonggpu_sync_create(struct loonggpu_sync *sync);
int loonggpu_sync_fence(struct loonggpu_device *adev, struct loonggpu_sync *sync,
		      struct dma_fence *f, bool explicit);
int loonggpu_sync_resv(struct loonggpu_device *adev,
		     struct loonggpu_sync *sync,
		     lg_dma_resv_t *resv,
		     enum loonggpu_sync_mode mode,
		     void *owner,
		     bool explicit_sync);
struct dma_fence *loonggpu_sync_peek_fence(struct loonggpu_sync *sync,
				     struct loonggpu_ring *ring);
struct dma_fence *loonggpu_sync_get_fence(struct loonggpu_sync *sync, bool *explicit);
int loonggpu_sync_clone(struct loonggpu_sync *source, struct loonggpu_sync *clone);
int loonggpu_sync_wait(struct loonggpu_sync *sync, bool intr);
void loonggpu_sync_free(struct loonggpu_sync *sync);
int loonggpu_sync_init(void);
void loonggpu_sync_fini(void);

#endif /* __LOONGGPU_SYNC_H__ */
