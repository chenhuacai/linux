#include <linux/dma-fence-array.h>
#include <linux/interval_tree_generic.h>
#include <linux/idr.h>
#include "loonggpu.h"
#include "loonggpu_drm.h"
#include "loonggpu_trace.h"
#include "loonggpu_vm_it.h"
#include "loonggpu_gmc.h"
#include "loonggpu_lgkcd.h"
#include "loonggpu_helper.h"
#include "kcd_svm.h"
#include "loonggpu_gtt_mgr_helper.h"

/**
 * DOC: GPUVM
 *
 * GPUVM is similar to the legacy gart on older asics, however
 * rather than there being a single global gart table
 * for the entire GPU, there are multiple VM page tables active
 * at any given time.  The VM page tables can contain a mix
 * vram pages and system memory pages and system memory pages
 * can be mapped as snooped (cached system pages) or unsnooped
 * (uncached system pages).
 * Each VM has an ID associated with it and there is a page table
 * associated with each VMID.  When execting a command buffer,
 * the kernel tells the the ring what VMID to use for that command
 * buffer.  VMIDs are allocated dynamically as commands are submitted.
 * The userspace drivers maintain their own address space and the kernel
 * sets up their pages tables accordingly when they submit their
 * command buffers and a VMID is assigned.
 * Cayman/Trinity support up to 8 active VMs at any given time;
 * SI supports 16.
 */

/**
 * loonggpu_vm_bo_base_init - Adds bo to the list of bos associated with the vm
 *
 * @base: base structure for tracking BO usage in a VM
 * @vm: vm to which bo is to be added
 * @bo: loonggpu buffer object
 *
 * Initialize a bo_va_base structure and add it to the appropriate lists
 *
 */
static void loonggpu_vm_bo_base_init(struct loonggpu_vm_bo_base *base,
				   struct loonggpu_vm *vm,
				   struct loonggpu_bo *bo)
{
	lg_dma_resv_t *bo_dma_resv, *root_dma_resv;

	base->vm = vm;
	base->bo = bo;
	INIT_LIST_HEAD(&base->bo_list);
	INIT_LIST_HEAD(&base->vm_status);

	if (!bo)
		return;

	list_add_tail(&base->bo_list, &bo->va);

	if (bo->tbo.type == ttm_bo_type_kernel)
		list_move(&base->vm_status, &vm->relocated);

	bo_dma_resv = to_dma_resv(bo);
	root_dma_resv = to_dma_resv(vm->root.base.bo);
	if (bo_dma_resv != root_dma_resv)
		return;

	if (bo->preferred_domains &
	    loonggpu_mem_type_to_domain(lg_get_bo_mem_type(bo)))
		return;

	/*
	 * we checked all the prerequisites, but it looks like this per vm bo
	 * is currently evicted. add the bo to the evicted list to make sure it
	 * is validated on next vm use to avoid fault.
	 * */
	list_move_tail(&base->vm_status, &vm->evicted);
	base->moved = true;
}

static u32 loonggpu_get_pde_pte_size (struct loonggpu_device *ldev)
{
	return ldev->vm_manager.pde_pte_bytes;
}

/**
 * loonggpu_vm_level_shift - return the addr shift for each level
 *
 * @ldev: loonggpu_device pointer
 * @level: VMPT level
 *
 * Returns:
 * The number of bits the pfn needs to be right shifted for a level.
 */
static unsigned loonggpu_vm_level_shift(struct loonggpu_device *ldev,
				      unsigned level)
{
	unsigned shift = 0xff;

	switch (level) {
	case LOONGGPU_VM_DIR0:
		shift = ldev->vm_manager.dir0_shift - ldev->vm_manager.dir2_shift;
		break;
	case LOONGGPU_VM_DIR1:
		shift = ldev->vm_manager.dir1_shift - ldev->vm_manager.dir2_shift;
		break;
	case LOONGGPU_VM_DIR2:
		shift = 0;
		break;
	default:
		dev_err(ldev->dev, "the level%d isn't supported.\n", level);
	}

	return shift;
}

/**
 * loonggpu_vm_set_pasid - manage pasid and vm ptr mapping
 *
 * @adev: loonggpu_device pointer
 * @vm: loonggpu_vm pointer
 * @pasid: the pasid the VM is using on this GPU
 *
 * Set the pasid this VM is using on this GPU, can also be used to remove the
 * pasid by passing in zero.
 *
 */
int loonggpu_vm_set_pasid(struct loonggpu_device *adev, struct loonggpu_vm *vm,
			u32 pasid)
{
	int r;

	if (vm->pasid == pasid)
		return 0;

	if (vm->pasid) {
		unsigned long flags;

		spin_lock_irqsave(&adev->vm_manager.pasid_lock, flags);
		idr_remove(&adev->vm_manager.pasid_idr, vm->pasid);
		spin_unlock_irqrestore(&adev->vm_manager.pasid_lock, flags);

		vm->pasid = 0;
	}

	if (pasid) {
		unsigned long flags;

		spin_lock_irqsave(&adev->vm_manager.pasid_lock, flags);
		r = idr_alloc(&adev->vm_manager.pasid_idr, vm, pasid, pasid + 1,
			      GFP_ATOMIC);
		spin_unlock_irqrestore(&adev->vm_manager.pasid_lock, flags);

		vm->pasid = pasid;
	}

	return 0;
}

/**
 * loonggpu_vm_num_entries - return the number of entries in a PD/PT
 *
 * @ldev: loonggpu_device pointer
 * @level: VMPT level
 *
 * Returns:
 * The number of entries in a page directory or page table.
 */
static unsigned loonggpu_vm_num_entries(struct loonggpu_device *ldev,
				      unsigned level)
{
	unsigned width = 0;

	switch (level) {
	case LOONGGPU_VM_DIR0:
		width = ldev->vm_manager.dir0_width;
		break;
	case LOONGGPU_VM_DIR1:
		width = ldev->vm_manager.dir1_width;
		break;
	case LOONGGPU_VM_DIR2:
		width = ldev->vm_manager.dir2_width;
		break;
	default:
		dev_err(ldev->dev, "the level%d isn't supported.\n", level);
	}

	return 1 << width;
}

/**
 * loonggpu_vm_bo_size - returns the size of the BOs in bytes
 *
 * @ldev: loonggpu_device pointer
 * @level: VMPT level
 *
 * Returns:
 * The size of the BO for a page directory or page table in bytes.
 */
static unsigned loonggpu_vm_bo_size(struct loonggpu_device *ldev, unsigned level)
{
	return LOONGGPU_GPU_PAGE_ALIGN(loonggpu_vm_num_entries(ldev, level) *
				loonggpu_get_pde_pte_size(ldev));
}

/**
 * loonggpu_vm_get_pd_bo - add the VM PD to a validation list
 *
 * @vm: vm providing the BOs
 * @validated: head of validation list
 * @entry: entry to add
 *
 * Add the page directory to the list of BOs to
 * validate for command submission.
 */
void loonggpu_vm_get_pd_bo(struct loonggpu_vm *vm,
			 struct list_head *validated,
			 struct loonggpu_bo_list_entry *entry)
{
	entry->robj = vm->root.base.bo;
	entry->priority = 0;
	entry->tv.bo = &entry->robj->tbo;
	lg_ttm_validate_buffer_set_shared(&entry->tv, true);
	entry->user_pages = NULL;
	list_add(&entry->tv.head, validated);
}

/**
 * loonggpu_vm_validate_pt_bos - validate the page table BOs
 *
 * @ldev: loonggpu device pointer
 * @vm: vm providing the BOs
 * @validate: callback to do the validation
 * @param: parameter for the validation callback
 *
 * Validate the page table BOs on command submission if neccessary.
 *
 * Returns:
 * Validation result.
 */
int loonggpu_vm_validate_pt_bos(struct loonggpu_device *ldev, struct loonggpu_vm *vm,
			      int (*validate)(void *p, struct loonggpu_bo *bo),
			      void *param)
{
	lg_ttm_global_t *glob = &ttm_glob;
	struct loonggpu_vm_bo_base *bo_base, *tmp;
	int r = 0;

	list_for_each_entry_safe(bo_base, tmp, &vm->evicted, vm_status) {
		struct loonggpu_bo *bo = bo_base->bo;

		if (bo->parent) {
			r = validate(param, bo);
			if (r)
				break;
			spin_lock(&ldev->mman.bdev.lru_lock);
			lg_ttm_bo_move_to_lru_tail(lg_ttm_bo_move_pass_args(&bo->tbo, NULL));
			if (bo->shadow)
				lg_ttm_bo_move_to_lru_tail(lg_ttm_bo_move_pass_args(&bo->shadow->tbo, NULL));
			spin_unlock(&ldev->mman.bdev.lru_lock);
		}

		if (bo->tbo.type != ttm_bo_type_kernel) {
			spin_lock(&vm->moved_lock);
			list_move(&bo_base->vm_status, &vm->moved);
			spin_unlock(&vm->moved_lock);
		} else {
			list_move(&bo_base->vm_status, &vm->relocated);
		}
	}

	spin_lock(&ldev->mman.bdev.lru_lock);
	list_for_each_entry(bo_base, &vm->idle, vm_status) {
		struct loonggpu_bo *bo = bo_base->bo;

		if (!bo->parent)
			continue;

		lg_ttm_bo_move_to_lru_tail(lg_ttm_bo_move_pass_args(&bo->tbo, NULL));
		if (bo->shadow)
			lg_ttm_bo_move_to_lru_tail(lg_ttm_bo_move_pass_args(&bo->shadow->tbo, NULL));
	}
	spin_unlock(&ldev->mman.bdev.lru_lock);

	return r;
}

/**
 * loonggpu_vm_ready - check VM is ready for updates
 *
 * @vm: VM to check
 *
 * Check if all VM PDs/PTs are ready for updates
 *
 * Returns:
 * True if eviction list is empty.
 */
bool loonggpu_vm_ready(struct loonggpu_vm *vm)
{
	return list_empty(&vm->evicted);
}

/**
 * loonggpu_vm_clear_bo - initially clear the PDs/PTs
 *
 * @ldev: loonggpu_device pointer
 * @vm: VM to clear BO from
 * @bo: BO to clear
 * @level: level this BO is at
 *
 * Root PD needs to be reserved when calling this.
 *
 * Returns:
 * 0 on success, errno otherwise.
 */
