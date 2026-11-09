#include "loonggpu.h"
#include "loonggpu_trace.h"
#include "loonggpu_lgkcd.h"
#include "loonggpu_sync_helper.h"
struct loonggpu_sync_entry {
	struct hlist_node	node;
	struct dma_fence	*fence;
	bool	explicit;
};

static struct kmem_cache *loonggpu_sync_slab;

/**
 * loonggpu_sync_create - zero init sync object
 *
 * @sync: sync object to initialize
 *
 * Just clear the sync object for now.
 */
void loonggpu_sync_create(struct loonggpu_sync *sync)
{
	hash_init(sync->fences);
	sync->last_vm_update = NULL;
}

/**
 * loonggpu_sync_same_dev - test if fence belong to us
 *
 * @adev: loonggpu device to use for the test
 * @f: fence to test
 *
 * Test if the fence was issued by us.
 */
bool loonggpu_sync_same_dev(struct loonggpu_device *adev,
				 struct dma_fence *f)
{
	struct drm_sched_fence *s_fence = to_drm_sched_fence(f);

	if (s_fence) {
		struct loonggpu_ring *ring;

		ring = container_of(s_fence->sched, struct loonggpu_ring, sched);
		return ring->adev == adev;
	}

	return false;
}

/**
 * loonggpu_sync_get_owner - extract the owner of a fence
 *
 * @fence: fence get the owner from
 *
 * Extract who originally created the fence.
 */
void *loonggpu_sync_get_owner(struct dma_fence *f)
{
	struct drm_sched_fence *s_fence;
	struct loonggpu_lgkcd_fence *kcd_fence;

	if (!f)
		return LOONGGPU_FENCE_OWNER_UNDEFINED;

	s_fence = to_drm_sched_fence(f);
	if (s_fence)
		return s_fence->owner;

	kcd_fence = to_loonggpu_lgkcd_fence(f);
	if (kcd_fence)
		return LOONGGPU_FENCE_OWNER_KCD;

	return LOONGGPU_FENCE_OWNER_UNDEFINED;
}

/**
 * loonggpu_sync_keep_later - Keep the later fence
 *
 * @keep: existing fence to test
 * @fence: new fence
 *
 * Either keep the existing fence or the new one, depending which one is later.
 */
static void loonggpu_sync_keep_later(struct dma_fence **keep,
				   struct dma_fence *fence)
{
	if (*keep && dma_fence_is_later(*keep, fence))
		return;

	dma_fence_put(*keep);
	*keep = dma_fence_get(fence);
}

/**
 * loonggpu_sync_add_later - add the fence to the hash
 *
 * @sync: sync object to add the fence to
 * @f: fence to add
 *
 * Tries to add the fence to an existing hash entry. Returns true when an entry
 * was found, false otherwise.
 */
static bool loonggpu_sync_add_later(struct loonggpu_sync *sync, struct dma_fence *f, bool explicit)
{
	struct loonggpu_sync_entry *e;

	hash_for_each_possible(sync->fences, e, node, f->context) {
		if (unlikely(e->fence->context != f->context))
			continue;

		loonggpu_sync_keep_later(&e->fence, f);

		/* Preserve eplicit flag to not loose pipe line sync */
		e->explicit |= explicit;

		return true;
	}
	return false;
}

/**
 * loonggpu_sync_fence - remember to sync to this fence
 *
 * @sync: sync object to add fence to
 * @fence: fence to sync to
 *
 */
int loonggpu_sync_fence(struct loonggpu_device *adev, struct loonggpu_sync *sync,
		      struct dma_fence *f, bool explicit)
{
	struct loonggpu_sync_entry *e;

	if (!f)
		return 0;
	if (loonggpu_sync_same_dev(adev, f) &&
	    loonggpu_sync_get_owner(f) == LOONGGPU_FENCE_OWNER_VM)
		loonggpu_sync_keep_later(&sync->last_vm_update, f);

	if (loonggpu_sync_add_later(sync, f, explicit))
		return 0;

	e = kmem_cache_alloc(loonggpu_sync_slab, GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	e->explicit = explicit;

	hash_add(sync->fences, &e->node, f->context);
	e->fence = dma_fence_get(f);
	return 0;
}

/**
 * loonggpu_sync_resv - sync to a reservation object
 *
 * @sync: sync object to add fences from reservation object to
 * @resv: reservation object with embedded fence
 * @explicit_sync: true if we should only sync to the exclusive fence
 *
 * Sync to the fence
 */
int loonggpu_sync_resv(struct loonggpu_device *adev,
		     struct loonggpu_sync *sync,
		     lg_dma_resv_t *resv,
		     enum loonggpu_sync_mode mode,
		     void *owner, bool explicit_sync)
{
	if (resv == NULL)
		return -EINVAL;

	return lg_loonggpu_sync_resv(adev, sync, resv, mode, owner, explicit_sync);
}

/**
 * loonggpu_sync_peek_fence - get the next fence not signaled yet
 *
 * @sync: the sync object
 * @ring: optional ring to use for test
 *
 * Returns the next fence not signaled yet without removing it from the sync
 * object.
 */
struct dma_fence *loonggpu_sync_peek_fence(struct loonggpu_sync *sync,
					 struct loonggpu_ring *ring)
{
	struct loonggpu_sync_entry *e;
	struct hlist_node *tmp;
	int i;

