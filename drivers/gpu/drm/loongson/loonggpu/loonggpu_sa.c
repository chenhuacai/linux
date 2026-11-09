#include "loonggpu.h"

static void loonggpu_sa_bo_remove_locked(struct loonggpu_sa_bo *sa_bo);
static void loonggpu_sa_bo_try_free(struct loonggpu_sa_manager *sa_manager);

int loonggpu_sa_bo_manager_init(struct loonggpu_device *adev,
			      struct loonggpu_sa_manager *sa_manager,
			      unsigned size, u32 align, u32 domain)
{
	int i, r;

	init_waitqueue_head(&sa_manager->wq);
	sa_manager->bo = NULL;
	sa_manager->size = size;
	sa_manager->domain = domain;
	sa_manager->align = align;
	sa_manager->hole = &sa_manager->olist;
	INIT_LIST_HEAD(&sa_manager->olist);
	for (i = 0; i < LOONGGPU_SA_NUM_FENCE_LISTS; ++i)
		INIT_LIST_HEAD(&sa_manager->flist[i]);

	r = loonggpu_bo_create_kernel(adev, size, align, domain, &sa_manager->bo,
				&sa_manager->gpu_addr, &sa_manager->cpu_ptr);
	if (r) {
		dev_err(adev->dev, "(%d) failed to allocate bo for manager\n", r);
		return r;
	}

	memset(sa_manager->cpu_ptr, 0, sa_manager->size);
	return r;
}

void loonggpu_sa_bo_manager_fini(struct loonggpu_device *adev,
					struct loonggpu_sa_manager *sa_manager)
{
	struct loonggpu_sa_bo *sa_bo, *tmp;

	if (sa_manager->bo == NULL) {
		dev_err(adev->dev, "no bo for sa manager\n");
		return;
	}

	if (!list_empty(&sa_manager->olist)) {
		sa_manager->hole = &sa_manager->olist,
		loonggpu_sa_bo_try_free(sa_manager);
		if (!list_empty(&sa_manager->olist)) {
			dev_err(adev->dev, "sa_manager is not empty, clearing anyway\n");
		}
	}
	list_for_each_entry_safe(sa_bo, tmp, &sa_manager->olist, olist) {
		loonggpu_sa_bo_remove_locked(sa_bo);
	}

	loonggpu_bo_free_kernel(&sa_manager->bo, &sa_manager->gpu_addr, &sa_manager->cpu_ptr);
	sa_manager->size = 0;
}

static void loonggpu_sa_bo_remove_locked(struct loonggpu_sa_bo *sa_bo)
{
	struct loonggpu_sa_manager *sa_manager = sa_bo->manager;
	if (sa_manager->hole == &sa_bo->olist) {
		sa_manager->hole = sa_bo->olist.prev;
	}
	list_del_init(&sa_bo->olist);
	list_del_init(&sa_bo->flist);
	dma_fence_put(sa_bo->fence);
	kfree(sa_bo);
}

static void loonggpu_sa_bo_try_free(struct loonggpu_sa_manager *sa_manager)
{
	struct loonggpu_sa_bo *sa_bo, *tmp;

	if (sa_manager->hole->next == &sa_manager->olist)
		return;

	sa_bo = list_entry(sa_manager->hole->next, struct loonggpu_sa_bo, olist);
	list_for_each_entry_safe_from(sa_bo, tmp, &sa_manager->olist, olist) {
		if (sa_bo->fence == NULL ||
		    !dma_fence_is_signaled(sa_bo->fence)) {
			return;
		}
		loonggpu_sa_bo_remove_locked(sa_bo);
	}
}

static inline unsigned loonggpu_sa_bo_hole_soffset(struct loonggpu_sa_manager *sa_manager)
{
	struct list_head *hole = sa_manager->hole;

	if (hole != &sa_manager->olist) {
		return list_entry(hole, struct loonggpu_sa_bo, olist)->eoffset;
	}
	return 0;
}

static inline unsigned loonggpu_sa_bo_hole_eoffset(struct loonggpu_sa_manager *sa_manager)
{
	struct list_head *hole = sa_manager->hole;

	if (hole->next != &sa_manager->olist) {
		return list_entry(hole->next, struct loonggpu_sa_bo, olist)->soffset;
	}
	return sa_manager->size;
}

