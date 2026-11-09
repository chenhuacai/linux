#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <drm/drm_drv.h>
#include <drm/gpu_scheduler.h>
#include "loonggpu.h"
#include "loonggpu_trace.h"
#include "loonggpu_helper.h"
#include "loonggpu_helper.h"
#include <linux/xarray.h>

static lg_loonggpu_job_timedout_ret loonggpu_job_timedout(struct drm_sched_job *s_job)
{
	struct loonggpu_ring *ring = to_loonggpu_ring(s_job->sched);
	struct loonggpu_job *job = to_loonggpu_job(s_job);

	DRM_ERROR("gpu %s ring %s timeout, signaled seq=%u, emitted seq=%u\n",
		  loonggpu_family_name[ring->adev->family_type],
		  job->base.sched->name, atomic_read(&ring->fence_drv.last_seq),
		  ring->fence_drv.sync_seq);

	loonggpu_device_gpu_recover(ring->adev, job, false);
	return LG_DRM_GPU_SCHED_STAT_NOMINAL;
}

int loonggpu_job_alloc(struct loonggpu_device *adev, unsigned num_ibs,
		     struct loonggpu_job **job, struct loonggpu_vm *vm)
{
	size_t size = sizeof(struct loonggpu_job);

	if (num_ibs == 0)
		return -EINVAL;

	size += sizeof(struct loonggpu_ib) * num_ibs;

	*job = kzalloc(size, GFP_KERNEL);
	if (!*job)
		return -ENOMEM;

	/*
	 * Initialize the scheduler to at least some ring so that we always
	 * have a pointer to adev.
	 */
	(*job)->base.sched = &adev->rings[0]->sched;
	(*job)->vm = vm;
	(*job)->ibs = (void *)&(*job)[1];
	(*job)->num_ibs = num_ibs;

	loonggpu_sync_create(&(*job)->sync);
	loonggpu_sync_create(&(*job)->sched_sync);
	(*job)->vram_lost_counter = atomic_read(&adev->vram_lost_counter);
	(*job)->vm_pd_addr = LOONGGPU_BO_INVALID_OFFSET;

	xa_init_flags(&(*job)->base.dependencies, XA_FLAGS_ALLOC);
	return 0;
}

int loonggpu_job_alloc_with_ib(struct loonggpu_device *adev, unsigned size,
			     struct loonggpu_job **job)
{
	int r;

	r = loonggpu_job_alloc(adev, 1, job, NULL);
	if (r)
		return r;

	r = loonggpu_ib_get(adev, NULL, size, &(*job)->ibs[0]);
	if (r) {
		drm_sched_job_cleanup(&(*job)->base);
		kfree(*job);
	} else
		(*job)->vm_pd_addr = adev->gart.table_addr;

	return r;
}

void loonggpu_job_free_resources(struct loonggpu_job *job)
{
	struct loonggpu_ring *ring = to_loonggpu_ring(job->base.sched);
	struct dma_fence *f;
	unsigned i;

	/* use sched fence if available */
	f = job->base.s_fence ? &job->base.s_fence->finished : job->fence;

	for (i = 0; i < job->num_ibs; ++i)
		loonggpu_ib_free(ring->adev, &job->ibs[i], f);
}

static void loonggpu_job_free_cb(struct drm_sched_job *s_job)
{
	struct loonggpu_ring *ring = to_loonggpu_ring(s_job->sched);
	struct loonggpu_job *job = to_loonggpu_job(s_job);

	lg_drm_sched_job_cleanup(s_job);

	loonggpu_ring_priority_put(ring, s_job->s_priority);
	dma_fence_put(job->fence);
	loonggpu_sync_free(&job->sync);
	loonggpu_sync_free(&job->sched_sync);
	kfree(job);
}

void loonggpu_job_free(struct loonggpu_job *job)
{
	loonggpu_job_free_resources(job);

	dma_fence_put(job->fence);
	loonggpu_sync_free(&job->sync);
	loonggpu_sync_free(&job->sched_sync);
	kfree(job);
}

