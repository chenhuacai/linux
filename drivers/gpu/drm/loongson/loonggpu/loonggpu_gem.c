#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/pci.h>
#include <linux/dma-buf.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_gem_ttm_helper.h>
#include "loonggpu.h"
#include "loonggpu_drm.h"
#include "loonggpu_display.h"
#include "loonggpu_helper.h"
#include "loonggpu_bo_pin_helper.h"

static vm_fault_t loonggpu_gem_fault(struct vm_fault *vmf)
{
        struct ttm_buffer_object *bo = vmf->vma->vm_private_data;
        struct drm_device *ddev = bo->base.dev;
        vm_fault_t ret;
        int idx;

        ret = ttm_bo_vm_reserve(bo, vmf);
        if (ret)
                return ret;

        if (drm_dev_enter(ddev, &idx)) {
                ret = loonggpu_bo_fault_reserve_notify(bo);
                if (ret) {
                        drm_dev_exit(idx);
                        goto unlock;
                }

                ret = ttm_bo_vm_fault_reserved(vmf, vmf->vma->vm_page_prot,
                                               TTM_BO_VM_NUM_PREFAULT);
                drm_dev_exit(idx);
        } else {
                ret = ttm_bo_vm_dummy_page(vmf, vmf->vma->vm_page_prot);
        }
        if (ret == VM_FAULT_RETRY && !(vmf->flags & FAULT_FLAG_RETRY_NOWAIT))
                return ret;

unlock:
        dma_resv_unlock(bo->base.resv);
        return ret;
}

static const struct vm_operations_struct loonggpu_gem_vm_ops = {
        .fault = loonggpu_gem_fault,
        .open = ttm_bo_vm_open,
        .close = ttm_bo_vm_close,
        .access = ttm_bo_vm_access
};

static int loonggpu_gem_object_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
        struct loonggpu_bo *bo = gem_to_loonggpu_bo(obj);

        if (loonggpu_ttm_tt_get_usermm(bo->tbo.ttm))
                return -EPERM;
        if (bo->flags & LOONGGPU_GEM_CREATE_NO_CPU_ACCESS)
                return -EPERM;

        /* Workaround for Thunk bug creating PROT_NONE,MAP_PRIVATE mappings
         * for debugger access to invisible VRAM. Should have used MAP_SHARED
         * instead. Clearing VM_MAYWRITE prevents the mapping from ever
         * becoming writable and makes vma_is_cow_mapping(vma) false.
         */
        if (vma_is_cow_mapping(vma) &&
            !(vma->vm_flags & VM_ACCESS_FLAGS))
                vm_flags_clear(vma, VM_MAYWRITE);

        return drm_gem_ttm_mmap(obj, vma);
}

void loonggpu_gem_object_free(struct drm_gem_object *gobj)
{
	struct loonggpu_bo *robj = gem_to_loonggpu_bo(gobj);

	if (robj) {
		loonggpu_mn_unregister(robj);
		loonggpu_bo_unref(&robj);
	}
}

const struct drm_gem_object_funcs loonggpu_gem_object_funcs = {
	.free = loonggpu_gem_object_free,
	.open = loonggpu_gem_object_open,
	.close = loonggpu_gem_object_close,
	.export = loonggpu_gem_prime_export,
	.get_sg_table = loonggpu_gem_prime_get_sg_table,
	.vmap = drm_gem_ttm_vmap,
	.vunmap = drm_gem_ttm_vunmap,
	.mmap = loonggpu_gem_object_mmap,
	.vm_ops = &loonggpu_gem_vm_ops,
};

int loonggpu_gem_object_create(struct loonggpu_device *adev, unsigned long size,
			     int alignment, u32 initial_domain,
			     u64 flags, enum ttm_bo_type type,
			     lg_dma_resv_t *resv,
			     struct drm_gem_object **obj)
{
	struct loonggpu_bo *bo;
	struct loonggpu_bo_param bp;
	int r;

	memset(&bp, 0, sizeof(bp));
	*obj = NULL;
	/* At least align on page size */
	if (alignment < PAGE_SIZE) {
		alignment = PAGE_SIZE;
	}