	hash_for_each_safe(sync->fences, i, tmp, e, node) {
		struct dma_fence *f = e->fence;
		struct drm_sched_fence *s_fence = to_drm_sched_fence(f);

		if (dma_fence_is_signaled(f)) {
			hash_del(&e->node);
			dma_fence_put(f);
			kmem_cache_free(loonggpu_sync_slab, e);
			continue;
		}
		if (ring && s_fence) {
			/* For fences from the same ring it is sufficient
			 * when they are scheduled.
			 */
			if (s_fence->sched == &ring->sched) {
				if (dma_fence_is_signaled(&s_fence->scheduled))
					continue;

				return &s_fence->scheduled;
			}
		}

		return f;
	}

	return NULL;
}

/**
 * loonggpu_sync_get_fence - get the next fence from the sync object
 *
 * @sync: sync object to use
 * @explicit: true if the next fence is explicit
 *
 * Get and removes the next fence from the sync object not signaled yet.
 */
struct dma_fence *loonggpu_sync_get_fence(struct loonggpu_sync *sync, bool *explicit)
{
	struct loonggpu_sync_entry *e;
	struct hlist_node *tmp;
	struct dma_fence *f;
	int i;
	hash_for_each_safe(sync->fences, i, tmp, e, node) {

		f = e->fence;
		if (explicit)
			*explicit = e->explicit;

		hash_del(&e->node);
		kmem_cache_free(loonggpu_sync_slab, e);

		if (!dma_fence_is_signaled(f))
			return f;

		dma_fence_put(f);
	}
	return NULL;
}

/**
 * loonggpu_sync_clone - clone a sync object
 *
 * @source: sync object to clone
 * @clone: pointer to destination sync object
 *
 * Adds references to all unsignaled fences in @source to @clone. Also
 * removes signaled fences from @source while at it.
 */
int loonggpu_sync_clone(struct loonggpu_sync *source, struct loonggpu_sync *clone)
{
	struct loonggpu_sync_entry *e;
	struct hlist_node *tmp;
	struct dma_fence *f;
	int i, r;

	hash_for_each_safe(source->fences, i, tmp, e, node) {
		f = e->fence;
		if (!dma_fence_is_signaled(f)) {
			r = loonggpu_sync_fence(NULL, clone, f, e->explicit);
			if (r)
				return r;
		} else {
			hash_del(&e->node);
			dma_fence_put(f);
			kmem_cache_free(loonggpu_sync_slab, e);
		}
	}

	dma_fence_put(clone->last_vm_update);
	clone->last_vm_update = dma_fence_get(source->last_vm_update);

	return 0;
}

int loonggpu_sync_wait(struct loonggpu_sync *sync, bool intr)
{
	struct loonggpu_sync_entry *e;
	struct hlist_node *tmp;
	int i, r;

	hash_for_each_safe(sync->fences, i, tmp, e, node) {
		r = dma_fence_wait(e->fence, intr);
		if (r)
			return r;

		hash_del(&e->node);
		dma_fence_put(e->fence);
		kmem_cache_free(loonggpu_sync_slab, e);
	}

	return 0;
}

/**
 * loonggpu_sync_free - free the sync object
 *
 * @sync: sync object to use
 *
 * Free the sync object.
 */
void loonggpu_sync_free(struct loonggpu_sync *sync)
{
	struct loonggpu_sync_entry *e;
	struct hlist_node *tmp;
	unsigned i;

	hash_for_each_safe(sync->fences, i, tmp, e, node) {
		hash_del(&e->node);
		dma_fence_put(e->fence);
		kmem_cache_free(loonggpu_sync_slab, e);
	}

	dma_fence_put(sync->last_vm_update);
}

/**
 * loonggpu_sync_init - init sync object subsystem
 *
 * Allocate the slab allocator.
 */
int loonggpu_sync_init(void)
{
	loonggpu_sync_slab = kmem_cache_create(
		"loonggpu_sync", sizeof(struct loonggpu_sync_entry), 0,
		SLAB_HWCACHE_ALIGN, NULL);
	if (!loonggpu_sync_slab)
		return -ENOMEM;

	return 0;
}

/**
 * loonggpu_sync_fini - fini sync object subsystem
 *
 * Free the slab allocator.
 */
void loonggpu_sync_fini(void)
{
	kmem_cache_destroy(loonggpu_sync_slab);
}