int loonggpu_job_submit(struct loonggpu_job *job, struct drm_sched_entity *entity,
		      void *owner, struct dma_fence **f)
{
	enum drm_sched_priority priority;
	struct loonggpu_ring *ring;
	int r;

	if (!f)
		return -EINVAL;

	r = lg_drm_sched_job_init(&job->base, entity, 1, owner, 0);
	if (r)
		return r;
	lg_drm_sched_job_arm(&job->base);

	job->owner = owner;
	*f = dma_fence_get(&job->base.s_fence->finished);
	loonggpu_job_free_resources(job);
	priority = job->base.s_priority;
	lg_drm_sched_entity_push_job(&job->base, entity);

	ring = to_loonggpu_ring(entity->rq->sched);
	loonggpu_ring_priority_get(ring, priority);

	return 0;
}

int loonggpu_job_submit_direct(struct loonggpu_job *job, struct loonggpu_ring *ring,
			     struct dma_fence **fence)
{
	int r;

	job->base.sched = &ring->sched;
	r = loonggpu_ib_schedule(ring, job->num_ibs, job->ibs, NULL, fence);
	job->fence = dma_fence_get(*fence);
	if (r)
		return r;

	loonggpu_job_free(job);
	return 0;
}

static struct dma_fence *loonggpu_job_dependency(struct drm_sched_job *sched_job,
					       struct drm_sched_entity *s_entity)
{
	struct loonggpu_ring *ring = to_loonggpu_ring(s_entity->rq->sched);
	struct loonggpu_job *job = to_loonggpu_job(sched_job);
	struct loonggpu_vm *vm = job->vm;
	struct dma_fence *fence;
	bool explicit = false;
	int r;

	fence = loonggpu_sync_get_fence(&job->sync, &explicit);
	if (fence && explicit) {
		if (lg_drm_sched_dependency_optimized(fence, s_entity)) {
			r = loonggpu_sync_fence(ring->adev, &job->sched_sync,
					      fence, false);
			if (r)
				DRM_ERROR("Error adding fence (%d)\n", r);
		}
	}

	while (fence == NULL && vm && !job->vmid) {
		r = loonggpu_vmid_grab(vm, ring, &job->sync,
				     &job->base.s_fence->finished,
				     job);
		if (r)
			DRM_ERROR("Error getting VM ID (%d)\n", r);

		fence = loonggpu_sync_get_fence(&job->sync, NULL);
	}

	return fence;
}

static struct dma_fence *loonggpu_job_run(struct drm_sched_job *sched_job)
{
	struct loonggpu_ring *ring = to_loonggpu_ring(sched_job->sched);
	struct dma_fence *fence = NULL, *finished;
	struct loonggpu_job *job;
	int r;

	job = to_loonggpu_job(sched_job);
	finished = &job->base.s_fence->finished;

	BUG_ON(loonggpu_sync_peek_fence(&job->sync, NULL));

	trace_loonggpu_sched_run_job(job);

	if (job->vram_lost_counter != atomic_read(&ring->adev->vram_lost_counter))
		dma_fence_set_error(finished, -ECANCELED);/* skip IB as well if VRAM lost */

	if (finished->error < 0) {
		DRM_INFO("Skip scheduling IBs!\n");
	} else {
		r = loonggpu_ib_schedule(ring, job->num_ibs, job->ibs, job,
				       &fence);
		if (r)
			DRM_ERROR("Error scheduling IBs (%d)\n", r);
	}
	/* if gpu reset, hw fence will be replaced here */
	dma_fence_put(job->fence);
	job->fence = dma_fence_get(fence);

	loonggpu_job_free_resources(job);
	return fence;
}

const struct drm_sched_backend_ops loonggpu_sched_ops = {
	lg_sched_ops_set_prepare
	.run_job = loonggpu_job_run,
	.timedout_job = loonggpu_job_timedout,
	.free_job = loonggpu_job_free_cb
};