	bp.size = size;
	bp.byte_align = alignment;
	bp.type = type;
	bp.resv = resv;
	bp.preferred_domain = initial_domain;
retry:
	bp.flags = flags;
	bp.domain = initial_domain;
	r = loonggpu_bo_create(adev, &bp, &bo);
	if (r) {
		if (r != -ERESTARTSYS) {
			if (flags & LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED) {
				flags &= ~LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
				goto retry;
			}

			if (initial_domain == LOONGGPU_GEM_DOMAIN_VRAM &&
				!(flags & LOONGGPU_GEM_CREATE_COMPRESSED_MASK)) {
				initial_domain |= LOONGGPU_GEM_DOMAIN_GTT;
				goto retry;
			}
			DRM_DEBUG("Failed to allocate GEM object (%ld, %d, %u, %d)\n",
				  size, initial_domain, alignment, r);
		}
		return r;
	}
	*obj = &lg_gbo_to_gem_obj(bo);
	(*obj)->funcs = &loonggpu_gem_object_funcs;
	return 0;
}

void loonggpu_gem_force_release(struct loonggpu_device *adev)
{
	struct drm_device *ddev = adev->ddev;
	struct drm_file *file;

	mutex_lock(&ddev->filelist_mutex);

	list_for_each_entry(file, &ddev->filelist, lhead) {
		struct drm_gem_object *gobj;
		int handle;

		WARN_ONCE(1, "Still active user space clients!\n");
		spin_lock(&file->table_lock);
		idr_for_each_entry(&file->object_idr, gobj, handle) {
			WARN_ONCE(1, "And also active allocations!\n");
			lg_drm_gem_object_put(gobj);
		}
		idr_destroy(&file->object_idr);
		spin_unlock(&file->table_lock);
	}

	mutex_unlock(&ddev->filelist_mutex);
}

/*
 * Call from drm_gem_handle_create which appear in both new and open ioctl
 * case.
 */
int loonggpu_gem_object_open(struct drm_gem_object *obj,
			   struct drm_file *file_priv)
{
	struct loonggpu_bo *abo = gem_to_loonggpu_bo(obj);
	struct loonggpu_device *adev = loonggpu_ttm_adev(abo->tbo.bdev);
	struct loonggpu_fpriv *fpriv = file_priv->driver_priv;
	struct loonggpu_vm *vm = &fpriv->vm;
	struct loonggpu_bo_va *bo_va;
	lg_dma_resv_t *bo_resv = to_dma_resv(abo);
	lg_dma_resv_t *root_resv = to_dma_resv(vm->root.base.bo);
	struct mm_struct *mm;
	int r;

	mm = loonggpu_ttm_tt_get_usermm(abo->tbo.ttm);
	if (mm && mm != current->mm)
		return -EPERM;

	if (abo->flags & LOONGGPU_GEM_CREATE_VM_ALWAYS_VALID && bo_resv != root_resv)
		return -EPERM;

	r = loonggpu_bo_reserve(abo, false);
	if (r)
		return r;

	bo_va = loonggpu_vm_bo_find(vm, abo);
	if (!bo_va) {
		bo_va = loonggpu_vm_bo_add(adev, vm, abo);
	} else {
		++bo_va->ref_count;
	}
	loonggpu_bo_unreserve(abo);
	return 0;
}

void loonggpu_gem_object_close(struct drm_gem_object *obj,
			     struct drm_file *file_priv)
{
	struct loonggpu_bo *bo = gem_to_loonggpu_bo(obj);
	struct loonggpu_device *adev = loonggpu_ttm_adev(bo->tbo.bdev);
	struct loonggpu_fpriv *fpriv = file_priv->driver_priv;
	struct loonggpu_vm *vm = &fpriv->vm;

	struct loonggpu_bo_list_entry vm_pd;
	struct list_head list, duplicates;
	struct ttm_validate_buffer tv;
	struct ww_acquire_ctx ticket;
	struct loonggpu_bo_va *bo_va;
	int r;

	INIT_LIST_HEAD(&list);
	INIT_LIST_HEAD(&duplicates);

	tv.bo = &bo->tbo;
	lg_ttm_validate_buffer_set_shared(&tv, true);
	list_add(&tv.head, &list);

	loonggpu_vm_get_pd_bo(vm, &list, &vm_pd);