static int loonggpu_vm_clear_bo(struct loonggpu_device *ldev,
			      struct loonggpu_vm *vm, struct loonggpu_bo *bo,
			      unsigned level)
{
	struct ttm_operation_ctx ctx = { true, false };
	struct dma_fence *fence = NULL;
	lg_dma_resv_t *resv = to_dma_resv(bo);
	unsigned entries;
	struct loonggpu_ring *ring;
	struct loonggpu_job *job;
	u64 addr;
	int r;

	if (ldev->family_type == CHIP_NO_GPU)
		return 0;

	entries = loonggpu_bo_size(bo) / loonggpu_get_pde_pte_size(ldev);

	ring = container_of(vm->entity.rq->sched, struct loonggpu_ring, sched);

	r = lg_dma_resv_reserve_shared(resv, 1);
	if (r)
		return r;

	r = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
	if (r)
		goto error;

	r = loonggpu_job_alloc_with_ib(ldev, 64, &job);
	if (r)
		goto error;

	addr = loonggpu_bo_gpu_offset(bo);

	if (entries)
		loonggpu_vm_set_pte_pde(ldev, &job->ibs[0], addr, 0,
				      entries, LOONGGPU_GPU_PAGE_SIZE, 0);

	loonggpu_ring_pad_ib(ring, &job->ibs[0]);

	WARN_ON(job->ibs[0].length_dw > 64);
	r = loonggpu_sync_resv(ldev, &job->sync, resv, LOONGGPU_SYNC_ALWAYS,
			     LOONGGPU_FENCE_OWNER_UNDEFINED, false);
	if (r)
		goto error_free;

	r = loonggpu_job_submit(job, &vm->entity, LOONGGPU_FENCE_OWNER_UNDEFINED, &fence);
	if (r)
		goto error_free;

	loonggpu_bo_fence(bo, fence, true);
	dma_fence_put(fence);

	if (bo->shadow)
		return loonggpu_vm_clear_bo(ldev, vm, bo->shadow, level);

	return 0;

error_free:
	loonggpu_job_free(job);

error:
	return r;
}

/**
 * loonggpu_vm_alloc_levels - allocate the PD/PT levels
 *
 * @ldev: loonggpu_device pointer
 * @vm: requested vm
 * @parent: parent PT
 * @saddr: start of the address range
 * @eaddr: end of the address range
 * @level: VMPT level
 *
 * Make sure the page directories and page tables are allocated
 *
 * Returns:
 * 0 on success, errno otherwise.
 */
static int loonggpu_vm_alloc_levels(struct loonggpu_device *ldev,
				  struct loonggpu_vm *vm,
				  struct loonggpu_vm_pt *parent,
				  u64 saddr, u64 eaddr,
				  unsigned level)
{
	unsigned shift = loonggpu_vm_level_shift(ldev, level);
	unsigned pt_idx, from, to;
	u64 flags;
	int r;

	if (!parent->entries) {
		unsigned num_entries = loonggpu_vm_num_entries(ldev, level);

		parent->entries = kvmalloc_array(num_entries,
						   sizeof(struct loonggpu_vm_pt),
						   GFP_KERNEL | __GFP_ZERO);
		if (!parent->entries)
			return -ENOMEM;
	}

	from = saddr >> shift;
	to = eaddr >> shift;
	if (from >= loonggpu_vm_num_entries(ldev, level) ||
	    to >= loonggpu_vm_num_entries(ldev, level))
		return -EINVAL;

	++level;
	saddr = saddr & ((1 << shift) - 1);
	eaddr = eaddr & ((1 << shift) - 1);

	flags = LOONGGPU_GEM_CREATE_VRAM_CONTIGUOUS;
	if (vm->root.base.bo->shadow)
		flags |= LOONGGPU_GEM_CREATE_SHADOW;
	if (vm->use_cpu_for_update)
		flags |= LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
	else
		flags |= LOONGGPU_GEM_CREATE_NO_CPU_ACCESS;

	/**
	 * walk over the address space and allocate the page tables
	 */
	for (pt_idx = from; pt_idx <= to; ++pt_idx) {
		lg_dma_resv_t *resv = to_dma_resv(vm->root.base.bo);
		struct loonggpu_vm_pt *entry = &parent->entries[pt_idx];
		struct loonggpu_bo *pt;

		if (!entry->base.bo) {
			struct loonggpu_bo_param bp;

			memset(&bp, 0, sizeof(bp));
			bp.size = loonggpu_vm_bo_size(ldev, level);
			bp.byte_align = LOONGGPU_GPU_PAGE_SIZE;
			bp.domain = LOONGGPU_GEM_DOMAIN_VRAM;
			bp.flags = flags;
			bp.type = ttm_bo_type_kernel;
			bp.resv = resv;
			r = loonggpu_bo_create(ldev, &bp, &pt);
			if (r)
				return r;

			r = loonggpu_vm_clear_bo(ldev, vm, pt, level);
			if (r) {
				loonggpu_bo_unref(&pt->shadow);
				loonggpu_bo_unref(&pt);
				return r;
			}

			if (vm->use_cpu_for_update) {
				r = loonggpu_bo_kmap(pt, NULL);
				if (r) {
					loonggpu_bo_unref(&pt->shadow);
					loonggpu_bo_unref(&pt);
					return r;
				}
			}

			/**
			 * Keep a reference to the root directory to avoid
			 * freeing them up in the wrong order.
			 */
			pt->parent = loonggpu_bo_ref(parent->base.bo);

			loonggpu_vm_bo_base_init(&entry->base, vm, pt);
		}

		if (level < LOONGGPU_VM_DIR2) {
			u64 sub_saddr = (pt_idx == from) ? saddr : 0;
			u64 sub_eaddr = (pt_idx == to) ? eaddr :
				((1 << shift) - 1);
			r = loonggpu_vm_alloc_levels(ldev, vm, entry, sub_saddr,
						   sub_eaddr, level);
			if (r)
				return r;
		}
	}

	return 0;
}

/**
 * loonggpu_vm_alloc_pts - Allocate page tables.
 *
 * @ldev: loonggpu_device pointer
 * @vm: VM to allocate page tables for
 * @saddr: Start address which needs to be allocated
 * @size: Size from start address we need.
 *
 * Make sure the page tables are allocated.
 *
 * Returns:
 * 0 on success, errno otherwise.
 */
int loonggpu_vm_alloc_pts(struct loonggpu_device *ldev,
			struct loonggpu_vm *vm,
			u64 saddr, u64 size)
{
	u64 eaddr;

	/**
	 * validate the parameters
	 */
	if (saddr & LOONGGPU_GPU_PAGE_MASK || size & LOONGGPU_GPU_PAGE_MASK)
		return -EINVAL;

	eaddr = saddr + size - 1;

	saddr /= LOONGGPU_GPU_PAGE_SIZE;
	eaddr /= LOONGGPU_GPU_PAGE_SIZE;

	if (eaddr >= ldev->vm_manager.max_pfn) {
		dev_err(ldev->dev, "va above limit (0x%08llX >= 0x%08llX)\n",
			eaddr, ldev->vm_manager.max_pfn);
		return -EINVAL;
	}

	return loonggpu_vm_alloc_levels(ldev, vm, &vm->root, saddr, eaddr,
				      ldev->vm_manager.root_level);
}

/**
 * loonggpu_vm_need_pipeline_sync - Check if pipe sync is needed for job.
 *
 * @ring: ring on which the job will be submitted
 * @job: job to submit
 *
 * Returns:
 * True if sync is needed.
 */
bool loonggpu_vm_need_pipeline_sync(struct loonggpu_ring *ring,
				  struct loonggpu_job *job)
{
	struct loonggpu_device *ldev = ring->adev;
	struct loonggpu_vmid_mgr *id_mgr = &ldev->vm_manager.id_mgr;
	struct loonggpu_vmid *id;
	bool vm_flush_needed = job->vm_needs_flush;

	if (job->vmid == 0)
		return false;

	id = &id_mgr->ids[job->vmid];

	if (loonggpu_vmid_had_gpu_reset(ldev, id))
		return true;

	return vm_flush_needed;
}

/**
 * loonggpu_vm_flush - hardware flush the vm
 *
 * @ring: ring to use for flush
 * @job:  related job
 * @need_pipe_sync: is pipe sync needed
 *
 * Emit a VM flush when it is necessary.
 *
 * Returns:
 * 0 on success, errno otherwise.
 */
int loonggpu_vm_flush(struct loonggpu_ring *ring, struct loonggpu_job *job, bool need_pipe_sync)
{
	struct loonggpu_device *ldev = ring->adev;
	struct loonggpu_vmid_mgr *id_mgr = &ldev->vm_manager.id_mgr;
	struct loonggpu_vmid *id = &id_mgr->ids[job->vmid];
	bool vm_flush_needed = job->vm_needs_flush;
	struct dma_fence *fence = NULL;
	int r;
	bool pasid_mapping_needed = false;

	if (loonggpu_vmid_had_gpu_reset(ldev, id)) {
		vm_flush_needed = true;
		pasid_mapping_needed = true;
	}

	mutex_lock(&id_mgr->lock);
	if (id->pasid != job->pasid || !id->pasid_mapping ||
	    !dma_fence_is_signaled(id->pasid_mapping))
		pasid_mapping_needed = true;
	mutex_unlock(&id_mgr->lock);

	vm_flush_needed &= !!ring->funcs->emit_vm_flush  &&
			job->vm_pd_addr != LOONGGPU_BO_INVALID_OFFSET;

	pasid_mapping_needed &= ldev->gmc.gmc_funcs->emit_pasid_mapping &&
		ring->funcs->emit_wreg;

	if (!vm_flush_needed && !need_pipe_sync)
		return 0;

	if (pasid_mapping_needed)
		loonggpu_gmc_emit_pasid_mapping(ring, job->vmid, job->pasid);

	if (vm_flush_needed || pasid_mapping_needed) {
		trace_loonggpu_vm_flush(ring, job->vmid, job->vm_pd_addr);
		loonggpu_ring_emit_vm_flush(ring, job->vmid, job->vm_pd_addr);
	}

	if (vm_flush_needed) {
		r = loonggpu_fence_emit(ring, &fence, 0);
		if (r)
			return r;
	}

	if (vm_flush_needed) {
		mutex_lock(&id_mgr->lock);
		dma_fence_put(id->last_flush);
		id->last_flush = dma_fence_get(fence);
		id->current_gpu_reset_count =
			atomic_read(&ldev->gpu_reset_counter);
		mutex_unlock(&id_mgr->lock);
	}


	if (pasid_mapping_needed) {
		mutex_lock(&id_mgr->lock);
		id->pasid = job->pasid;
		dma_fence_put(id->pasid_mapping);
		id->pasid_mapping = dma_fence_get(fence);
		mutex_unlock(&id_mgr->lock);
	}

	dma_fence_put(fence);

	return 0;
}

/**
 * loonggpu_vm_bo_find - find the bo_va for a specific vm & bo
 *
 * @vm: requested vm
 * @bo: requested buffer object
 *
 * Find @bo inside the requested vm.
 * Search inside the @bos vm list for the requested vm
 * Returns the found bo_va or NULL if none is found
 *
 * Object has to be reserved!
 *
 * Returns:
 * Found bo_va or NULL.
 */
struct loonggpu_bo_va *loonggpu_vm_bo_find(struct loonggpu_vm *vm,
				       struct loonggpu_bo *bo)
{
	struct loonggpu_bo_va *bo_va;

	list_for_each_entry(bo_va, &bo->va, base.bo_list) {
		if (bo_va->base.vm == vm) {
			return bo_va;
		}
	}
	return NULL;
}

