/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/slab.h>
#include <linux/random.h>

#include "kcd_ipc.h"
#include "kcd_priv.h"
#include "loonggpu_lgkcd.h"

#define KCD_IPC_HASH_TABLE_SIZE_SHIFT 4
#define KCD_IPC_HASH_TABLE_SIZE_MASK ((1 << KCD_IPC_HASH_TABLE_SIZE_SHIFT) - 1)

static struct kcd_ipc_handles {
	DECLARE_HASHTABLE(handles, KCD_IPC_HASH_TABLE_SIZE_SHIFT);
	struct mutex lock;
} kcd_ipc_handles;

/* Since, handles are random numbers, it can be used directly as hashing key.
 * The least 4 bits of the handle are used as key. However, during import all
 * 128 bits of the handle are checked to prevent handle snooping.
 */
#define HANDLE_TO_KEY(sh) ((*(uint64_t *)sh) & KCD_IPC_HASH_TABLE_SIZE_MASK)

int kcd_ipc_store_insert(struct dma_buf *dmabuf, struct kcd_ipc_obj **ipc_obj,
			 uint32_t flags, uint32_t *restore_handle)
{
	struct kcd_ipc_obj *obj;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	if (restore_handle)
		memcpy(obj->share_handle, restore_handle, sizeof(obj->share_handle));
	else
		get_random_bytes(obj->share_handle, sizeof(obj->share_handle));

	mutex_lock(&kcd_ipc_handles.lock);
	if (restore_handle) {
		struct kcd_ipc_obj *entry;

		/* When doing CRIU restore, we may have a race condition where two processes try
		 * to insert handles with the same key. Make sure this key does not already exist
		 */
		hlist_for_each_entry(entry,
			&kcd_ipc_handles.handles[HANDLE_TO_KEY(obj->share_handle)], node) {
			if (!memcmp(entry->share_handle,
				    obj->share_handle,
				    sizeof(entry->share_handle))) {
				mutex_unlock(&kcd_ipc_handles.lock);
				kfree(obj);
				return -EINVAL;
			}
		}
	}

	/* The initial ref belongs to the allocator process.
	 * The IPC object store itself does not hold a ref since
	 * there is no specific moment in time where that ref should
	 * be dropped, except "when there are no more userspace processes
	 * holding a ref to the object". Therefore the removal from IPC
	 * storage happens at ipc_obj release time.
	 */
	kref_init(&obj->ref);
	obj->dmabuf = dmabuf;
	obj->flags = flags;

	hlist_add_head(&obj->node, &kcd_ipc_handles.handles[HANDLE_TO_KEY(obj->share_handle)]);
	mutex_unlock(&kcd_ipc_handles.lock);

	if (ipc_obj)
		*ipc_obj = obj;

	return 0;
}

static void ipc_obj_release(struct kref *r)
{
	struct kcd_ipc_obj *obj;

	obj = container_of(r, struct kcd_ipc_obj, ref);

	mutex_lock(&kcd_ipc_handles.lock);
	hash_del(&obj->node);
	mutex_unlock(&kcd_ipc_handles.lock);

	dma_buf_put(obj->dmabuf);
	kfree(obj);
}

static struct kcd_ipc_obj *ipc_obj_get(struct kcd_ipc_obj *obj)
{
	if (kref_get_unless_zero(&obj->ref))
		return obj;
	return NULL;
}

void kcd_ipc_obj_put(struct kcd_ipc_obj **obj)
{
	if (*obj) {
		kref_put(&(*obj)->ref, ipc_obj_release);
		*obj = NULL;
	}
}

int kcd_ipc_init(void)
{
	mutex_init(&kcd_ipc_handles.lock);
	hash_init(kcd_ipc_handles.handles);
	return 0;
}