	r = lg_ttm_eu_reserve_buffers(&ticket, &list, false, &duplicates, false);
	if (r) {
		dev_err(adev->dev, "leaking bo va because "
			"we fail to reserve bo (%d)\n", r);
		return;
	}
	bo_va = loonggpu_vm_bo_find(vm, bo);
	if (bo_va && --bo_va->ref_count == 0) {
		loonggpu_vm_bo_rmv(adev, bo_va);

		if (loonggpu_vm_ready(vm)) {
			struct dma_fence *fence = NULL;

			r = loonggpu_vm_clear_freed(adev, vm, &fence);
			if (unlikely(r)) {
				dev_err(adev->dev, "failed to clear page "
					"tables on GEM object close (%d)\n", r);
			}

			if (fence) {
				loonggpu_bo_fence(bo, fence, true);
				dma_fence_put(fence);
			}
		}
	}
	ttm_eu_backoff_reservation(&ticket, &list);
}

/*
 * GEM ioctls.
 */
int loonggpu_gem_create_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *filp)
{
	struct loonggpu_device *adev = dev->dev_private;
	struct loonggpu_fpriv *fpriv = filp->driver_priv;
	struct loonggpu_vm *vm = &fpriv->vm;
	union drm_loonggpu_gem_create *args = data;
	uint64_t flags = args->in.domain_flags;
	uint64_t size = args->in.bo_size;
	lg_dma_resv_t *resv = NULL;
	struct drm_gem_object *gobj;
	uint32_t handle;
	int r;

	/* reject DOORBELLs until userspace code to use it is available */
	if (args->in.domains & LOONGGPU_GEM_DOMAIN_DOORBELL)
		return -EINVAL;

	/* reject invalid gem flags */
	if (flags & ~(LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED |
		      LOONGGPU_GEM_CREATE_NO_CPU_ACCESS |
		      LOONGGPU_GEM_CREATE_CPU_GTT_USWC |
		      LOONGGPU_GEM_CREATE_VRAM_CLEARED |
		      LOONGGPU_GEM_CREATE_VM_ALWAYS_VALID |
		      LOONGGPU_GEM_CREATE_EXPLICIT_SYNC |
		      LOONGGPU_GEM_CREATE_COMPRESSED_MASK |
		      LOONGGPU_GEM_CREATE_DISCARDABLE))

		return -EINVAL;

	/* reject invalid gem domains */
	if (args->in.domains & ~LOONGGPU_GEM_DOMAIN_MASK)
		return -EINVAL;

	/* reject invalid gem compressed_mode */
	if (flags & LOONGGPU_GEM_CREATE_COMPRESSED_MASK && (
			args->in.domains != LOONGGPU_GEM_DOMAIN_VRAM
			|| !adev->zip_meta.ready))
		return -EINVAL;

	size = roundup(size, PAGE_SIZE);

	if (flags & LOONGGPU_GEM_CREATE_VM_ALWAYS_VALID) {
		r = loonggpu_bo_reserve(vm->root.base.bo, false);
		if (r)
			return r;

		resv = to_dma_resv(vm->root.base.bo);
	}

	r = loonggpu_gem_object_create(adev, size, args->in.alignment,
				     (u32)(0xffffffff & args->in.domains),
				     flags, ttm_bo_type_device, resv, &gobj);
	if (flags & LOONGGPU_GEM_CREATE_VM_ALWAYS_VALID) {
		if (!r) {
			struct loonggpu_bo *abo = gem_to_loonggpu_bo(gobj);

			abo->parent = loonggpu_bo_ref(vm->root.base.bo);
		}
		loonggpu_bo_unreserve(vm->root.base.bo);
	}
	if (r)
		return r;

	r = drm_gem_handle_create(filp, gobj, &handle);
	/* drop reference from allocate - handle holds it now */
	lg_drm_gem_object_put(gobj);
	if (r)
		return r;

	memset(args, 0, sizeof(*args));
	args->out.handle = handle;
	return 0;
}