/**
 * loonggpu_vm_do_set_ptes - helper to call the right asic function
 *
 * @params: see loonggpu_pte_update_params definition
 * @bo: PD/PT to update
 * @pe: addr of the page entry
 * @addr: dst addr to write into pe
 * @count: number of page entries to update
 * @incr: increase next addr by incr bytes
 * @flags: hw access flags
 *
 * Traces the parameters and calls the right asic functions
 * to setup the page table using the DMA.
 */
static void loonggpu_vm_do_set_ptes(struct loonggpu_pte_update_params *params,
				  struct loonggpu_bo *bo,
				  u64 pe, u64 addr,
				  unsigned count, u32 incr,
				  u64 flags)
{
	pe += loonggpu_bo_gpu_offset(bo);
	trace_loonggpu_vm_set_ptes(pe, addr, count, incr, flags);

	loonggpu_vm_set_pte_pde(params->ldev, params->ib, pe, addr,
			      count, incr, flags);
}

/**
 * loonggpu_vm_do_copy_ptes - copy the PTEs from the GART
 *
 * @params: see loonggpu_pte_update_params definition
 * @bo: PD/PT to update
 * @pe: addr of the page entry
 * @addr: dst addr to write into pe
 * @count: number of page entries to update
 * @incr: increase next addr by incr bytes
 * @flags: hw access flags
 *
 * Traces the parameters and calls the DMA function to copy the PTEs.
 */
static void loonggpu_vm_do_copy_ptes(struct loonggpu_pte_update_params *params,
				   struct loonggpu_bo *bo,
				   u64 pe, u64 addr,
				   unsigned count, u32 incr,
				   u64 flags)
{
	u64 src = (params->src +
				(addr >> LOONGGPU_GPU_PAGE_SHIFT) *
				loonggpu_get_pde_pte_size(params->ldev));

	pe += loonggpu_bo_gpu_offset(bo);
	trace_loonggpu_vm_copy_ptes(pe, src, count);

	loonggpu_vm_copy_pte(params->ldev, params->ib, pe, src, count);
}

/**
 * loonggpu_vm_map_gart - Resolve gart mapping of addr
 *
 * @pages_addr: optional DMA address to use for lookup
 * @addr: the unmapped addr
 *
 * Look up the physical address of the page that the pte resolves
 * to.
 *
 * Returns:
 * The pointer for the page table entry.
 */
static u64 loonggpu_vm_map_gart(const dma_addr_t *pages_addr, u64 addr)
{
	u64 result;

	/* page table offset */
	result = pages_addr[addr >> PAGE_SHIFT];

	/* in case cpu page size != gpu page size*/
	result |= addr & (~PAGE_MASK);

	result &= ~((1ULL << LOONGGPU_GPU_PAGE_SHIFT) - 1);

	return result;
}

/**
 * loonggpu_vm_cpu_set_ptes - helper to update page tables via CPU
 *
 * @params: see loonggpu_pte_update_params definition
 * @bo: PD/PT to update
 * @pe: kmap addr of the page entry
 * @addr: dst addr to write into pe
 * @count: number of page entries to update
 * @incr: increase next addr by incr bytes
 * @flags: hw access flags
 *
 * Write count number of PT/PD entries directly.
 */
static void loonggpu_vm_cpu_set_ptes(struct loonggpu_pte_update_params *params,
				   struct loonggpu_bo *bo,
				   u64 pe, u64 addr,
				   unsigned count, u32 incr,
				   u64 flags)
{
	unsigned int i, r;
	void *kptr;
	u64 value;

	r = loonggpu_bo_kmap(bo, &kptr);
	if (r)
		return;
	pe += (u64)kptr;

	trace_loonggpu_vm_set_ptes(pe, addr, count, incr, flags);

	for (i = 0; i < count; i++) {
		value = params->pages_addr ?
			loonggpu_vm_map_gart(params->pages_addr, addr) : addr;
		loonggpu_gmc_set_pte_pde(params->ldev, (void *)(uintptr_t)pe,
				       i, value, flags);
		addr += incr;
	}
}


/**
 * loonggpu_vm_wait_pd - Wait for PT BOs to be free.
 *
 * @ldev: loonggpu_device pointer
 * @vm: related vm
 * @owner: fence owner
 *
 * Returns:
 * 0 on success, errno otherwise.
 */
static int loonggpu_vm_wait_pd(struct loonggpu_device *ldev, struct loonggpu_vm *vm,
			     void *owner)
{
	struct loonggpu_sync sync;
	lg_dma_resv_t *resv = to_dma_resv(vm->root.base.bo);
	int r;

	loonggpu_sync_create(&sync);
	loonggpu_sync_resv(ldev, &sync, resv, LOONGGPU_SYNC_ALWAYS, owner, false);
	r = loonggpu_sync_wait(&sync, true);
	if (r == -ERESTARTSYS)
		r = loonggpu_sync_wait(&sync, false);
	loonggpu_sync_free(&sync);

	return r;
}

/*
 * loonggpu_vm_update_pde - update a single level in the hierarchy
 *
 * @param: parameters for the update
 * @vm: requested vm
 * @parent: parent directory
 * @entry: entry to update
 *
 * Makes sure the requested entry in parent is up to date.
 */
static void loonggpu_vm_update_pde(struct loonggpu_pte_update_params *params,
				 struct loonggpu_vm *vm,
				 struct loonggpu_vm_pt *parent,
				 struct loonggpu_vm_pt *entry)
{
	struct loonggpu_bo *bo = parent->base.bo, *pbo;
	u64 pde, pt, flags;
	unsigned level;

	for (level = 0, pbo = bo->parent; pbo; ++level)
		pbo = pbo->parent;

	level += params->ldev->vm_manager.root_level;
	pt = loonggpu_bo_gpu_offset(entry->base.bo);
	flags = LOONGGPU_PTE_PRESENT;
	loonggpu_gmc_get_vm_pde(params->ldev, level, &pt, &flags);
	pde = (entry - parent->entries) * loonggpu_get_pde_pte_size(params->ldev);
	if (bo->shadow)
		params->func(params, bo->shadow, pde, pt, 1, 0, flags);
	params->func(params, bo, pde, pt, 1, 0, flags);
}

/*
 * loonggpu_vm_invalidate_level - mark all PD levels as invalid
 *
 * @ldev: loonggpu_device pointer
 * @vm: related vm
 * @parent: parent PD
 * @level: VMPT level
 *
 * Mark all PD level as invalid after an error.
 */
static void loonggpu_vm_invalidate_level(struct loonggpu_device *ldev,
				       struct loonggpu_vm *vm,
				       struct loonggpu_vm_pt *parent,
				       unsigned level)
{
	unsigned pt_idx, num_entries;

	/*
	 * Recurse into the subdirectories. This recursion is harmless because
	 * we only have a maximum of 5 layers.
	 */
	num_entries = loonggpu_vm_num_entries(ldev, level);
	for (pt_idx = 0; pt_idx < num_entries; ++pt_idx) {
		struct loonggpu_vm_pt *entry = &parent->entries[pt_idx];

		if (!entry->base.bo)
			continue;

		if (!entry->base.moved)
			list_move(&entry->base.vm_status, &vm->relocated);
		loonggpu_vm_invalidate_level(ldev, vm, entry, level + 1);
	}
}

/*
 * loonggpu_vm_update_directories - make sure that all directories are valid
 *
 * @ldev: loonggpu_device pointer
 * @vm: requested vm
 *
 * Makes sure all directories are up to date.
 *
 * Returns:
 * 0 for success, error for failure.
 */
int loonggpu_vm_update_directories(struct loonggpu_device *ldev,
				 struct loonggpu_vm *vm)
{
	struct loonggpu_pte_update_params params;
	struct loonggpu_job *job;
	unsigned ndw = 0;
	int r = 0;

	if (list_empty(&vm->relocated))
		return 0;

restart:
	memset(&params, 0, sizeof(params));
	params.ldev = ldev;

	if (vm->use_cpu_for_update) {
		struct loonggpu_vm_bo_base *bo_base;

		list_for_each_entry(bo_base, &vm->relocated, vm_status) {
			r = loonggpu_bo_kmap(bo_base->bo, NULL);
			if (unlikely(r))
				return r;
		}

		r = loonggpu_vm_wait_pd(ldev, vm, LOONGGPU_FENCE_OWNER_VM);
		if (unlikely(r))
			return r;

		params.func = loonggpu_vm_cpu_set_ptes;
	} else {
		ndw = 512 * 8; /* TODO */
		r = loonggpu_job_alloc_with_ib(ldev, ndw * LOONGGPU_BYTES_PER_DW, &job);
		if (r)
			return r;

		params.ib = &job->ibs[0];
		params.func = loonggpu_vm_do_set_ptes;
	}

	while (!list_empty(&vm->relocated)) {
		struct loonggpu_vm_bo_base *bo_base, *parent;
		struct loonggpu_vm_pt *pt, *entry;
		struct loonggpu_bo *bo;

		bo_base = list_first_entry(&vm->relocated,
					   struct loonggpu_vm_bo_base,
					   vm_status);
		bo_base->moved = false;
		list_del_init(&bo_base->vm_status);

		bo = bo_base->bo->parent;
		if (!bo)
			continue;

		parent = list_first_entry(&bo->va, struct loonggpu_vm_bo_base,
					  bo_list);
		pt = container_of(parent, struct loonggpu_vm_pt, base);
		entry = container_of(bo_base, struct loonggpu_vm_pt, base);

		loonggpu_vm_update_pde(&params, vm, pt, entry);

		if (!vm->use_cpu_for_update &&
		    (ndw - params.ib->length_dw) < 32)
			break;
	}

	if (vm->use_cpu_for_update) {
		/* Flush HDP */
		mb();
		/* TODO make sure cpu write pte is coherent */
	} else if (params.ib->length_dw == 0) {
		loonggpu_job_free(job);
	} else {
		struct loonggpu_bo *root = vm->root.base.bo;
		struct loonggpu_ring *ring;
		struct dma_fence *fence;
		lg_dma_resv_t *resv = to_dma_resv(root);

		ring = container_of(vm->entity.rq->sched, struct loonggpu_ring,
				    sched);

		loonggpu_ring_pad_ib(ring, params.ib);
		loonggpu_sync_resv(ldev, &job->sync, resv, LOONGGPU_SYNC_ALWAYS, LOONGGPU_FENCE_OWNER_VM, false);
		WARN_ON(params.ib->length_dw > ndw);
		r = loonggpu_job_submit(job, &vm->entity, LOONGGPU_FENCE_OWNER_VM, &fence);
		if (r)
			goto error;

		loonggpu_bo_fence(root, fence, true);
		dma_fence_put(vm->last_update);
		vm->last_update = fence;
	}

	if (!list_empty(&vm->relocated))
		goto restart;

	return 0;

error:
	loonggpu_vm_invalidate_level(ldev, vm, &vm->root,
				   ldev->vm_manager.root_level);
	loonggpu_job_free(job);
	return r;
}