static bool loonggpu_sa_bo_try_alloc(struct loonggpu_sa_manager *sa_manager,
				   struct loonggpu_sa_bo *sa_bo,
				   unsigned size, unsigned align)
{
	unsigned soffset, eoffset, wasted;

	soffset = loonggpu_sa_bo_hole_soffset(sa_manager);
	eoffset = loonggpu_sa_bo_hole_eoffset(sa_manager);
	wasted = (align - (soffset % align)) % align;

	if ((eoffset - soffset) >= (size + wasted)) {
		soffset += wasted;

		sa_bo->manager = sa_manager;
		sa_bo->soffset = soffset;
		sa_bo->eoffset = soffset + size;
		list_add(&sa_bo->olist, sa_manager->hole);
		INIT_LIST_HEAD(&sa_bo->flist);
		sa_manager->hole = &sa_bo->olist;
		return true;
	}
	return false;
}

/**
 * loonggpu_sa_event - Check if we can stop waiting
 *
 * @sa_manager: pointer to the sa_manager
 * @size: number of bytes we want to allocate
 * @align: alignment we need to match
 *
 * Check if either there is a fence we can wait for or
 * enough free memory to satisfy the allocation directly
 */
static bool loonggpu_sa_event(struct loonggpu_sa_manager *sa_manager,
			    unsigned size, unsigned align)
{
	unsigned soffset, eoffset, wasted;
	int i;

	for (i = 0; i < LOONGGPU_SA_NUM_FENCE_LISTS; ++i)
		if (!list_empty(&sa_manager->flist[i]))
			return true;

	soffset = loonggpu_sa_bo_hole_soffset(sa_manager);
	eoffset = loonggpu_sa_bo_hole_eoffset(sa_manager);
	wasted = (align - (soffset % align)) % align;

	if ((eoffset - soffset) >= (size + wasted)) {
		return true;
	}

	return false;
}

static bool loonggpu_sa_bo_next_hole(struct loonggpu_sa_manager *sa_manager,
				   struct dma_fence **fences,
				   unsigned *tries)
{
	struct loonggpu_sa_bo *best_bo = NULL;
	unsigned i, soffset, best, tmp;

	/* if hole points to the end of the buffer */
	if (sa_manager->hole->next == &sa_manager->olist) {
		/* try again with its beginning */
		sa_manager->hole = &sa_manager->olist;
		return true;
	}

	soffset = loonggpu_sa_bo_hole_soffset(sa_manager);
	/* to handle wrap around we add sa_manager->size */
	best = sa_manager->size * 2;
	/* go over all fence list and try to find the closest sa_bo
	 * of the current last
	 */
	for (i = 0; i < LOONGGPU_SA_NUM_FENCE_LISTS; ++i) {
		struct loonggpu_sa_bo *sa_bo;

		fences[i] = NULL;

		if (list_empty(&sa_manager->flist[i]))
			continue;

		sa_bo = list_first_entry(&sa_manager->flist[i],
					 struct loonggpu_sa_bo, flist);

		if (!dma_fence_is_signaled(sa_bo->fence)) {
			fences[i] = sa_bo->fence;
			continue;
		}

		/* limit the number of tries each ring gets */
		if (tries[i] > 2) {
			continue;
		}

		tmp = sa_bo->soffset;
		if (tmp < soffset) {
			/* wrap around, pretend it's after */
			tmp += sa_manager->size;
		}
		tmp -= soffset;
		if (tmp < best) {
			/* this sa bo is the closest one */
			best = tmp;
			best_bo = sa_bo;
		}
	}

	if (best_bo) {
		uint32_t idx = best_bo->fence->context;

		idx %= LOONGGPU_SA_NUM_FENCE_LISTS;
		++tries[idx];
		sa_manager->hole = best_bo->olist.prev;

		/* we knew that this one is signaled,
		   so it's save to remote it */
		loonggpu_sa_bo_remove_locked(best_bo);
		return true;
	}
	return false;
}