int loonggpu_gem_userptr_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *filp)
{
	struct ttm_operation_ctx ctx = { true, false };
	struct loonggpu_device *adev = dev->dev_private;
	struct drm_loonggpu_gem_userptr *args = data;
	struct drm_gem_object *gobj;
	struct loonggpu_bo *bo;
	uint32_t handle;
	int r;

	if (offset_in_page(args->addr | args->size))
		return -EINVAL;

	/* reject unknown flag values */
	if (args->flags & ~(LOONGGPU_GEM_USERPTR_READONLY |
	    LOONGGPU_GEM_USERPTR_ANONONLY | LOONGGPU_GEM_USERPTR_VALIDATE |
	    LOONGGPU_GEM_USERPTR_REGISTER))
		return -EINVAL;

	if (!(args->flags & LOONGGPU_GEM_USERPTR_READONLY) &&
	     !(args->flags & LOONGGPU_GEM_USERPTR_REGISTER)) {

		/* if we want to write to it we must install a MMU notifier */
		return -EACCES;
	}

	/* create a gem object to contain this object in */
	r = loonggpu_gem_object_create(adev, args->size, 0, LOONGGPU_GEM_DOMAIN_CPU,
				     0, ttm_bo_type_device, NULL, &gobj);
	if (r)
		return r;

	bo = gem_to_loonggpu_bo(gobj);
	bo->preferred_domains = LOONGGPU_GEM_DOMAIN_GTT;
	bo->allowed_domains = LOONGGPU_GEM_DOMAIN_GTT;
	r = loonggpu_ttm_tt_set_userptr(bo->tbo.ttm, args->addr, args->flags);
	if (r)
		goto release_object;

	if (args->flags & LOONGGPU_GEM_USERPTR_REGISTER) {
		r = loonggpu_mn_register(bo, args->addr);
		if (r)
			goto release_object;
	}

	if (args->flags & LOONGGPU_GEM_USERPTR_VALIDATE) {
		r = loonggpu_ttm_tt_get_user_pages(bo->tbo.ttm,
						 bo->tbo.ttm->pages);
		if (r)
			goto release_object;

		r = loonggpu_bo_reserve(bo, true);
		if (r)
			goto free_pages;

		loonggpu_bo_placement_from_domain(bo, LOONGGPU_GEM_DOMAIN_GTT);
		r = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
		loonggpu_bo_unreserve(bo);
		if (r)
			goto free_pages;
	}

	r = drm_gem_handle_create(filp, gobj, &handle);
	/* drop reference from allocate - handle holds it now */
	lg_drm_gem_object_put(gobj);
	if (r)
		return r;

	args->handle = handle;
	return 0;

free_pages:
	release_pages(bo->tbo.ttm->pages, bo->tbo.ttm->num_pages);

release_object:
	lg_drm_gem_object_put(gobj);

	return r;
}

int loonggpu_mode_dumb_mmap(struct drm_file *filp,
			  struct drm_device *dev,
			  uint32_t handle, uint64_t *offset_p)
{
	struct drm_gem_object *gobj;
	struct loonggpu_bo *robj;

	gobj = drm_gem_object_lookup(filp, handle);
	if (gobj == NULL) {
		return -ENOENT;
	}
	robj = gem_to_loonggpu_bo(gobj);
	if (loonggpu_ttm_tt_get_usermm(robj->tbo.ttm) ||
	    (robj->flags & LOONGGPU_GEM_CREATE_NO_CPU_ACCESS)) {
		lg_drm_gem_object_put(gobj);
		return -EPERM;
	}
	*offset_p = loonggpu_bo_mmap_offset(robj);
	lg_drm_gem_object_put(gobj);
	return 0;
}

int loonggpu_gem_mmap_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *filp)
{
	union drm_loonggpu_gem_mmap *args = data;
	uint32_t handle = args->in.handle;
	memset(args, 0, sizeof(*args));
	return loonggpu_mode_dumb_mmap(filp, dev, handle, &args->out.addr_ptr);
}

/**
 * loonggpu_gem_timeout - calculate jiffies timeout from absolute value
 *
 * @timeout_ns: timeout in ns
 *
 * Calculate the timeout in jiffies from an absolute timeout in ns.
 */
unsigned long loonggpu_gem_timeout(uint64_t timeout_ns)
{
	unsigned long timeout_jiffies;
	ktime_t timeout;

	/* clamp timeout if it's to large */
	if (((int64_t)timeout_ns) < 0)
		return MAX_SCHEDULE_TIMEOUT;

	timeout = ktime_sub(ns_to_ktime(timeout_ns), ktime_get());
	if (ktime_to_ns(timeout) < 0)
		return 0;

	timeout_jiffies = nsecs_to_jiffies(ktime_to_ns(timeout));
	/*  clamp timeout to avoid unsigned-> signed overflow */
	if (timeout_jiffies > MAX_SCHEDULE_TIMEOUT)
		return MAX_SCHEDULE_TIMEOUT - 1;

	return timeout_jiffies;
}