/**
 * loonggpu_vm_find_entry - find the entry for an address
 *
 * @p: see loonggpu_pte_update_params definition
 * @addr: virtual address in question
 * @entry: resulting entry or NULL
 * @parent: parent entry
 *
 * Find the vm_pt entry and it's parent for the given address.
 */
void loonggpu_vm_get_entry(struct loonggpu_pte_update_params *p, u64 addr,
			 struct loonggpu_vm_pt **entry,
			 struct loonggpu_vm_pt **parent)
{
	unsigned level = p->ldev->vm_manager.root_level;

	*parent = NULL;
	*entry = &p->vm->root;

	while ((*entry)->entries) {
		unsigned shift = loonggpu_vm_level_shift(p->ldev, level++);

		*parent = *entry;
		*entry = &(*entry)->entries[addr >> shift];
		addr &= (1ULL << shift) - 1;
	}

	if (level != LOONGGPU_VM_DIR2)
		*entry = NULL;
}

/**
 * loonggpu_vm_update_ptes - make sure that page tables are valid
 *
 * @params: see loonggpu_pte_update_params definition
 * @start: start of GPU address range
 * @end: end of GPU address range
 * @dst: destination address to map to, the next dst inside the function
 * @flags: mapping flags
 *
 * Update the page tables in the range @start - @end.
 *
 * Returns:
 * 0 for success, -EINVAL for failure.
 */
static int loonggpu_vm_update_ptes(struct loonggpu_pte_update_params *params,
				  u64 start, u64 end,
				  u64 dst, u64 flags)
{
	struct loonggpu_device *ldev = params->ldev;
	const u64 mask = LOONGGPU_VM_PTE_COUNT(ldev) - 1;

	u64 addr, pe_start;
	struct loonggpu_bo *pt;
	unsigned nptes;

	/**
	 * walk over the address space and update the page tables
	 */
	for (addr = start; addr < end; addr += nptes,
	     dst += nptes * LOONGGPU_GPU_PAGE_SIZE) {
		struct loonggpu_vm_pt *entry, *parent;

		loonggpu_vm_get_entry(params, addr, &entry, &parent);
		if (!entry)
			return -ENOENT;

		if ((addr & ~mask) == (end & ~mask))
			nptes = end - addr;
		else
			nptes = LOONGGPU_VM_PTE_COUNT(ldev) - (addr & mask);

		pt = entry->base.bo;
		pe_start = (addr & mask) * loonggpu_get_pde_pte_size(params->ldev);
		if (pt->shadow)
			params->func(params, pt->shadow, pe_start, dst, nptes,
				     LOONGGPU_GPU_PAGE_SIZE, flags);
		params->func(params, pt, pe_start, dst, nptes,
			     LOONGGPU_GPU_PAGE_SIZE, flags);
	}

	return 0;
}

/**
 * loonggpu_vm_bo_update_mapping - update a mapping in the vm page table
 *
 * @ldev: loonggpu_device pointer
 * @exclusive: fence we need to sync to
 * @pages_addr: DMA addresses to use for mapping
 * @vm: requested vm
 * @start: start of mapped range
 * @last: last mapped entry
 * @flags: flags for the entries
 * @addr: addr to set the area to
 * @fence: optional resulting fence
 *
 * Fill in the page table entries between @start and @last.
 *
 * Returns:
 * 0 for success, -EINVAL for failure.
 */
int loonggpu_vm_bo_update_mapping(struct loonggpu_device *ldev,
				       struct dma_fence *exclusive,
				       dma_addr_t *pages_addr,
				       struct loonggpu_vm *vm,
				       u64 start, u64 last,
				       u64 flags, u64 addr,
				       struct dma_fence **fence)
{
	struct loonggpu_ring *ring;
	void *owner = LOONGGPU_FENCE_OWNER_VM;
	unsigned nptes, ncmds, ndw;
	struct loonggpu_job *job;
	struct loonggpu_pte_update_params params;
	struct dma_fence *f = NULL;
	lg_dma_resv_t *resv;
	int r;

	memset(&params, 0, sizeof(params));
	params.ldev = ldev;
	params.vm = vm;

	/**
	 * sync to everything on unmapping
	 */
	if (!(flags & LOONGGPU_PTE_PRESENT))
		owner = LOONGGPU_FENCE_OWNER_UNDEFINED;

	if (vm->use_cpu_for_update) {
		/**
		 * params.src is used as flag to indicate system Memory
		 */
		if (pages_addr)
			params.src = ~0;

		/**
		 * Wait for PT BOs to be free. PTs share the same resv. object
		 * as the root PD BO
		 */
		r = loonggpu_vm_wait_pd(ldev, vm, owner);
		if (unlikely(r))
			return r;

		params.func = loonggpu_vm_cpu_set_ptes;
		params.pages_addr = pages_addr;
		return loonggpu_vm_update_ptes(&params, start, last + 1,
					   addr, flags);
	}

	ring = container_of(vm->entity.rq->sched, struct loonggpu_ring, sched);

	nptes = last - start + 1;

	/**
	 * reserve space for two commands every (1 << BLOCK_SIZE)
	 * entries or 2k dwords (whatever is smaller)
     *
     * The second command is for the shadow pagetables.
	 *
	 * formula - loonggpu_vm_update_ptes
	 */
	if (vm->root.base.bo->shadow)
		ncmds = ((nptes >> min(ldev->vm_manager.block_size, 11u)) + 2) * 2;
	else
		ncmds = ((nptes >> min(ldev->vm_manager.block_size, 11u)) + 2);

	/* ib padding, default is 8 bytes, but reserved 64 bytes. */
	ndw = 64;

	if (pages_addr) {
		/**
		 * copy commands needed
		 */
		ndw += ncmds * ldev->vm_manager.vm_pte_funcs->copy_pte_num_dw;

		/**
		 * and also PTEs
		 */
		ndw += nptes * (loonggpu_get_pde_pte_size(ldev) / LOONGGPU_BYTES_PER_DW);

		params.func = loonggpu_vm_do_copy_ptes;

	} else {
		/**
		 * set page commands needed
		 */
		ndw += ncmds * ldev->vm_manager.vm_pte_funcs->set_pte_pde_num_dw;

		params.func = loonggpu_vm_do_set_ptes;
	}

	r = loonggpu_job_alloc_with_ib(ldev, ndw * LOONGGPU_BYTES_PER_DW, &job);
	if (r)
		return r;

	params.ib = &job->ibs[0];

	if (pages_addr) {
		u64 *pte;
		unsigned i;

		/*
		 * Put the PTEs at the end of the IB
		 */
		i = ndw - nptes * (loonggpu_get_pde_pte_size(ldev) / LOONGGPU_BYTES_PER_DW);
		pte = (u64 *)&(job->ibs->ptr[i]);
		params.src = job->ibs->gpu_addr + i * LOONGGPU_BYTES_PER_DW;

		for (i = 0; i < nptes; ++i) {
			pte[i] = loonggpu_vm_map_gart(pages_addr, addr + i *
						    LOONGGPU_GPU_PAGE_SIZE);
			pte[i] |= flags;
		}
		addr = 0;
	}

	r = loonggpu_sync_fence(ldev, &job->sync, exclusive, false);
	if (r)
		goto error_free;

	resv = to_dma_resv(vm->root.base.bo);
	r = loonggpu_sync_resv(ldev, &job->sync, resv, LOONGGPU_SYNC_ALWAYS, owner, false);
	if (r)
		goto error_free;

	r = lg_dma_resv_reserve_shared(resv, 1);
	if (r)
		goto error_free;

	r = loonggpu_vm_update_ptes(&params, start, last + 1, addr, flags);
	if (r)
		goto error_free;

	loonggpu_ring_pad_ib(ring, params.ib);
	WARN_ON(params.ib->length_dw > ndw);
	r = loonggpu_job_submit(job, &vm->entity, LOONGGPU_FENCE_OWNER_VM, &f);
	if (r)
		goto error_free;

	loonggpu_bo_fence(vm->root.base.bo, f, true);
	dma_fence_put(*fence);
	*fence = f;

	return 0;

error_free:
	loonggpu_job_free(job);
	return r;
}

/**
 * loonggpu_vm_bo_split_mapping - split a mapping into smaller chunks
 *
 * @ldev: loonggpu_device pointer
 * @exclusive: fence we need to sync to
 * @pages_addr: DMA addresses to use for mapping
 * @vm: requested vm
 * @mapping: mapped range and flags to use for the update
 * @flags: HW flags for the mapping
 * @nodes: array of drm_mm_nodes with the MC addresses
 * @fence: optional resulting fence
 *
 * Split the mapping into smaller chunks so that each update fits
 * into a SDMA IB.
 *
 * Returns:
 * 0 for success, -EINVAL for failure.
 */
static int loonggpu_vm_bo_split_mapping(struct loonggpu_device *ldev,
				      lg_ttm_mem_t *res,
				      struct dma_fence *exclusive,
				      dma_addr_t *pages_addr,
				      struct loonggpu_vm *vm,
				      struct loonggpu_bo_va_mapping *mapping,
				      u64 flags,
				      struct drm_mm_node *nodes,
				      struct dma_fence **fence)
{
	u64 pfn, start = mapping->start;
	int r;

	if (!(mapping->flags & LOONGGPU_PTE_WRITEABLE))
		flags &= ~LOONGGPU_PTE_WRITEABLE;
	trace_loonggpu_vm_bo_update(mapping);
	pfn = mapping->offset >> PAGE_SHIFT;

	struct loonggpu_res_cursor cursor;
	loonggpu_res_first(pages_addr ? NULL : res, mapping->offset,
			(mapping->last - mapping->start + 1) * LOONGGPU_GPU_PAGE_SIZE, &cursor);

	while (cursor.remaining) {
		dma_addr_t *dma_addr;
		u64 max_entries;
		u64 addr, last;

		if (pages_addr) {
			addr = pfn << PAGE_SHIFT;
			max_entries = LOONGGPU_VM_MAX_UPDATE_SIZE;
			dma_addr = pages_addr;
		} else if (cursor.node) {
			addr = cursor.start;
			addr += ldev->vm_manager.vram_base_offset;
			max_entries = cursor.size / LOONGGPU_GPU_PAGE_SIZE;
			dma_addr = NULL;
		} else {
			addr = 0;
			dma_addr = NULL;
			max_entries = cursor.size / LOONGGPU_GPU_PAGE_SIZE;
		}

		last = min(mapping->last, start + max_entries - 1);
		r = loonggpu_vm_bo_update_mapping(ldev, exclusive, dma_addr, vm, start, last, flags, addr, fence);
		if (r)
			return r;

		loonggpu_res_next(&cursor, (last - start + 1 ) * LOONGGPU_GPU_PAGE_SIZE);

		pfn += (last - start + 1) / LOONGGPU_GPU_PAGES_IN_CPU_PAGE;

		start = last + 1;

		if (start == mapping->last + 1)
			break;
	}

	return 0;
}