int loonggpu_sa_bo_new(struct loonggpu_sa_manager *sa_manager,
		     struct loonggpu_sa_bo **sa_bo,
		     unsigned size, unsigned align)
{
	struct dma_fence *fences[LOONGGPU_SA_NUM_FENCE_LISTS];
	unsigned tries[LOONGGPU_SA_NUM_FENCE_LISTS];
	unsigned count;
	int i, r;
	signed long t;

	if (WARN_ON_ONCE(align > sa_manager->align))
		return -EINVAL;

	if (WARN_ON_ONCE(size > sa_manager->size))
		return -EINVAL;

	*sa_bo = kmalloc(sizeof(struct loonggpu_sa_bo), GFP_KERNEL);
	if (!(*sa_bo))
		return -ENOMEM;
	(*sa_bo)->manager = sa_manager;
	(*sa_bo)->fence = NULL;
	INIT_LIST_HEAD(&(*sa_bo)->olist);
	INIT_LIST_HEAD(&(*sa_bo)->flist);

	spin_lock(&sa_manager->wq.lock);
	do {
		for (i = 0; i < LOONGGPU_SA_NUM_FENCE_LISTS; ++i)
			tries[i] = 0;

		do {
			loonggpu_sa_bo_try_free(sa_manager);

			if (loonggpu_sa_bo_try_alloc(sa_manager, *sa_bo,
						   size, align)) {
				spin_unlock(&sa_manager->wq.lock);
				return 0;
			}

			/* see if we can skip over some allocations */
		} while (loonggpu_sa_bo_next_hole(sa_manager, fences, tries));

		for (i = 0, count = 0; i < LOONGGPU_SA_NUM_FENCE_LISTS; ++i)
			if (fences[i])
				fences[count++] = dma_fence_get(fences[i]);

		if (count) {
			spin_unlock(&sa_manager->wq.lock);
			t = dma_fence_wait_any_timeout(fences, count, false,
						       MAX_SCHEDULE_TIMEOUT,
						       NULL);
			for (i = 0; i < count; ++i)
				dma_fence_put(fences[i]);

			r = (t > 0) ? 0 : t;
			spin_lock(&sa_manager->wq.lock);
		} else {
			/* if we have nothing to wait for block */
			r = wait_event_interruptible_locked(
				sa_manager->wq,
				loonggpu_sa_event(sa_manager, size, align)
			);
		}

	} while (!r);

	spin_unlock(&sa_manager->wq.lock);
	kfree(*sa_bo);
	*sa_bo = NULL;
	return r;
}

void loonggpu_sa_bo_free(struct loonggpu_device *adev, struct loonggpu_sa_bo **sa_bo,
		       struct dma_fence *fence)
{
	struct loonggpu_sa_manager *sa_manager;

	if (sa_bo == NULL || *sa_bo == NULL) {
		return;
	}

	sa_manager = (*sa_bo)->manager;
	spin_lock(&sa_manager->wq.lock);
	if (fence && !dma_fence_is_signaled(fence)) {
		uint32_t idx;

		(*sa_bo)->fence = dma_fence_get(fence);
		idx = fence->context % LOONGGPU_SA_NUM_FENCE_LISTS;
		list_add_tail(&(*sa_bo)->flist, &sa_manager->flist[idx]);
	} else {
		loonggpu_sa_bo_remove_locked(*sa_bo);
	}
	wake_up_all_locked(&sa_manager->wq);
	spin_unlock(&sa_manager->wq.lock);
	*sa_bo = NULL;
}

#if defined(CONFIG_DEBUG_FS)

void loonggpu_sa_bo_dump_debug_info(struct loonggpu_sa_manager *sa_manager,
				  struct seq_file *m)
{
	struct loonggpu_sa_bo *i;

	spin_lock(&sa_manager->wq.lock);
	list_for_each_entry(i, &sa_manager->olist, olist) {
		uint64_t soffset = i->soffset + sa_manager->gpu_addr;
		uint64_t eoffset = i->eoffset + sa_manager->gpu_addr;
		if (&i->olist == sa_manager->hole) {
			seq_printf(m, ">");
		} else {
			seq_printf(m, " ");
		}
		seq_printf(m, "[0x%010llx 0x%010llx] size %8lld",
			   soffset, eoffset, eoffset - soffset);

		if (i->fence)
			seq_printf(m, " protected by 0x%016llx on context %llu",
				   (u64)i->fence->seqno, i->fence->context);

		seq_printf(m, "\n");
	}
	spin_unlock(&sa_manager->wq.lock);
}
#endif