int loonggpu_gem_wait_idle_ioctl(struct drm_device *dev, void *data,
			      struct drm_file *filp)
{
	union drm_loonggpu_gem_wait_idle *args = data;
	struct drm_gem_object *gobj;
	struct loonggpu_bo *robj;
	lg_dma_resv_t *resv;
	uint32_t handle = args->in.handle;
	unsigned long timeout = loonggpu_gem_timeout(args->in.timeout);
	int r = 0;
	long ret;

	gobj = drm_gem_object_lookup(filp, handle);
	if (gobj == NULL) {
		return -ENOENT;
	}
	robj = gem_to_loonggpu_bo(gobj);
	resv = to_dma_resv(robj);
	ret = lg_dma_resv_wait_timeout_rcu(resv, LG_DMA_RESV_USAGE_READ, true, true, timeout);

	/* ret == 0 means not signaled,
	 * ret > 0 means signaled
	 * ret < 0 means interrupted before timeout
	 */
	if (ret >= 0) {
		memset(args, 0, sizeof(*args));
		args->out.status = (ret == 0);
	} else
		r = ret;

	lg_drm_gem_object_put(gobj);
	return r;
}

int loonggpu_gem_metadata_ioctl(struct drm_device *dev, void *data,
				struct drm_file *filp)
{
	struct drm_loonggpu_gem_metadata *args = data;
	struct drm_gem_object *gobj;
	struct loonggpu_bo *robj;
	int r = -1;

	DRM_DEBUG("%d \n", args->handle);
	gobj = drm_gem_object_lookup(filp, args->handle);
	if (gobj == NULL)
		return -ENOENT;
	robj = gem_to_loonggpu_bo(gobj);

	r = loonggpu_bo_reserve(robj, false);
	if (unlikely(r != 0))
		goto out;

	if (args->op == LOONGGPU_GEM_METADATA_OP_GET_METADATA) {
		loonggpu_bo_get_tiling_flags(robj, &args->data.tiling_info);
		r = loonggpu_bo_get_metadata(robj, args->data.data,
					   sizeof(args->data.data),
					   &args->data.data_size_bytes,
					   &args->data.flags);
	} else if (args->op == LOONGGPU_GEM_METADATA_OP_SET_METADATA) {
		if (args->data.data_size_bytes > sizeof(args->data.data)) {
			r = -EINVAL;
			goto unreserve;
		}
		r = loonggpu_bo_set_tiling_flags(robj, args->data.tiling_info);
		if (!r)
			r = loonggpu_bo_set_metadata(robj, args->data.data,
						   args->data.data_size_bytes,
						   args->data.flags);
	}

unreserve:
	loonggpu_bo_unreserve(robj);
out:
	lg_drm_gem_object_put(gobj);
	return r;
}

/**
 * loonggpu_gem_va_update_vm -update the bo_va in its VM
 *
 * @adev: loonggpu_device pointer
 * @vm: vm to update
 * @bo_va: bo_va to update
 * @operation: map, unmap or clear
 *
 * Update the bo_va directly after setting its address. Errors are not
 * vital here, so they are not reported back to userspace.
 */
static void loonggpu_gem_va_update_vm(struct loonggpu_device *adev,
				    struct loonggpu_vm *vm,
				    struct loonggpu_bo_va *bo_va,
				    uint32_t operation)
{
	int r;

	if (!loonggpu_vm_ready(vm))
		return;

	r = loonggpu_vm_clear_freed(adev, vm, NULL);
	if (r)
		goto error;

	if (operation == LOONGGPU_VA_OP_MAP ||
	    operation == LOONGGPU_VA_OP_REPLACE) {
		r = loonggpu_vm_bo_update(adev, bo_va, false);
		if (r)
			goto error;
	}

	r = loonggpu_vm_update_directories(adev, vm);

error:
	if (r && r != -ERESTARTSYS)
		DRM_ERROR("Couldn't update BO_VA (%d)\n", r);
}

