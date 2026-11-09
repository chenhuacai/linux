#ifndef __LOONGGPU_JOB_H__
#define __LOONGGPU_JOB_H__

/* bit set means command submit involves a preamble IB */
#define LOONGGPU_PREAMBLE_IB_PRESENT          (1 << 0)
/* bit set means preamble IB is first presented in belonging context */
#define LOONGGPU_PREAMBLE_IB_PRESENT_FIRST    (1 << 1)
/* bit set means context switch occured */
#define LOONGGPU_HAVE_CTX_SWITCH              (1 << 2)

#define to_loonggpu_job(sched_job)		\
		container_of((sched_job), struct loonggpu_job, base)

struct loonggpu_fence;

struct loonggpu_job {
	struct drm_sched_job    base;
	struct loonggpu_vm	*vm;
	struct loonggpu_sync	sync;
	struct loonggpu_sync	sched_sync;
	struct loonggpu_ib	*ibs;
	struct dma_fence	*fence; /* the hw fence */
	uint32_t		preamble_status;
	uint32_t		num_ibs;
	void			*owner;
	bool                    vm_needs_flush;
	uint64_t		vm_pd_addr;
	unsigned		vmid;
	unsigned		pasid;
	uint32_t		vram_lost_counter;

	/* user fence handling */
	uint64_t		uf_addr;
	uint64_t		uf_sequence;

};

int loonggpu_job_alloc(struct loonggpu_device *adev, unsigned num_ibs,
		     struct loonggpu_job **job, struct loonggpu_vm *vm);
int loonggpu_job_alloc_with_ib(struct loonggpu_device *adev, unsigned size,
			     struct loonggpu_job **job);

void loonggpu_job_free_resources(struct loonggpu_job *job);
void loonggpu_job_free(struct loonggpu_job *job);
int loonggpu_job_submit(struct loonggpu_job *job, struct drm_sched_entity *entity,
		      void *owner, struct dma_fence **f);
int loonggpu_job_submit_direct(struct loonggpu_job *job, struct loonggpu_ring *ring,
			     struct dma_fence **fence);

#endif /* __LOONGGPU_JOB_H__ */
