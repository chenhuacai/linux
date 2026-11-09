#ifndef __LOONGGPU_IDS_H__
#define __LOONGGPU_IDS_H__

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/dma-fence.h>

#include "loonggpu_sync.h"

/* maximum number of VMIDs */
#define LOONGGPU_NUM_VMID	4

struct loonggpu_device;
struct loonggpu_vm;
struct loonggpu_ring;
struct loonggpu_sync;
struct loonggpu_job;

struct loonggpu_vmid {
	struct list_head	list;
	struct loonggpu_sync	active;
	struct dma_fence	*last_flush;
	uint64_t		owner;

	uint64_t		pd_gpu_addr;
	/* last flushed PD/PT update */
	struct dma_fence	*flushed_updates;

	uint32_t                current_gpu_reset_count;

	unsigned		pasid;
	struct dma_fence	*pasid_mapping;
};

struct loonggpu_vmid_mgr {
	struct mutex		lock;
	unsigned		num_ids;
	struct list_head	ids_lru;
	struct loonggpu_vmid	ids[LOONGGPU_NUM_VMID];
	atomic_t		reserved_vmid_num;
};

int loonggpu_pasid_alloc(unsigned int bits);
void loonggpu_pasid_free(unsigned int pasid);
void loonggpu_pasid_free_delayed(lg_dma_resv_t *resv, unsigned int pasid);

bool loonggpu_vmid_had_gpu_reset(struct loonggpu_device *adev,
			       struct loonggpu_vmid *id);
int loonggpu_vmid_alloc_reserved(struct loonggpu_device *adev, struct loonggpu_vm *vm);
void loonggpu_vmid_free_reserved(struct loonggpu_device *adev, struct loonggpu_vm *vm);
int loonggpu_vmid_grab(struct loonggpu_vm *vm, struct loonggpu_ring *ring,
		     struct loonggpu_sync *sync, struct dma_fence *fence,
		     struct loonggpu_job *job);
void loonggpu_vmid_reset(struct loonggpu_device *adev, unsigned vmid);
void loonggpu_vmid_reset_all(struct loonggpu_device *adev);

void loonggpu_vmid_mgr_init(struct loonggpu_device *adev);
void loonggpu_vmid_mgr_fini(struct loonggpu_device *adev);

#endif /* __LOONGGPU_IDS_H__ */