int loonggpu_gem_va_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *filp)
{
	const uint32_t valid_flags = LOONGGPU_VM_DELAY_UPDATE |
		LOONGGPU_VM_PAGE_READABLE | LOONGGPU_VM_PAGE_WRITEABLE |
		LOONGGPU_VM_PAGE_EXECUTABLE | LOONGGPU_VM_MTYPE_MASK;
	const uint32_t prt_flags = LOONGGPU_VM_DELAY_UPDATE |
		LOONGGPU_VM_PAGE_PRT;

	struct drm_loonggpu_gem_va *args = data;
	struct drm_gem_object *gobj;
	struct loonggpu_device *adev = dev->dev_private;
	struct loonggpu_fpriv *fpriv = filp->driver_priv;
	struct loonggpu_bo *abo;
	struct loonggpu_bo_va *bo_va;
	struct loonggpu_bo_list_entry vm_pd;
	struct ttm_validate_buffer tv;
	struct ww_acquire_ctx ticket;
	struct list_head list, duplicates;
	uint64_t va_flags;
	int r = 0;

	if (adev->family_type == CHIP_NO_GPU)
		return 0;

	if (args->va_address < LOONGGPU_VA_RESERVED_SIZE) {
		dev_dbg(&adev->pdev->dev,
			"va_address 0x%LX is in reserved area 0x%LX\n",
			args->va_address, LOONGGPU_VA_RESERVED_SIZE);
		return -EINVAL;
	}

	if (args->va_address >= LOONGGPU_VA_HOLE_START &&
	    args->va_address < LOONGGPU_VA_HOLE_END) {
		dev_dbg(&adev->pdev->dev,
			"va_address 0x%LX is in VA hole 0x%LX-0x%LX\n",
			args->va_address, LOONGGPU_VA_HOLE_START,
			LOONGGPU_VA_HOLE_END);
		return -EINVAL;
	}

	args->va_address &= LOONGGPU_VA_HOLE_MASK;

	if ((args->flags & ~valid_flags) && (args->flags & ~prt_flags)) {
		dev_dbg(&adev->pdev->dev, "invalid flags combination 0x%08X\n",
			args->flags);
		return -EINVAL;
	}

	switch (args->operation) {
	case LOONGGPU_VA_OP_MAP:
	case LOONGGPU_VA_OP_UNMAP:
	case LOONGGPU_VA_OP_CLEAR:
	case LOONGGPU_VA_OP_REPLACE:
		break;
	default:
		dev_dbg(&adev->pdev->dev, "unsupported operation %d\n",
			args->operation);
		return -EINVAL;
	}

	INIT_LIST_HEAD(&list);
	INIT_LIST_HEAD(&duplicates);
	if ((args->operation != LOONGGPU_VA_OP_CLEAR) &&
	    !(args->flags & LOONGGPU_VM_PAGE_PRT)) {
		gobj = drm_gem_object_lookup(filp, args->handle);
		if (gobj == NULL)
			return -ENOENT;
		abo = gem_to_loonggpu_bo(gobj);
		tv.bo = &abo->tbo;
		lg_ttm_validate_buffer_set_shared(&tv,
				!!(abo->flags & LOONGGPU_GEM_CREATE_VM_ALWAYS_VALID));
		list_add(&tv.head, &list);
	} else {
		gobj = NULL;
		abo = NULL;
	}

	loonggpu_vm_get_pd_bo(&fpriv->vm, &list, &vm_pd);

	r = lg_ttm_eu_reserve_buffers(&ticket, &list, true, &duplicates, false);
	if (r)
		goto error_unref;

	if (abo) {
		bo_va = loonggpu_vm_bo_find(&fpriv->vm, abo);
		if (!bo_va) {
			r = -ENOENT;
			goto error_backoff;
		}
	} else if (args->operation != LOONGGPU_VA_OP_CLEAR) {
		bo_va = fpriv->prt_va;
	} else {
		bo_va = NULL;
	}

	switch (args->operation) {
	case LOONGGPU_VA_OP_MAP:
		r = loonggpu_vm_alloc_pts(adev, bo_va->base.vm, args->va_address,
					args->map_size);
		if (r)
			goto error_backoff;

		va_flags = loonggpu_gmc_get_pte_flags(adev, args->flags);
		r = loonggpu_vm_bo_map(adev, bo_va, args->va_address,
				     args->offset_in_bo, args->map_size,
				     va_flags);
		break;
	case LOONGGPU_VA_OP_UNMAP:
		r = loonggpu_vm_bo_unmap(adev, bo_va, args->va_address);
		break;

	case LOONGGPU_VA_OP_CLEAR:
		r = loonggpu_vm_bo_clear_mappings(adev, &fpriv->vm,
						args->va_address,
						args->map_size);
		break;
	case LOONGGPU_VA_OP_REPLACE:
		r = loonggpu_vm_alloc_pts(adev, bo_va->base.vm, args->va_address,
					args->map_size);
		if (r)
			goto error_backoff;

		va_flags = loonggpu_gmc_get_pte_flags(adev, args->flags);
		r = loonggpu_vm_bo_replace_map(adev, bo_va, args->va_address,
					     args->offset_in_bo, args->map_size,
					     va_flags);
		break;
	default:
		break;
	}
	if (!r && !(args->flags & LOONGGPU_VM_DELAY_UPDATE) && !loonggpu_vm_debug)
		loonggpu_gem_va_update_vm(adev, &fpriv->vm, bo_va,
					args->operation);

error_backoff:
	ttm_eu_backoff_reservation(&ticket, &list);

error_unref:
	lg_drm_gem_object_put(gobj);
	return r;
}