static int kcd_import_dmabuf_create_kcd_bo(struct kcd_node *dev,
			  struct kcd_process *p,
			  uint32_t gpu_id,
			  struct dma_buf *dmabuf, struct kcd_ipc_obj *ipc_obj,
			  uint64_t va_addr, uint64_t *handle,
			  uint64_t *mmap_offset, bool restore)
{
	int r;
	struct kgd_mem *mem;
	uint64_t size;
	int idr_handle;
	struct kcd_process_device *pdd = NULL;

	if (!handle)
		return -EINVAL;

	if (!dev)
		return -EINVAL;

	if (restore)
		idr_handle = GET_IDR_HANDLE(*handle);
	else
		idr_handle = -1;

	pdd = kcd_bind_process_to_device(dev, p);
	if (IS_ERR(pdd))
		return PTR_ERR(pdd);

	r = loonggpu_lgkcd_gpuvm_import_dmabuf(dev->adev, dmabuf, ipc_obj,
					      va_addr, pdd->drm_priv,
					      &mem, &size, mmap_offset);
	if (r)
		return r;

	idr_handle = kcd_process_device_create_obj_handle(pdd, mem,
						va_addr, size, 0, 0, idr_handle);
	if (idr_handle < 0) {
		r = -EFAULT;
		goto err_free;
	}

	*handle = MAKE_HANDLE(gpu_id, idr_handle);

	return 0;

err_free:
	loonggpu_lgkcd_gpuvm_free_memory_of_gpu(dev->adev, (struct kgd_mem *)mem, pdd->drm_priv, NULL);
	return r;
}

int kcd_ipc_import_dmabuf(struct kcd_node *dev,
					   struct kcd_process *p,
					   uint32_t gpu_id, int dmabuf_fd,
					   uint64_t va_addr, uint64_t *handle,
					   uint64_t *mmap_offset)
{
	int r;
	struct dma_buf *dmabuf = dma_buf_get(dmabuf_fd);

	if (!dmabuf)
		return -EINVAL;

	mutex_lock(&p->mutex);

	r = kcd_import_dmabuf_create_kcd_bo(dev, p, gpu_id, dmabuf, NULL,
					    va_addr, handle, mmap_offset, false);

	mutex_unlock(&p->mutex);
	dma_buf_put(dmabuf);
	return r;
}

int kcd_ipc_import_handle(struct kcd_node *dev, struct kcd_process *p,
			  uint32_t gpu_id, uint32_t *share_handle,
			  uint64_t va_addr, uint64_t *handle,
			  uint64_t *mmap_offset, uint32_t *pflags, bool restore)
{
	int r;
	struct kcd_ipc_obj *entry, *found = NULL;

	mutex_lock(&kcd_ipc_handles.lock);
	/* Convert the user provided handle to hash key and search only in that
	 * bucket
	 */
	hlist_for_each_entry(entry,
		&kcd_ipc_handles.handles[HANDLE_TO_KEY(share_handle)], node) {
		if (!memcmp(entry->share_handle, share_handle,
			    sizeof(entry->share_handle))) {
			found = ipc_obj_get(entry);
			break;
		}
	}
	mutex_unlock(&kcd_ipc_handles.lock);

	if (!found)
		return -EINVAL;

	pr_debug("Found ipc_dma_buf: %p\n", found->dmabuf);

	if (!restore) {
		mutex_lock(&p->mutex);
	}

	r = kcd_import_dmabuf_create_kcd_bo(dev, p, gpu_id,
					    found->dmabuf, found,
					    va_addr, handle, mmap_offset,
					    restore);
	if (!restore) {
		mutex_unlock(&p->mutex);
	}
	if (r)
		goto error_unref;

	if (pflags)
		*pflags = found->flags;

	return r;

error_unref:
	kcd_ipc_obj_put(&found);
	return r;
}

int kcd_ipc_export_as_handle(struct kcd_node *dev, struct kcd_process *p,
			     uint64_t handle, uint32_t *ipc_handle,
			     uint32_t flags)
{
	struct kcd_process_device *pdd = NULL;
	struct kcd_ipc_obj *ipc_obj;
	struct kcd_bo *kcd_bo = NULL;
	struct kgd_mem *mem;
	int r;

	if (!dev || !ipc_handle)
		return -EINVAL;

	/* Protect kgd_mem object from being deleted by another thread */
	mutex_lock(&p->mutex);
	pdd = kcd_bind_process_to_device(dev, p);
	if (IS_ERR(pdd)) {
		pr_err("Failed to get pdd\n");
		r = PTR_ERR(pdd);
		goto unlock;
	}

	kcd_bo = kcd_process_device_find_bo(pdd, GET_IDR_HANDLE(handle));

	if (!kcd_bo) {
		pr_err("Failed to get bo");
		r = -EINVAL;
		goto unlock;
	}
	mem = (struct kgd_mem *)kcd_bo->mem;

	r = loonggpu_lgkcd_gpuvm_export_ipc_obj(dev->adev, pdd->drm_priv, mem,
					       &ipc_obj, flags, NULL);
	if (r)
		goto unlock;

	memcpy(ipc_handle, ipc_obj->share_handle,
	       sizeof(ipc_obj->share_handle));

unlock:
	mutex_unlock(&p->mutex);
	return r;
}