/**
 * loonggpu_vm_bo_update - update all BO mappings in the vm page table
 *
 * @ldev: loonggpu_device pointer
 * @bo_va: requested BO and VM object
 * @clear: if true clear the entries
 *
 * Fill in the page table entries for @bo_va.
 *
 * Returns:
 * 0 for success, -EINVAL for failure.
 */
int loonggpu_vm_bo_update(struct loonggpu_device *ldev,
			struct loonggpu_bo_va *bo_va,
			bool clear)
{
	struct loonggpu_bo *bo = bo_va->base.bo;
	struct loonggpu_vm *vm = bo_va->base.vm;
	struct loonggpu_bo_va_mapping *mapping;
	dma_addr_t *pages_addr = NULL;
	lg_ttm_mem_t *mem;
	struct drm_mm_node *nodes = NULL;
	struct dma_fence *exclusive, **last_update;
	lg_dma_resv_t *bo_resv, *root_resv;
	u64 flags;
	int r;

	if (clear || !bo) {
		mem = NULL;
		nodes = NULL;
		exclusive = NULL;
	} else {
		mem = lg_tbo_to_mem(&bo->tbo);
		if (mem->mem_type == TTM_PL_TT) {
			pages_addr = lg_tbo_to_dma_address(&bo->tbo);
		}

		bo_resv = to_dma_resv(bo);
		exclusive = lg_dma_resv_get_excl(bo_resv);
	}

	if (bo)
		flags = loonggpu_ttm_tt_pte_flags(ldev, bo->tbo.ttm, mem);
	else
		flags = 0x0;

	if (bo && bo->flags & LOONGGPU_GEM_CREATE_COMPRESSED_MASK)
		flags |= (bo->flags & LOONGGPU_GEM_CREATE_COMPRESSED_MASK)
				  >> LOONGGPU_PTE_COMPRESSED_SHIFT;

	root_resv = to_dma_resv(vm->root.base.bo);
	if (clear || (bo && bo_resv == root_resv))
		last_update = &vm->last_update;
	else
		last_update = &bo_va->last_pt_update;

	if (!clear && bo_va->base.moved) {
		bo_va->base.moved = false;
		list_splice_init(&bo_va->valids, &bo_va->invalids);

	} else if (bo_va->cleared != clear) {
		list_splice_init(&bo_va->valids, &bo_va->invalids);
	}

	list_for_each_entry(mapping, &bo_va->invalids, list) {
		r = loonggpu_vm_bo_split_mapping(ldev, mem, exclusive, pages_addr, vm,
						mapping, flags, nodes, last_update);
		if (r)
			return r;
	}


	spin_lock(&vm->moved_lock);
	list_del_init(&bo_va->base.vm_status);
	spin_unlock(&vm->moved_lock);

	/*
	 * If the BO is not in its preferred location add it back to
	 * the evicted list so that it gets validated again on the
	 * next command submission.
	 */
	if (bo && bo_resv == root_resv) {
		u32 mem_type = lg_get_bo_mem_type(bo);

		if (!(bo->preferred_domains & loonggpu_mem_type_to_domain(mem_type)))
			list_add_tail(&bo_va->base.vm_status, &vm->evicted);
		else
			list_add(&bo_va->base.vm_status, &vm->idle);
	}

	list_splice_init(&bo_va->invalids, &bo_va->valids);
	bo_va->cleared = clear;

	if (trace_loonggpu_vm_bo_mapping_enabled()) {
		list_for_each_entry(mapping, &bo_va->valids, list)
			trace_loonggpu_vm_bo_mapping(mapping);
	}

	return 0;
}

/**
 * loonggpu_vm_free_mapping - free a mapping
 *
 * @ldev: loonggpu_device pointer
 * @vm: requested vm
 * @mapping: mapping to be freed
 * @fence: fence of the unmap operation
 *
 * Free a mapping and make sure we decrease the PRT usage count if applicable.
 */
static void loonggpu_vm_free_mapping(struct loonggpu_device *ldev,
				   struct loonggpu_vm *vm,
				   struct loonggpu_bo_va_mapping *mapping,
				   struct dma_fence *fence)
{
	kfree(mapping);
}

/**
 * loonggpu_vm_clear_freed - clear freed BOs in the PT
 *
 * @ldev: loonggpu_device pointer
 * @vm: requested vm
 * @fence: optional resulting fence (unchanged if no work needed to be done
 * or if an error occurred)
 *
 * Make sure all freed BOs are cleared in the PT.
 * PTs have to be reserved and mutex must be locked!
 *
 * Returns:
 * 0 for success.
 *
 */
int loonggpu_vm_clear_freed(struct loonggpu_device *ldev,
			  struct loonggpu_vm *vm,
			  struct dma_fence **fence)
{
	struct loonggpu_bo_va_mapping *mapping;
	u64 init_pte_value = 0;
	struct dma_fence *f = NULL;
	int r;

	while (!list_empty(&vm->freed)) {
		mapping = list_first_entry(&vm->freed,
			struct loonggpu_bo_va_mapping, list);
		list_del(&mapping->list);

		r = loonggpu_vm_bo_update_mapping(ldev, NULL, NULL, vm,
						mapping->start, mapping->last,
						init_pte_value, 0, &f);
		loonggpu_vm_free_mapping(ldev, vm, mapping, f);
		if (r) {
			dma_fence_put(f);
			return r;
		}
	}

	if (fence && f) {
		dma_fence_put(*fence);
		*fence = f;
	} else {
		dma_fence_put(f);
	}

	return 0;

}

/**
 * loonggpu_vm_handle_moved - handle moved BOs in the PT
 *
 * @ldev: loonggpu_device pointer
 * @vm: requested vm
 *
 * Make sure all BOs which are moved are updated in the PTs.
 *
 * Returns:
 * 0 for success.
 *
 * PTs have to be reserved!
 */
int loonggpu_vm_handle_moved(struct loonggpu_device *ldev,
			   struct loonggpu_vm *vm)
{
	struct loonggpu_bo_va *bo_va, *tmp;
	struct list_head moved;
	bool clear;
	int r;

	INIT_LIST_HEAD(&moved);
	spin_lock(&vm->moved_lock);
	list_splice_init(&vm->moved, &moved);
	spin_unlock(&vm->moved_lock);

	list_for_each_entry_safe(bo_va, tmp, &moved, base.vm_status) {
		lg_dma_resv_t *resv = to_dma_resv(bo_va->base.bo);
		lg_dma_resv_t *root_resv = to_dma_resv(vm->root.base.bo);

		/* Per VM BOs never need to bo cleared in the page tables */
		if (resv == root_resv)
			clear = false;
		/* Try to reserve the BO to avoid clearing its ptes */
		else if (!loonggpu_vm_debug && lg_dma_resv_trylock(resv))
			clear = false;
		/* Somebody else is using the BO right now */
		else
			clear = true;

		r = loonggpu_vm_bo_update(ldev, bo_va, clear);
		if (r) {
			spin_lock(&vm->moved_lock);
			list_splice(&moved, &vm->moved);
			spin_unlock(&vm->moved_lock);
			return r;
		}

		if (!clear && resv != root_resv)
			lg_dma_resv_unlock(resv);

	}

	return 0;
}

/**
 * loonggpu_vm_bo_add - add a bo to a specific vm
 *
 * @ldev: loonggpu_device pointer
 * @vm: requested vm
 * @bo: loonggpu buffer object
 *
 * Add @bo into the requested vm.
 * Add @bo to the list of bos associated with the vm
 *
 * Returns:
 * Newly added bo_va or NULL for failure
 *
 * Object has to be reserved!
 */
struct loonggpu_bo_va *loonggpu_vm_bo_add(struct loonggpu_device *ldev,
				      struct loonggpu_vm *vm,
				      struct loonggpu_bo *bo)
{
	struct loonggpu_bo_va *bo_va;

	bo_va = kzalloc(sizeof(struct loonggpu_bo_va), GFP_KERNEL);
	if (bo_va == NULL) {
		return NULL;
	}
	loonggpu_vm_bo_base_init(&bo_va->base, vm, bo);

	bo_va->ref_count = 1;
	INIT_LIST_HEAD(&bo_va->valids);
	INIT_LIST_HEAD(&bo_va->invalids);

	return bo_va;
}


/**
 * loonggpu_vm_bo_insert_mapping - insert a new mapping
 *
 * @ldev: loonggpu_device pointer
 * @bo_va: bo_va to store the address
 * @mapping: the mapping to insert
 *
 * Insert a new mapping into all structures.
 */
static void loonggpu_vm_bo_insert_map(struct loonggpu_device *ldev,
				    struct loonggpu_bo_va *bo_va,
				    struct loonggpu_bo_va_mapping *mapping)
{
	struct loonggpu_vm *vm = bo_va->base.vm;
	struct loonggpu_bo *bo = bo_va->base.bo;
	lg_dma_resv_t *resv, *root_resv;

	mapping->bo_va = bo_va;
	list_add(&mapping->list, &bo_va->invalids);
	loonggpu_vm_it_insert(mapping, &vm->va);

	if (bo) {
		resv = to_dma_resv(bo);
		root_resv = to_dma_resv(vm->root.base.bo);

		if (resv == root_resv && !bo_va->base.moved) {
			spin_lock(&vm->moved_lock);
			list_move(&bo_va->base.vm_status, &vm->moved);
			spin_unlock(&vm->moved_lock);
		}
	}

	trace_loonggpu_vm_bo_map(bo_va, mapping);
}

/**
 * loonggpu_vm_bo_map - map bo inside a vm
 *
 * @ldev: loonggpu_device pointer
 * @bo_va: bo_va to store the address
 * @saddr: where to map the BO
 * @offset: requested offset in the BO
 * @size: BO size in bytes
 * @flags: attributes of pages (read/write/valid/etc.)
 *
 * Add a mapping of the BO at the specefied addr into the VM.
 *
 * Returns:
 * 0 for success, error for failure.
 *
 * Object has to be reserved and unreserved outside!
 */
int loonggpu_vm_bo_map(struct loonggpu_device *ldev,
		     struct loonggpu_bo_va *bo_va,
		     u64 saddr, u64 offset,
		     u64 size, u64 flags)
{
	struct loonggpu_bo_va_mapping *mapping, *tmp;
	struct loonggpu_bo *bo = bo_va->base.bo;
	struct loonggpu_vm *vm = bo_va->base.vm;
	u64 eaddr;

	/* validate the parameters */
	if (saddr & LOONGGPU_GPU_PAGE_MASK || offset & LOONGGPU_GPU_PAGE_MASK ||
	    size == 0 || size & LOONGGPU_GPU_PAGE_MASK)
		return -EINVAL;

	/* make sure object fit at this offset */
	eaddr = saddr + size - 1;
	if (saddr >= eaddr ||
	    (bo && offset + size > loonggpu_bo_size(bo)))
		return -EINVAL;

	saddr /= LOONGGPU_GPU_PAGE_SIZE;
	eaddr /= LOONGGPU_GPU_PAGE_SIZE;

	tmp = loonggpu_vm_it_iter_first(&vm->va, saddr, eaddr);
	if (tmp) {
		/* bo and tmp overlap, invalid addr */
		dev_err(ldev->dev, "bo %p va 0x%010Lx-0x%010Lx conflict with "
			"0x%010Lx-0x%010Lx\n", bo, saddr, eaddr,
			tmp->start, tmp->last + 1);
		return -EINVAL;
	}

	mapping = kmalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping)
		return -ENOMEM;

	mapping->start = saddr;
	mapping->last = eaddr;
	mapping->offset = offset;
	mapping->flags = flags;

	loonggpu_vm_bo_insert_map(ldev, bo_va, mapping);

	return 0;
}