int loonggpu_gem_op_ioctl(struct drm_device *dev, void *data,
			struct drm_file *filp)
{
	struct loonggpu_device *adev = dev->dev_private;
	struct drm_loonggpu_gem_op *args = data;
	struct drm_gem_object *gobj;
	struct loonggpu_bo *robj;
	int r;

	gobj = drm_gem_object_lookup(filp, args->handle);
	if (gobj == NULL) {
		return -ENOENT;
	}
	robj = gem_to_loonggpu_bo(gobj);

	r = loonggpu_bo_reserve(robj, false);
	if (unlikely(r))
		goto out;

	switch (args->op) {
	case LOONGGPU_GEM_OP_GET_GEM_CREATE_INFO: {
		struct drm_loonggpu_gem_create_in info;
		void __user *out = u64_to_user_ptr(args->value);

		info.bo_size = lg_gbo_to_gem_obj(robj).size;
		info.alignment = robj->tbo.page_alignment << PAGE_SHIFT;
		info.domains = robj->preferred_domains;
		info.domain_flags = robj->flags;
		loonggpu_bo_unreserve(robj);
		if (copy_to_user(out, &info, sizeof(info)))
			r = -EFAULT;
		break;
	}
	case LOONGGPU_GEM_OP_SET_PLACEMENT:
		if (robj->prime_shared_count && (args->value & LOONGGPU_GEM_DOMAIN_VRAM)) {
			r = -EINVAL;
			loonggpu_bo_unreserve(robj);
			break;
		}
		if (loonggpu_ttm_tt_get_usermm(robj->tbo.ttm)) {
			r = -EPERM;
			loonggpu_bo_unreserve(robj);
			break;
		}
		robj->preferred_domains = args->value & (LOONGGPU_GEM_DOMAIN_VRAM |
							LOONGGPU_GEM_DOMAIN_GTT |
							LOONGGPU_GEM_DOMAIN_CPU);
		robj->allowed_domains = robj->preferred_domains;
		if (robj->allowed_domains == LOONGGPU_GEM_DOMAIN_VRAM)
			robj->allowed_domains |= LOONGGPU_GEM_DOMAIN_GTT;

		if (robj->flags & LOONGGPU_GEM_CREATE_VM_ALWAYS_VALID)
			loonggpu_vm_bo_invalidate(adev, robj, true);

		loonggpu_bo_unreserve(robj);
		break;
	default:
		loonggpu_bo_unreserve(robj);
		r = -EINVAL;
	}

out:
	lg_drm_gem_object_put(gobj);
	return r;
}

int loonggpu_mode_dumb_create(struct drm_file *file_priv,
			    struct drm_device *dev,
			    struct drm_mode_create_dumb *args)
{
	struct loonggpu_device *adev = dev->dev_private;
	struct drm_gem_object *gobj;
	uint32_t handle;
	u32 domain;
	int r;

	args->pitch = loonggpu_align_pitch(adev, args->width,
					 DIV_ROUND_UP(args->bpp, 8), 0);
	args->size = (u64)args->pitch * args->height;
	args->size = ALIGN(args->size, PAGE_SIZE);
	domain = loonggpu_bo_get_preferred_pin_domain(adev,
				loonggpu_display_supported_domains(adev));
	r = loonggpu_gem_object_create(adev, args->size, 0, domain,
				     LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED |
				     LOONGGPU_GEM_CREATE_VRAM_CONTIGUOUS |
				     LOONGGPU_GEM_CREATE_VRAM_CLEARED,
				     ttm_bo_type_device, NULL, &gobj);
	if (r)
		return -ENOMEM;

	r = drm_gem_handle_create(file_priv, gobj, &handle);
	/* drop reference from allocate - handle holds it now */
	lg_drm_gem_object_put(gobj);
	if (r) {
		return r;
	}
	args->handle = handle;
	return 0;
}

#if defined(CONFIG_DEBUG_FS)

#define loonggpu_debugfs_gem_bo_print_flag(m, bo, flag) do {	\
	if (bo->flags & (LOONGGPU_GEM_CREATE_ ## flag)) {	\
		seq_printf((m), " " #flag);		\
	}	\
} while (0)

static int loonggpu_debugfs_gem_bo_info(int id, void *ptr, void *data)
{
	struct drm_gem_object *gobj = ptr;
	struct loonggpu_bo *bo = gem_to_loonggpu_bo(gobj);
	struct seq_file *m = data;

	struct dma_buf_attachment *attachment;
	struct dma_buf *dma_buf;
	unsigned domain;
	const char *placement;
	unsigned pin_count;

	domain = loonggpu_mem_type_to_domain(lg_get_bo_mem_type(bo));
	switch (domain) {
	case LOONGGPU_GEM_DOMAIN_VRAM:
		placement = "VRAM";
		break;
	case LOONGGPU_GEM_DOMAIN_GTT:
		placement = " GTT";
		break;
	case LOONGGPU_GEM_DOMAIN_CPU:
	default:
		placement = " CPU";
		break;
	}
	seq_printf(m, "\t0x%08x: %12ld byte %s",
		   id, loonggpu_bo_size(bo), placement);

	pin_count = READ_ONCE(lg_bo_pin_count_ptr);
	if (pin_count)
		seq_printf(m, " pin count %d", pin_count);

	dma_buf = READ_ONCE(lg_gbo_to_gem_obj(bo).dma_buf);
	attachment = READ_ONCE(lg_gbo_to_gem_obj(bo).import_attach);

	if (attachment)
		seq_printf(m, " imported from %p", dma_buf);
	else if (dma_buf)
		seq_printf(m, " exported as %p", dma_buf);

	loonggpu_debugfs_gem_bo_print_flag(m, bo, CPU_ACCESS_REQUIRED);
	loonggpu_debugfs_gem_bo_print_flag(m, bo, NO_CPU_ACCESS);
	loonggpu_debugfs_gem_bo_print_flag(m, bo, CPU_GTT_USWC);
	loonggpu_debugfs_gem_bo_print_flag(m, bo, VRAM_CLEARED);
	loonggpu_debugfs_gem_bo_print_flag(m, bo, SHADOW);
	loonggpu_debugfs_gem_bo_print_flag(m, bo, VRAM_CONTIGUOUS);
	loonggpu_debugfs_gem_bo_print_flag(m, bo, VM_ALWAYS_VALID);
	loonggpu_debugfs_gem_bo_print_flag(m, bo, EXPLICIT_SYNC);

	seq_printf(m, "\n");

	return 0;
}

static int loonggpu_debugfs_gem_info(struct seq_file *m, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *dev = node->minor->dev;
	struct drm_file *file;
	int r;

	r = mutex_lock_interruptible(&dev->filelist_mutex);
	if (r)
		return r;

	list_for_each_entry(file, &dev->filelist, lhead) {
		struct task_struct *task;

		/*
		 * Although we have a valid reference on file->pid, that does
		 * not guarantee that the task_struct who called get_pid() is
		 * still alive (e.g. get_pid(current) => fork() => exit()).
		 * Therefore, we need to protect this ->comm access using RCU.
		 */
		rcu_read_lock();
		task = pid_task(file->pid, PIDTYPE_PID);
		seq_printf(m, "pid %8d command %s:\n", pid_nr(file->pid),
			   task ? task->comm : "<unknown>");
		rcu_read_unlock();

		spin_lock(&file->table_lock);
		idr_for_each(&file->object_idr, loonggpu_debugfs_gem_bo_info, m);
		spin_unlock(&file->table_lock);
	}

	mutex_unlock(&dev->filelist_mutex);
	return 0;
}

static const struct drm_info_list loonggpu_debugfs_gem_list[] = {
	{"loonggpu_gem_info", &loonggpu_debugfs_gem_info, 0, NULL},
};
#endif

int loonggpu_debugfs_gem_init(struct loonggpu_device *adev)
{
#if defined(CONFIG_DEBUG_FS)
	return loonggpu_debugfs_add_files(adev, loonggpu_debugfs_gem_list, 1);
#endif
	return 0;
}