/**
 * loonggpu_vm_bo_replace_map - map bo inside a vm, replacing existing mappings
 *
 * @ldev: loonggpu_device pointer
 * @bo_va: bo_va to store the address
 * @saddr: where to map the BO
 * @offset: requested offset in the BO
 * @size: BO size in bytes
 * @flags: attributes of pages (read/write/valid/etc.)
 *
 * Add a mapping of the BO at the specefied addr into the VM. Replace existing
 * mappings as we do so.
 *
 * Returns:
 * 0 for success, error for failure.
 *
 * Object has to be reserved and unreserved outside!
 */
int loonggpu_vm_bo_replace_map(struct loonggpu_device *ldev,
			     struct loonggpu_bo_va *bo_va,
			     u64 saddr, u64 offset,
			     u64 size, u64 flags)
{
	struct loonggpu_bo_va_mapping *mapping;
	struct loonggpu_bo *bo = bo_va->base.bo;
	u64 eaddr;
	int r;

	/* validate the parameters */
	if (saddr & LOONGGPU_GPU_PAGE_MASK || offset & LOONGGPU_GPU_PAGE_MASK ||
	    size == 0 || size & LOONGGPU_GPU_PAGE_MASK)
		return -EINVAL;

	/* make sure object fit at this offset */
	eaddr = saddr + size - 1;
	if (saddr >= eaddr ||
	    (bo && offset + size > loonggpu_bo_size(bo)))
		return -EINVAL;

	/* Allocate all the needed memory */
	mapping = kmalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping)
		return -ENOMEM;

	r = loonggpu_vm_bo_clear_mappings(ldev, bo_va->base.vm, saddr, size);
	if (r) {
		kfree(mapping);
		return r;
	}

	saddr /= LOONGGPU_GPU_PAGE_SIZE;
	eaddr /= LOONGGPU_GPU_PAGE_SIZE;

	mapping->start = saddr;
	mapping->last = eaddr;
	mapping->offset = offset;
	mapping->flags = flags;

	loonggpu_vm_bo_insert_map(ldev, bo_va, mapping);

	return 0;
}

/**
 * loonggpu_vm_bo_unmap - remove bo mapping from vm
 *
 * @ldev: loonggpu_device pointer
 * @bo_va: bo_va to remove the address from
 * @saddr: where to the BO is mapped
 *
 * Remove a mapping of the BO at the specefied addr from the VM.
 *
 * Returns:
 * 0 for success, error for failure.
 *
 * Object has to be reserved and unreserved outside!
 */
int loonggpu_vm_bo_unmap(struct loonggpu_device *ldev,
		       struct loonggpu_bo_va *bo_va,
		       u64 saddr)
{
	struct loonggpu_bo_va_mapping *mapping;
	struct loonggpu_vm *vm = bo_va->base.vm;
	bool valid = true;

	saddr /= LOONGGPU_GPU_PAGE_SIZE;

	list_for_each_entry(mapping, &bo_va->valids, list) {
		if (mapping->start == saddr)
			break;
	}

	if (&mapping->list == &bo_va->valids) {
		valid = false;

		list_for_each_entry(mapping, &bo_va->invalids, list) {
			if (mapping->start == saddr)
				break;
		}

		if (&mapping->list == &bo_va->invalids)
			return -ENOENT;
	}

	list_del(&mapping->list);
	loonggpu_vm_it_remove(mapping, &vm->va);
	mapping->bo_va = NULL;
	trace_loonggpu_vm_bo_unmap(bo_va, mapping);

	if (valid)
		list_add(&mapping->list, &vm->freed);
	else
		loonggpu_vm_free_mapping(ldev, vm, mapping,
				       bo_va->last_pt_update);

	return 0;
}

/**
 * loonggpu_vm_bo_clear_mappings - remove all mappings in a specific range
 *
 * @ldev: loonggpu_device pointer
 * @vm: VM structure to use
 * @saddr: start of the range
 * @size: size of the range
 *
 * Remove all mappings in a range, split them as appropriate.
 *
 * Returns:
 * 0 for success, error for failure.
 */
int loonggpu_vm_bo_clear_mappings(struct loonggpu_device *ldev,
				struct loonggpu_vm *vm,
				u64 saddr, u64 size)
{
	struct loonggpu_bo_va_mapping *before, *after, *tmp, *next;
	LIST_HEAD(removed);
	u64 eaddr;

	eaddr = saddr + size - 1;
	saddr /= LOONGGPU_GPU_PAGE_SIZE;
	eaddr /= LOONGGPU_GPU_PAGE_SIZE;

	/* Allocate all the needed memory */
	before = kzalloc(sizeof(*before), GFP_KERNEL);
	if (!before)
		return -ENOMEM;
	INIT_LIST_HEAD(&before->list);

	after = kzalloc(sizeof(*after), GFP_KERNEL);
	if (!after) {
		kfree(before);
		return -ENOMEM;
	}
	INIT_LIST_HEAD(&after->list);

	/* Now gather all removed mappings */
	tmp = loonggpu_vm_it_iter_first(&vm->va, saddr, eaddr);
	while (tmp) {
		/* Remember mapping split at the start */
		if (tmp->start < saddr) {
			before->start = tmp->start;
			before->last = saddr - 1;
			before->offset = tmp->offset;
			before->flags = tmp->flags;
			before->bo_va = tmp->bo_va;
			list_add(&before->list, &tmp->bo_va->invalids);
		}

		/* Remember mapping split at the end */
		if (tmp->last > eaddr) {
			after->start = eaddr + 1;
			after->last = tmp->last;
			after->offset = tmp->offset;
			after->offset += after->start - tmp->start;
			after->flags = tmp->flags;
			after->bo_va = tmp->bo_va;
			list_add(&after->list, &tmp->bo_va->invalids);
		}

		list_del(&tmp->list);
		list_add(&tmp->list, &removed);

		tmp = loonggpu_vm_it_iter_next(tmp, saddr, eaddr);
	}

	/* And free them up */
	list_for_each_entry_safe(tmp, next, &removed, list) {
		loonggpu_vm_it_remove(tmp, &vm->va);
		list_del(&tmp->list);

		if (tmp->start < saddr)
		    tmp->start = saddr;
		if (tmp->last > eaddr)
		    tmp->last = eaddr;

		tmp->bo_va = NULL;
		list_add(&tmp->list, &vm->freed);
		trace_loonggpu_vm_bo_unmap(NULL, tmp);
	}

	/* Insert partial mapping before the range */
	if (!list_empty(&before->list)) {
		loonggpu_vm_it_insert(before, &vm->va);
	} else {
		kfree(before);
	}

	/* Insert partial mapping after the range */
	if (!list_empty(&after->list)) {
		loonggpu_vm_it_insert(after, &vm->va);
	} else {
		kfree(after);
	}

	return 0;
}

/**
 * loonggpu_vm_bo_lookup_mapping - find mapping by address
 *
 * @vm: the requested VM
 * @addr: the address
 *
 * Find a mapping by it's address.
 *
 * Returns:
 * The loonggpu_bo_va_mapping matching for addr or NULL
 *
 */
struct loonggpu_bo_va_mapping *loonggpu_vm_bo_lookup_mapping(struct loonggpu_vm *vm,
							 u64 addr)
{
	return loonggpu_vm_it_iter_first(&vm->va, addr, addr);
}

/**
 * loonggpu_vm_bo_trace_cs - trace all reserved mappings
 *
 * @vm: the requested vm
 * @ticket: CS ticket
 *
 * Trace all mappings of BOs reserved during a command submission.
 */
void loonggpu_vm_bo_trace_cs(struct loonggpu_vm *vm, struct ww_acquire_ctx *ticket)
{
	struct loonggpu_bo_va_mapping *mapping;

	if (!trace_loonggpu_vm_bo_cs_enabled())
		return;

	for (mapping = loonggpu_vm_it_iter_first(&vm->va, 0, U64_MAX); mapping;
	     mapping = loonggpu_vm_it_iter_next(mapping, 0, U64_MAX)) {
		if (mapping->bo_va && mapping->bo_va->base.bo) {
			struct loonggpu_bo *bo = mapping->bo_va->base.bo;
			lg_dma_resv_t *resv = to_dma_resv(bo);

			if (READ_ONCE(resv->lock.ctx) != ticket)
				continue;
		}

		trace_loonggpu_vm_bo_cs(mapping);
	}
}

/**
 * loonggpu_vm_bo_rmv - remove a bo to a specific vm
 *
 * @ldev: loonggpu_device pointer
 * @bo_va: requested bo_va
 *
 * Remove @bo_va->bo from the requested vm.
 *
 * Object have to be reserved!
 */
void loonggpu_vm_bo_rmv(struct loonggpu_device *ldev,
		      struct loonggpu_bo_va *bo_va)
{
	struct loonggpu_bo_va_mapping *mapping, *next;
	struct loonggpu_vm *vm = bo_va->base.vm;

	list_del(&bo_va->base.bo_list);

	spin_lock(&vm->moved_lock);
	list_del(&bo_va->base.vm_status);
	spin_unlock(&vm->moved_lock);

	list_for_each_entry_safe(mapping, next, &bo_va->valids, list) {
		list_del(&mapping->list);
		loonggpu_vm_it_remove(mapping, &vm->va);
		mapping->bo_va = NULL;
		trace_loonggpu_vm_bo_unmap(bo_va, mapping);
		list_add(&mapping->list, &vm->freed);
	}
	list_for_each_entry_safe(mapping, next, &bo_va->invalids, list) {
		list_del(&mapping->list);
		loonggpu_vm_it_remove(mapping, &vm->va);
		loonggpu_vm_free_mapping(ldev, vm, mapping,
				       bo_va->last_pt_update);
	}

	dma_fence_put(bo_va->last_pt_update);
	kfree(bo_va);
}

/**
 * loonggpu_vm_bo_invalidate - mark the bo as invalid
 *
 * @ldev: loonggpu_device pointer
 * @bo: loonggpu buffer object
 * @evicted: is the BO evicted
 *
 * Mark @bo as invalid.
 */
void loonggpu_vm_bo_invalidate(struct loonggpu_device *ldev,
			     struct loonggpu_bo *bo, bool evicted)
{
	struct loonggpu_vm_bo_base *bo_base;

	/* shadow bo doesn't have bo base, its validation needs its parent */
	if (bo->parent && bo->parent->shadow == bo)
		bo = bo->parent;

	list_for_each_entry(bo_base, &bo->va, bo_list) {
		struct loonggpu_vm *vm = bo_base->vm;
		lg_dma_resv_t *root_resv = to_dma_resv(vm->root.base.bo);
		lg_dma_resv_t *resv = to_dma_resv(bo);
		bool was_moved = bo_base->moved;

		bo_base->moved = true;
		if (evicted && resv == root_resv) {
			if (bo->tbo.type == ttm_bo_type_kernel)
				list_move(&bo_base->vm_status, &vm->evicted);
			else
				list_move_tail(&bo_base->vm_status,
					       &vm->evicted);
			continue;
		}

		if (was_moved)
			continue;

		if (bo->tbo.type == ttm_bo_type_kernel) {
			list_move(&bo_base->vm_status, &vm->relocated);
		} else {
			spin_lock(&bo_base->vm->moved_lock);
			list_move(&bo_base->vm_status, &vm->moved);
			spin_unlock(&bo_base->vm->moved_lock);
		}
	}
}

/**
 * loonggpu_vm_get_block_size - calculate the shift of PTEs a block contains
 *
 * @vm_size: VM size in GB
 *
 * Returns:
 * The shift of PTEs a block contains
 */
static u32 loonggpu_vm_get_block_size(u64 vm_size)
{
	(void)(vm_size);
	return LOONGGPU_PAGE_PTE_SHIFT;
}

/**
 * loonggpu_vm_adjust_size - adjust vm size, block size
 *
 * @ldev: loonggpu_device pointer
 * @min_vm_size: the minimum vm size in GB if it's set auto
 * @max_level: max page table levels
 * @max_bits: max address space size in bits
 *
 */
void loonggpu_vm_adjust_size(struct loonggpu_device *ldev, u32 min_vm_size,
			  unsigned max_level, unsigned max_bits)
{
	unsigned int vm_size;
	u64 tmp;

#if 0
	unsigned int max_size = 1 << (max_bits - 30);

	struct sysinfo si;
	unsigned int phys_ram_gb;

	/* Optimal VM size depends on the amount of physical
	 * RAM available. Underlying requirements and
	 * assumptions:
	 *
	 *  - Need to map system memory and VRAM from all GPUs
	 *     - VRAM from other GPUs not known here
	 *     - Assume VRAM <= system memory
	 *  - On GFX8 and older, VM space can be segmented for
	 *    different MTYPEs
	 *  - Need to allow room for fragmentation, guard pages etc.
	 *
	 * This adds up to a rough guess of system memory x3.
	 * Round up to power of two to maximize the available
	 * VM size with the given page table size.
	 */
	si_meminfo(&si);
	phys_ram_gb = ((uint64_t)si.totalram * si.mem_unit +
		       (1 << 30) - 1) >> 30;
	vm_size = roundup_pow_of_two(min(max(phys_ram_gb * 3, min_vm_size), max_size));

#else
	vm_size = 1 << (max_bits - LOONGGPU_GB_SHIFT_BITS);

#endif
	ldev->vm_manager.pde_pte_bytes = LOONGGPU_VM_PDE_PTE_BYTES;
	ldev->vm_manager.max_pfn = (u64)vm_size <<
				(LOONGGPU_GB_SHIFT_BITS - LOONGGPU_GPU_PAGE_SHIFT);

	tmp = roundup_pow_of_two(ldev->vm_manager.max_pfn);
	if (loonggpu_vm_block_size != -1)
		tmp >>= loonggpu_vm_block_size - LOONGGPU_PAGE_PTE_SHIFT;

	tmp = DIV_ROUND_UP(fls64(tmp), LOONGGPU_PAGE_PTE_SHIFT);
	ldev->vm_manager.num_level = min(max_level, (unsigned)tmp);
	ldev->vm_manager.root_level = LOONGGPU_VM_DIR0;
	ldev->vm_manager.dir2_width = LOONGGPU_PAGE_PTE_SHIFT;
	ldev->vm_manager.dir2_shift = LOONGGPU_GPU_PAGE_SHIFT;
	ldev->vm_manager.dir1_shift = ldev->vm_manager.dir2_shift + ldev->vm_manager.dir2_width;
	ldev->vm_manager.dir1_width = LOONGGPU_PAGE_PTE_SHIFT;
	ldev->vm_manager.dir0_shift = ldev->vm_manager.dir1_shift + ldev->vm_manager.dir1_width;
	ldev->vm_manager.dir0_width = max_bits - ldev->vm_manager.dir0_shift;

	/* block size depends on vm size and hw setup*/
	if (loonggpu_vm_block_size != -1)
		ldev->vm_manager.block_size =
			min((unsigned)loonggpu_vm_block_size, (unsigned)LOONGGPU_PAGE_PTE_SHIFT);
	else if (ldev->vm_manager.num_level > 1)
		ldev->vm_manager.block_size = LOONGGPU_PAGE_PTE_SHIFT;
	else
		ldev->vm_manager.block_size = loonggpu_vm_get_block_size(vm_size);

	DRM_INFO("vm size is %u GB, %u levels, block size is %u-bit\n",
		 vm_size, ldev->vm_manager.num_level + 1,
		 ldev->vm_manager.block_size);
}

/**
 * loonggpu_vm_init - initialize a vm instance
 *
 * @ldev: loonggpu_device pointer
 * @vm: requested vm
 * @vm_context: Indicates if it GFX or Compute context
 * @pasid: Process address space identifier
 *
 * Init @vm fields.
 *
 * Returns:
 * 0 for success, error for failure.
 */
int loonggpu_vm_init(struct loonggpu_device *ldev, struct loonggpu_vm *vm,
		   int vm_context, unsigned int pasid)
{
	struct loonggpu_bo_param bp;
	struct loonggpu_bo *root;
	const unsigned align = min((unsigned)LOONGGPU_VM_PTB_ALIGN_SIZE,
		LOONGGPU_VM_PTE_COUNT(ldev) * loonggpu_get_pde_pte_size(ldev));
	unsigned ring_instance;
	struct loonggpu_ring *ring;
	struct drm_sched_rq *rq;
	struct drm_gpu_scheduler *sched;
	unsigned long size;
	u64 flags;
	int r;

	vm->va = RB_ROOT_CACHED;
	vm->reserved_vmid = NULL;
	INIT_LIST_HEAD(&vm->evicted);
	INIT_LIST_HEAD(&vm->relocated);
	spin_lock_init(&vm->moved_lock);
	INIT_LIST_HEAD(&vm->moved);
	INIT_LIST_HEAD(&vm->idle);
	INIT_LIST_HEAD(&vm->freed);

	/* create scheduler entity for page table updates */

	ring_instance = atomic_inc_return(&ldev->vm_manager.vm_pte_next_ring);
	ring_instance %= ldev->vm_manager.vm_pte_num_rings;
	ring = ldev->vm_manager.vm_pte_rings[ring_instance];
	rq = sched->sched_rq[DRM_SCHED_PRIORITY_KERNEL];
	sched = &ring->sched;
	if (ldev->family_type != CHIP_NO_GPU)
		r = lg_drm_sched_entity_init(&vm->entity, DRM_SCHED_PRIORITY_KERNEL,
					&sched, 1, &rq, 1, NULL);
	if (r)
		return r;

	vm->pte_support_ats = false;
	vm->is_compute_context = false;

	vm->use_cpu_for_update = ldev->vm_manager.vm_update_mode;

	DRM_DEBUG_DRIVER("VM update mode is %s\n",
			 vm->use_cpu_for_update ? "CPU" : "XDMA");
	WARN_ONCE((vm->use_cpu_for_update & !loonggpu_gmc_vram_full_visible(&ldev->gmc)),
		  "CPU update of VM recommended only for large BAR system\n");
	vm->last_update = NULL;

	flags = LOONGGPU_GEM_CREATE_VRAM_CONTIGUOUS;
	if (vm->use_cpu_for_update)
		flags |= LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
	else if (vm_context != LOONGGPU_VM_CONTEXT_COMPUTE)
		flags |= LOONGGPU_GEM_CREATE_SHADOW;

	size = loonggpu_vm_bo_size(ldev, ldev->vm_manager.root_level);
	memset(&bp, 0, sizeof(bp));
	bp.size = size;
	bp.byte_align = align;
	bp.domain = LOONGGPU_GEM_DOMAIN_VRAM;
	bp.flags = flags;
	bp.type = ttm_bo_type_kernel;
	bp.resv = NULL;
	r = loonggpu_bo_create(ldev, &bp, &root);
	if (r)
		goto error_free_sched_entity;

	r = loonggpu_bo_reserve(root, true);
	if (r)
		goto error_free_root;

	r = loonggpu_vm_clear_bo(ldev, vm, root,
			       ldev->vm_manager.root_level);
	if (r)
		goto error_unreserve;

	loonggpu_vm_bo_base_init(&vm->root.base, vm, root);
	loonggpu_bo_unreserve(vm->root.base.bo);

	if (pasid) {
		unsigned long flags;

		spin_lock_irqsave(&ldev->vm_manager.pasid_lock, flags);
		r = idr_alloc(&ldev->vm_manager.pasid_idr, vm, pasid, pasid + 1,
			      GFP_ATOMIC);
		spin_unlock_irqrestore(&ldev->vm_manager.pasid_lock, flags);
		if (r < 0)
			goto error_free_root;

		vm->pasid = pasid;
	}

	INIT_KFIFO(vm->faults);
	vm->fault_credit = 16;

	return 0;

error_unreserve:
	loonggpu_bo_unreserve(vm->root.base.bo);

error_free_root:
	loonggpu_bo_unref(&vm->root.base.bo->shadow);
	loonggpu_bo_unref(&vm->root.base.bo);
	vm->root.base.bo = NULL;

error_free_sched_entity:
	drm_sched_entity_destroy(&vm->entity);

	return r;
}

/**
 * loonggpu_vm_make_compute - Turn a GFX VM into a compute VM
 *
 * @adev: aloonggpu_device pointer
 * @vm: requested vm
 *
 * This only works on GFX VMs that don't have any BOs added and no
 * page tables allocated yet.
 *
 * Changes the following VM parameters:
 * - use_cpu_for_update
 * - pte_supports_ats
 * - pasid (old PASID is released, because compute manages its own PASIDs)
 *
 * Reinitializes the page directory to reflect the changed ATS
 * setting.
 *
 * Returns:
 * 0 for success, -errno for errors.
 */
int loonggpu_vm_make_compute(struct loonggpu_device *adev, struct loonggpu_vm *vm)
{
	int r;

	r = loonggpu_bo_reserve(vm->root.base.bo, true);
	if (r)
		return r;

	/* Sanity checks */
	if (!RB_EMPTY_ROOT(&vm->va.rb_root) || vm->root.entries) {
		r = -EINVAL;
		goto error;
	}

	/* Check if PD needs to be reinitialized and do it before
	 * changing any other state, in case it fails.
	 */
	r = loonggpu_vm_clear_bo(adev, vm, vm->root.base.bo,
			adev->vm_manager.root_level);
	if (r)
		goto error;

	/* Update VM state */
	vm->use_cpu_for_update = !!adev->vm_manager.vm_update_mode;
	DRM_DEBUG_DRIVER("VM update mode is %s\n",
			 vm->use_cpu_for_update ? "CPU" : "XDMA");
	WARN_ONCE((vm->use_cpu_for_update & !loonggpu_gmc_vram_full_visible(&adev->gmc)),
		  "CPU update of VM recommended only for large BAR system\n");

	vm->is_compute_context = true;
	r = loonggpu_lgkcd_gpuvm_set_vm_pasid(adev, vm, 0);
	if (r)
		goto error;

	/* Free the shadow bo for compute VM */
	loonggpu_bo_unref(&vm->root.base.bo->shadow);

error:
	loonggpu_bo_unreserve(vm->root.base.bo);
	return r;
}

/**
 * loonggpu_vm_release_compute - release a compute vm
 * @adev: loonggpu_device pointer
 * @vm: a vm turned into compute vm by calling loonggpu_vm_make_compute
 *
 * This is a correspondant of loonggpu_vm_make_compute. It decouples compute
 * pasid from vm. Compute should stop use of vm after this call.
 */
void loonggpu_vm_release_compute(struct loonggpu_device *adev, struct loonggpu_vm *vm)
{
	loonggpu_vm_set_pasid(adev, vm, 0);
	vm->is_compute_context = false;
}

/**
 * loonggpu_vm_free_levels - free PD/PT levels
 *
 * @ldev: loonggpu device structure
 * @parent: PD/PT starting level to free
 * @level: level of parent structure
 *
 * Free the page directory or page table level and all sub levels.
 */
static void loonggpu_vm_free_levels(struct loonggpu_device *ldev,
				  struct loonggpu_vm_pt *parent,
				  unsigned level)
{
	unsigned i, num_entries = loonggpu_vm_num_entries(ldev, level);

	if (parent->base.bo) {
		list_del(&parent->base.bo_list);
		list_del(&parent->base.vm_status);
		loonggpu_bo_unref(&parent->base.bo->shadow);
		loonggpu_bo_unref(&parent->base.bo);
	}

	if (parent->entries)
		for (i = 0; i < num_entries; i++)
			loonggpu_vm_free_levels(ldev, &parent->entries[i],
					      level + 1);

	kvfree(parent->entries);
}

/**
 * loonggpu_vm_fini - tear down a vm instance
 *
 * @ldev: loonggpu_device pointer
 * @vm: requested vm
 *
 * Tear down @vm.
 * Unbind the VM and remove all bos from the vm bo list
 */
void loonggpu_vm_fini(struct loonggpu_device *ldev, struct loonggpu_vm *vm)
{
	struct loonggpu_bo_va_mapping *mapping, *tmp;
	struct loonggpu_bo *root;
	int r;

	loonggpu_lgkcd_gpuvm_destroy_cb(ldev, vm);

	if (vm->pasid) {
		unsigned long flags;

		spin_lock_irqsave(&ldev->vm_manager.pasid_lock, flags);
		idr_remove(&ldev->vm_manager.pasid_idr, vm->pasid);
		spin_unlock_irqrestore(&ldev->vm_manager.pasid_lock, flags);
	}

	if (ldev->family_type != CHIP_NO_GPU)
		drm_sched_entity_destroy(&vm->entity);

	if (!RB_EMPTY_ROOT(&vm->va.rb_root)) {
		dev_err(ldev->dev, "still active bo inside vm\n");
	}
	rbtree_postorder_for_each_entry_safe(mapping, tmp,
					     &vm->va.rb_root, rb) {
		list_del(&mapping->list);
		loonggpu_vm_it_remove(mapping, &vm->va);
		kfree(mapping);
	}
	list_for_each_entry_safe(mapping, tmp, &vm->freed, list) {
		list_del(&mapping->list);
		loonggpu_vm_free_mapping(ldev, vm, mapping, NULL);
	}

	root = loonggpu_bo_ref(vm->root.base.bo);
	r = loonggpu_bo_reserve(root, true);
	if (r) {
		dev_err(ldev->dev, "Leaking page tables because BO reservation failed\n");
	} else {
		loonggpu_vm_free_levels(ldev, &vm->root,
				      ldev->vm_manager.root_level);
		loonggpu_bo_unreserve(root);
	}
	loonggpu_bo_unref(&root);
	dma_fence_put(vm->last_update);
	loonggpu_vmid_free_reserved(ldev, vm);

	loonggpu_sema_free(ldev, vm);
}

/**
 * loonggpu_vm_pasid_fault_credit - Check fault credit for given PASID
 *
 * @ldev: loonggpu_device pointer
 * @pasid: PASID do identify the VM
 *
 * This function is expected to be called in interrupt context.
 *
 * Returns:
 * True if there was fault credit, false otherwise
 */
bool loonggpu_vm_pasid_fault_credit(struct loonggpu_device *ldev,
				  unsigned int pasid)
{
	struct loonggpu_vm *vm;

	spin_lock(&ldev->vm_manager.pasid_lock);
	vm = idr_find(&ldev->vm_manager.pasid_idr, pasid);
	if (!vm) {
		/* VM not found, can't track fault credit */
		spin_unlock(&ldev->vm_manager.pasid_lock);
		return true;
	}

	/* No lock needed. only accessed by IRQ handler */
	if (!vm->fault_credit) {
		/* Too many faults in this VM */
		spin_unlock(&ldev->vm_manager.pasid_lock);
		return false;
	}

	vm->fault_credit--;
	spin_unlock(&ldev->vm_manager.pasid_lock);
	return true;
}

/**
 * loonggpu_vm_manager_init - init the VM manager
 *
 * @ldev: loonggpu_device pointer
 *
 * Initialize the VM manager structures
 */
void loonggpu_vm_manager_init(struct loonggpu_device *ldev)
{
	unsigned i;

	loonggpu_vmid_mgr_init(ldev);

	ldev->vm_manager.fence_context =
		dma_fence_context_alloc(LOONGGPU_MAX_RINGS);

	for (i = 0; i < LOONGGPU_MAX_RINGS; ++i)
		ldev->vm_manager.seqno[i] = 0;

	atomic_set(&ldev->vm_manager.vm_pte_next_ring, 0);
	spin_lock_init(&ldev->vm_manager.prt_lock);
	atomic_set(&ldev->vm_manager.num_prt_users, 0);

	if (loonggpu_vm_update_mode == -1)
		if (loonggpu_gmc_vram_full_visible(&ldev->gmc))
			ldev->vm_manager.vm_update_mode = 1;
		else
			ldev->vm_manager.vm_update_mode = 0;
	else
		ldev->vm_manager.vm_update_mode = loonggpu_vm_update_mode;

	idr_init(&ldev->vm_manager.pasid_idr);
	spin_lock_init(&ldev->vm_manager.pasid_lock);
}

/**
 * loonggpu_vm_manager_fini - cleanup VM manager
 *
 * @ldev: loonggpu_device pointer
 *
 * Cleanup the VM manager and free resources.
 */
void loonggpu_vm_manager_fini(struct loonggpu_device *ldev)
{
	WARN_ON(!idr_is_empty(&ldev->vm_manager.pasid_idr));
	idr_destroy(&ldev->vm_manager.pasid_idr);

	loonggpu_vmid_mgr_fini(ldev);
}

/**
 * loonggpu_vm_ioctl - Manages VMID reservation.
 *
 * @dev: drm device pointer
 * @data: drm_loonggpu_vm
 * @filp: drm file pointer
 *
 * Returns:
 * 0 for success, -errno for errors.
 */
int loonggpu_vm_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	union drm_loonggpu_vm *args = data;
	struct loonggpu_device *ldev = dev->dev_private;
	struct loonggpu_fpriv *fpriv = filp->driver_priv;
	int r;

	switch (args->in.op) {
	case LOONGGPU_VM_OP_RESERVE_VMID:
		/* current, we only have requirement to reserve vmid */
		r = loonggpu_vmid_alloc_reserved(ldev, &fpriv->vm);
		if (r)
			return r;
		break;
	case LOONGGPU_VM_OP_UNRESERVE_VMID:
		loonggpu_vmid_free_reserved(ldev, &fpriv->vm);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/**
 * loonggpu_vm_get_task_info - Extracts task info for a PASID.
 *
 * @dev: drm device pointer
 * @pasid: PASID identifier for VM
 * @task_info: task_info to fill.
 */
void loonggpu_vm_get_task_info(struct loonggpu_device *ldev, unsigned int pasid,
			 struct loonggpu_task_info *task_info)
{
	struct loonggpu_vm *vm;
	unsigned long flags;

	spin_lock_irqsave(&ldev->vm_manager.pasid_lock, flags);

	vm = idr_find(&ldev->vm_manager.pasid_idr, pasid);
	if (vm)
		*task_info = vm->task_info;

	spin_unlock_irqrestore(&ldev->vm_manager.pasid_lock, flags);
}

/**
 * loonggpu_vm_set_task_info - Sets VMs task info.
 *
 * @vm: vm for which to set the info
 */
void loonggpu_vm_set_task_info(struct loonggpu_vm *vm)
{
	if (!vm->task_info.pid) {
		vm->task_info.pid = current->pid;
		get_task_comm(vm->task_info.task_name, current);

		if (current->group_leader->mm == current->mm) {
			vm->task_info.tgid = current->group_leader->pid;
			get_task_comm(vm->task_info.process_name, current->group_leader);
		}
	}
}

/**
 * loonggpu_vm_handle_fault - graceful handling of VM faults.
 * @adev: loonggpu device pointer
 * @pasid: PASID of the VM
 * @vmid: VMID, only used for LG2XX.
 * @node_id: Node_id received in IH cookie. Only applicable for
 *           LG2XX.
 * @addr: Address of the fault
 * @write_fault: true is write fault, false is read fault
 *
 * Try to gracefully handle a VM fault. Return true if the fault was handled and
 * shouldn't be reported any more.
 */
int loonggpu_vm_handle_fault(struct loonggpu_device *adev, u32 pasid,
			    u32 vmid, u32 node_id, uint64_t addr,
			    u32 fault_domain)
{
	bool is_compute_context = false;
	struct loonggpu_bo *root;
	struct loonggpu_vm *vm;
	spin_lock(&adev->vm_manager.pasid_lock);
	vm = idr_find(&adev->vm_manager.pasid_idr, pasid);
	if (vm) {
		root = loonggpu_bo_ref(vm->root.base.bo);
		is_compute_context = vm->is_compute_context;
	} else {
		root = NULL;
	}
	spin_unlock(&adev->vm_manager.pasid_lock);

	if (!root)
		return false;

	addr /= LOONGGPU_GPU_PAGE_SIZE;
	if (is_compute_context && !svm_range_restore_pages(adev, pasid, vmid,
	    node_id, addr, true)) {
		loonggpu_bo_unref(&root);
		loonggpu_gmc_flush_gpu_retry(adev, vmid, fault_domain);
		return true;
	}

	loonggpu_bo_unref(&root);
	return false;
}
