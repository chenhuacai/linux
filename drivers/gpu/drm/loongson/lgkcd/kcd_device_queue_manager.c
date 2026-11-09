// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright 2014-2022 Advanced Micro Devices, Inc.
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
 *
 */

#include <linux/ratelimit.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/sched.h>
#include <linux/mmu_context.h>
#include "kcd_priv.h"
#include "kcd_device_queue_manager.h"
#include "kcd_mqd_manager.h"
#include "kcd_kernel_queue.h"
#include "loonggpu_lgkcd.h"
#include "kcd_debug.h"

static int execute_queues_cpsch(struct device_queue_manager *dqm,
				enum kcd_unmap_queues_filter filter,
				uint32_t filter_param);
static int unmap_queues_cpsch(struct device_queue_manager *dqm,
				enum kcd_unmap_queues_filter filter,
				uint32_t filter_param,
				bool reset);

static int map_queues_cpsch(struct device_queue_manager *dqm);

static void deallocate_sdma_queue(struct device_queue_manager *dqm,
				struct queue *q);

static int allocate_sdma_queue(struct device_queue_manager *dqm,
				struct queue *q, const uint32_t *restore_sdma_id);
static void kcd_process_hw_exception(struct work_struct *work);

static inline
enum KCD_MQD_TYPE get_mqd_type_from_queue_type(enum kcd_queue_type type)
{
	if (type == KCD_QUEUE_TYPE_SDMA)
		return KCD_MQD_TYPE_SDMA;
	return KCD_MQD_TYPE_CP;
}

unsigned int get_cp_queues_num(struct device_queue_manager *dqm)
{
	return bitmap_weight(dqm->dev->kcd->shared_resources.cp_queue_bitmap,
				KGD_MAX_QUEUES);
}

unsigned int get_queues_per_pipe(struct device_queue_manager *dqm)
{
	return dqm->dev->kcd->shared_resources.num_queue_per_pipe;
}

unsigned int get_pipes_per_mec(struct device_queue_manager *dqm)
{
	return dqm->dev->kcd->shared_resources.num_pipe_per_mec;
}

static unsigned int get_num_all_sdma_engines(struct device_queue_manager *dqm)
{
	return 1;
}

unsigned int get_num_sdma_queues(struct device_queue_manager *dqm)
{
	return kcd_get_num_sdma_engines(dqm->dev) *
		dqm->dev->kcd->device_info.num_sdma_queues_per_engine;
}

static void init_sdma_bitmaps(struct device_queue_manager *dqm)
{
	bitmap_zero(dqm->sdma_bitmap, KCD_MAX_SDMA_QUEUES);
	bitmap_set(dqm->sdma_bitmap, 0, get_num_sdma_queues(dqm));

	/* Mask out the reserved queues */
	bitmap_andnot(dqm->sdma_bitmap, dqm->sdma_bitmap,
		      dqm->dev->kcd->device_info.reserved_sdma_queues_bitmap,
		      KCD_MAX_SDMA_QUEUES);
}

static void kcd_hws_hang(struct device_queue_manager *dqm)
{
	/*
	 * Issue a GPU reset if HWS is unresponsive
	 */
	dqm->is_hws_hang = true;

	/* It's possible we're detecting a HWS hang in the
	 * middle of a GPU reset. No need to schedule another
	 * reset in this case.
	 */
	if (!dqm->is_resetting)
		schedule_work(&dqm->hw_exception_work);
}

static void increment_queue_count(struct device_queue_manager *dqm,
				  struct qcm_process_device *qpd,
				  struct queue *q)
{
	dqm->active_queue_count++;
	if (q->properties.type == KCD_QUEUE_TYPE_COMPUTE)
		dqm->active_cp_queue_count++;
}

static void decrement_queue_count(struct device_queue_manager *dqm,
				  struct qcm_process_device *qpd,
				  struct queue *q)
{
	dqm->active_queue_count--;
	if (q->properties.type == KCD_QUEUE_TYPE_COMPUTE)
		dqm->active_cp_queue_count--;
}

/*
 * Allocate a doorbell ID to this queue.
 * If doorbell_id is passed in, make sure requested ID is valid then allocate it.
 */
static int allocate_doorbell(struct qcm_process_device *qpd,
			     struct queue *q,
			     uint32_t const *restore_id)
{
	struct kcd_node *dev = qpd->dqm->dev;

	if (q->properties.type == KCD_QUEUE_TYPE_SDMA) {

		uint32_t *idx_offset = dev->kcd->shared_resources.sdma_doorbell_idx;

		/*
		 * q->properties.sdma_engine_id corresponds to the virtual
		 * sdma engine number. However, for doorbell allocation,
		 * we need the physical sdma engine id in order to get the
		 * correct doorbell offset.
		 */
		uint32_t valid_id = idx_offset[qpd->dqm->dev->node_id *
					       get_num_all_sdma_engines(qpd->dqm) +
					       q->properties.sdma_engine_id]
						+ (q->properties.sdma_queue_id & 1)
						* KCD_QUEUE_DOORBELL_MIRROR_OFFSET
						+ (q->properties.sdma_queue_id >> 1);

		if (restore_id && *restore_id != valid_id)
			return -EINVAL;

		if (valid_id % KCD_QUEUE_DOORBELL_MIRROR_OFFSET >= 
			dev->kcd->shared_resources.non_cp_doorbells_end)
			return -EINVAL;

		q->doorbell_id = valid_id;
	} else {
		/* For CP queues on LG2xx */
		if (restore_id) {
			/* make sure that ID is free  */
			if (__test_and_set_bit(*restore_id, qpd->doorbell_bitmap))
				return -EINVAL;

			q->doorbell_id = *restore_id;
		} else {
			/* or reserve a free doorbell ID */
			unsigned int found;

			found = find_first_zero_bit(qpd->doorbell_bitmap,
						    KCD_MAX_NUM_OF_QUEUES_PER_PROCESS);
			if (found >= KCD_MAX_NUM_OF_QUEUES_PER_PROCESS) {
				pr_debug("No doorbells available");
				return -EBUSY;
			}
			set_bit(found, qpd->doorbell_bitmap);
			q->doorbell_id = found;
		}
	}

	q->properties.doorbell_off = loonggpu_doorbell_index_on_bar(dev->adev,
								  qpd->proc_doorbells,
								  q->doorbell_id);
	return 0;
}

static void deallocate_doorbell(struct qcm_process_device *qpd,
				struct queue *q)
{
	unsigned int old;

	if (q->properties.type == KCD_QUEUE_TYPE_SDMA)
		return;

	old = test_and_clear_bit(q->doorbell_id, qpd->doorbell_bitmap);
	WARN_ON(!old);
}

static int update_queue(struct device_queue_manager *dqm, struct queue *q)
{
	int retval = 0;
	struct mqd_manager *mqd_mgr;
	struct kcd_process_device *pdd;
	bool prev_active = false;

	dqm_lock(dqm);
	pdd = kcd_get_process_device_data(q->device, q->process);
	if (!pdd) {
		retval = -ENODEV;
		goto out_unlock;
	}
	mqd_mgr = dqm->mqd_mgrs[get_mqd_type_from_queue_type(
			q->properties.type)];

	/* Save previous activity state for counters */
	prev_active = q->properties.is_active;

	/* Make sure the queue is unmapped before updating the MQD */
	retval = unmap_queues_cpsch(dqm,
					KCD_UNMAP_QUEUES_FILTER_DYNAMIC_QUEUES, 0, false);

	if (retval) {
		pr_err("unmap queue failed\n");
		goto out_unlock;
	}

	mqd_mgr->update_mqd(mqd_mgr, q->mqd, &q->properties);

	/*
	 * check active state vs. the previous state and modify
	 * counter accordingly. map_queues_cpsch uses the
	 * dqm->active_queue_count to determine whether a new runlist must be
	 * uploaded.
	 */
	if (q->properties.is_active && !prev_active) {
		increment_queue_count(dqm, &pdd->qpd, q);
	} else if (!q->properties.is_active && prev_active) {
		decrement_queue_count(dqm, &pdd->qpd, q);
	} 

	retval = map_queues_cpsch(dqm);

out_unlock:
	dqm_unlock(dqm);
	return retval;
}

/* suspend_single_queue does not lock the dqm like the
 * evict_process_queues_cpsch. You should
 * lock the dqm before calling, and unlock after calling.
 *
 * The reason we don't lock the dqm is because this function may be
 * called on multiple queues in a loop, so rather than locking/unlocking
 * multiple times, we will just keep the dqm locked for all of the calls.
 */
static int suspend_single_queue(struct device_queue_manager *dqm,
				      struct kcd_process_device *pdd,
				      struct queue *q)
{
	bool is_new;

	if (q->properties.is_suspended)
		return 0;

	pr_debug("Suspending PASID %u queue [%i]\n",
			pdd->process->pasid,
			q->properties.queue_id);

	is_new = q->properties.exception_status & KCD_EC_MASK(EC_QUEUE_NEW);

	if (is_new || q->properties.is_being_destroyed) {
		pr_debug("Suspend: skip %s queue id %i\n",
				is_new ? "new" : "destroyed",
				q->properties.queue_id);
		return -EBUSY;
	}

	q->properties.is_suspended = true;
	if (q->properties.is_active) {
		decrement_queue_count(dqm, &pdd->qpd, q);
		q->properties.is_active = false;
	}

	return 0;
}

/* resume_single_queue does not lock the dqm like the functions
 * restore_process_queues_cpsch. You should
 * lock the dqm before calling, and unlock after calling.
 *
 * The reason we don't lock the dqm is because this function may be
 * called on multiple queues in a loop, so rather than locking/unlocking
 * multiple times, we will just keep the dqm locked for all of the calls.
 */
static int resume_single_queue(struct device_queue_manager *dqm,
				      struct qcm_process_device *qpd,
				      struct queue *q)
{
	struct kcd_process_device *pdd;

	if (!q->properties.is_suspended)
		return 0;

	pdd = qpd_to_pdd(qpd);

	pr_debug("Restoring from suspend PASID %u queue [%i]\n",
			    pdd->process->pasid,
			    q->properties.queue_id);

	q->properties.is_suspended = false;

	if (QUEUE_IS_ACTIVE(q->properties)) {
		q->properties.is_active = true;
		increment_queue_count(dqm, qpd, q);
	}

	return 0;
}

static int evict_process_queues_cpsch(struct device_queue_manager *dqm,
				      struct qcm_process_device *qpd)
{
	struct queue *q;
	struct kcd_process_device *pdd;
	int retval = 0;

	dqm_lock(dqm);
	if (qpd->evicted++ > 0) /* already evicted, do nothing */
		goto out;

	pdd = qpd_to_pdd(qpd);

	/* The debugger creates processes that temporarily have not acquired
	 * all VMs for all devices and has no VMs itself.
	 * Skip queue eviction on process eviction.
	 */
	if (!pdd->drm_priv)
		goto out;

	pr_debug_ratelimited("Evicting PASID 0x%x queues\n",
			    pdd->process->pasid);

	/* Mark all queues as evicted. Deactivate all active queues on
	 * the qpd.
	 */
	list_for_each_entry(q, &qpd->queues_list, list) {
		q->properties.is_evicted = true;
		if (!q->properties.is_active)
			continue;

		q->properties.is_active = false;
		decrement_queue_count(dqm, qpd, q);
	}
	pdd->last_evict_timestamp = get_jiffies_64();
	retval = execute_queues_cpsch(dqm,
					qpd->is_debug ?
					KCD_UNMAP_QUEUES_FILTER_ALL_QUEUES :
					KCD_UNMAP_QUEUES_FILTER_DYNAMIC_QUEUES, 0);

out:
	dqm_unlock(dqm);
	return retval;
}

static int restore_process_queues_cpsch(struct device_queue_manager *dqm,
					struct qcm_process_device *qpd)
{
	struct queue *q;
	struct kcd_process_device *pdd;
	uint64_t eviction_duration;
	int retval = 0;

	pdd = qpd_to_pdd(qpd);

	dqm_lock(dqm);
	if (WARN_ON_ONCE(!qpd->evicted)) /* already restored, do nothing */
		goto out;
	if (qpd->evicted > 1) { /* ref count still > 0, decrement & quit */
		qpd->evicted--;
		goto out;
	}

	/* The debugger creates processes that temporarily have not acquired
	 * all VMs for all devices and has no VMs itself.
	 * Skip queue restore on process restore.
	 */
	if (!pdd->drm_priv)
		goto vm_not_acquired;

	pr_debug_ratelimited("Restoring PASID 0x%x queues\n",
			    pdd->process->pasid);

	/* Update PD Base in QPD */
	qpd->page_table_base = loonggpu_lgkcd_gpuvm_get_process_page_dir(pdd->drm_priv);
	pr_debug("Updated PD address to 0x%llx\n", qpd->page_table_base);

	/* activate all active queues on the qpd */
	list_for_each_entry(q, &qpd->queues_list, list) {
		q->properties.is_evicted = false;
		if (!QUEUE_IS_ACTIVE(q->properties))
			continue;

		q->properties.is_active = true;
		increment_queue_count(dqm, &pdd->qpd, q);
	}

	retval = execute_queues_cpsch(dqm,
					KCD_UNMAP_QUEUES_FILTER_DYNAMIC_QUEUES, 0);
	eviction_duration = get_jiffies_64() - pdd->last_evict_timestamp;
	atomic64_add(eviction_duration, &pdd->evict_duration_counter);
vm_not_acquired:
	qpd->evicted = 0;
out:
	dqm_unlock(dqm);
	return retval;
}

static int register_process(struct device_queue_manager *dqm,
					struct qcm_process_device *qpd)
{
	struct device_process_node *n;
	struct kcd_process_device *pdd;
	uint64_t pd_base;

	n = kzalloc(sizeof(*n), GFP_KERNEL);
	if (!n)
		return -ENOMEM;

	n->qpd = qpd;

	pdd = qpd_to_pdd(qpd);
	/* Retrieve PD base */
	pd_base = loonggpu_lgkcd_gpuvm_get_process_page_dir(pdd->drm_priv);

	dqm_lock(dqm);
	list_add(&n->list, &dqm->queues);

	/* Update PD Base in QPD */
	qpd->page_table_base = pd_base;
	pr_debug("Updated PD address to 0x%llx\n", pd_base);

	dqm->processes_count++;

	dqm_unlock(dqm);

	/* Outside the DQM lock because under the DQM lock we can't do
	 * reclaim or take other locks that others hold while reclaiming.
	 */
	kcd_inc_compute_active(dqm->dev);

	return 0;
}

static int unregister_process(struct device_queue_manager *dqm,
					struct qcm_process_device *qpd)
{
	int retval;
	struct device_process_node *cur, *next;

	pr_debug("qpd->queues_list is %s\n",
			list_empty(&qpd->queues_list) ? "empty" : "not empty");

	retval = 0;
	dqm_lock(dqm);

	list_for_each_entry_safe(cur, next, &dqm->queues, list) {
		if (qpd == cur->qpd) {
			list_del(&cur->list);
			kfree(cur);
			dqm->processes_count--;
			goto out;
		}
	}
	/* qpd not found in dqm list */
	retval = 1;
out:
	dqm_unlock(dqm);

	/* Outside the DQM lock because under the DQM lock we can't do
	 * reclaim or take other locks that others hold while reclaiming.
	 */
	if (!retval)
		kcd_dec_compute_active(dqm->dev);

	return retval;
}

static void uninitialize(struct device_queue_manager *dqm)
{
	int i;

	WARN_ON(dqm->active_queue_count > 0 || dqm->processes_count > 0);

	kfree(dqm->allocated_queues);
	for (i = 0 ; i < KCD_MQD_TYPE_MAX ; i++)
		kfree(dqm->mqd_mgrs[i]);
	mutex_destroy(&dqm->lock_hidden);
}

static void pre_reset(struct device_queue_manager *dqm)
{
	dqm_lock(dqm);
	dqm->is_resetting = true;
	dqm_unlock(dqm);
}

static int allocate_sdma_queue(struct device_queue_manager *dqm,
				struct queue *q, const uint32_t *restore_sdma_id)
{
	int bit;

	if (q->properties.type == KCD_QUEUE_TYPE_SDMA) {
		if (bitmap_empty(dqm->sdma_bitmap, KCD_MAX_SDMA_QUEUES)) {
			pr_err("No more SDMA queue to allocate\n");
			return -ENOMEM;
		}

		if (restore_sdma_id) {
			/* Re-use existing sdma_id */
			if (!test_bit(*restore_sdma_id, dqm->sdma_bitmap)) {
				pr_err("SDMA queue already in use\n");
				return -EBUSY;
			}
			clear_bit(*restore_sdma_id, dqm->sdma_bitmap);
			q->sdma_id = *restore_sdma_id;
		} else {
			/* Find first available sdma_id */
			bit = find_first_bit(dqm->sdma_bitmap,
					     get_num_sdma_queues(dqm));
			clear_bit(bit, dqm->sdma_bitmap);
			q->sdma_id = bit;
		}

		q->properties.sdma_engine_id =
			q->sdma_id % kcd_get_num_sdma_engines(dqm->dev);
		q->properties.sdma_queue_id = q->sdma_id /
				kcd_get_num_sdma_engines(dqm->dev);
	}

	pr_debug("SDMA engine id: %d\n", q->properties.sdma_engine_id);
	pr_debug("SDMA queue id: %d\n", q->properties.sdma_queue_id);

	return 0;
}

static void deallocate_sdma_queue(struct device_queue_manager *dqm,
				struct queue *q)
{
	if (q->properties.type == KCD_QUEUE_TYPE_SDMA) {
		if (q->sdma_id >= get_num_sdma_queues(dqm))
			return;
		set_bit(q->sdma_id, dqm->sdma_bitmap);
	}
}

static int initialize_cpsch(struct device_queue_manager *dqm)
{
	dev_info(kcd_device, "num of queues: %d\n", get_pipes_per_mec(dqm) * 
						get_queues_per_pipe(dqm));

	mutex_init(&dqm->lock_hidden);
	INIT_LIST_HEAD(&dqm->queues);
	dqm->active_queue_count = dqm->processes_count = 0;
	dqm->active_cp_queue_count = 0;
	dqm->active_runlist = false;
	INIT_WORK(&dqm->hw_exception_work, kcd_process_hw_exception);

	init_sdma_bitmaps(dqm);

	return 0;
}

static int start_cpsch(struct device_queue_manager *dqm)
{
	int retval;

	retval = 0;

	dqm_lock(dqm);

	retval = pm_init(&dqm->packet_mgr, dqm);
	if (retval)
		goto fail_packet_manager_init;

	pr_debug("Allocating fence memory\n");

	/* allocate fence memory on the gart */
	retval = kcd_gtt_sa_allocate(dqm->dev, sizeof(*dqm->fence_addr),
					&dqm->fence_mem);

	if (retval)
		goto fail_allocate_vidmem;

	dqm->fence_addr = (uint64_t *)dqm->fence_mem->cpu_ptr;
	dqm->fence_gpu_addr = dqm->fence_mem->gpu_addr;

	/* clear hang status when driver try to start the hw scheduler */
	dqm->is_hws_hang = false;
	dqm->is_resetting = false;
	dqm->sched_running = true;

	execute_queues_cpsch(dqm, KCD_UNMAP_QUEUES_FILTER_DYNAMIC_QUEUES, 0);

	dqm_unlock(dqm);

	return 0;
fail_allocate_vidmem:
	pm_uninit(&dqm->packet_mgr, false);
fail_packet_manager_init:
	dqm_unlock(dqm);
	return retval;
}

static int stop_cpsch(struct device_queue_manager *dqm)
{
	bool hanging;

	dqm_lock(dqm);
	if (!dqm->sched_running) {
		dqm_unlock(dqm);
		return 0;
	}

	if (!dqm->is_hws_hang) {
		unmap_queues_cpsch(dqm, KCD_UNMAP_QUEUES_FILTER_ALL_QUEUES, 0, false);
	}

	hanging = dqm->is_hws_hang || dqm->is_resetting;
	dqm->sched_running = false;

	pm_release_ib(&dqm->packet_mgr);

	kcd_gtt_sa_free(dqm->dev, dqm->fence_mem);
	pm_uninit(&dqm->packet_mgr, hanging);
	dqm_unlock(dqm);

	return 0;
}

static int create_queue_cpsch(struct device_queue_manager *dqm, struct queue *q,
			struct qcm_process_device *qpd,
			const void *restore_mqd)
{
	int retval;
	struct mqd_manager *mqd_mgr;

	if (dqm->total_queue_count >= max_num_of_queues_per_device) {
		pr_warn("Can't create new usermode queue because %d queues were already created\n",
				dqm->total_queue_count);
		retval = -EPERM;
		goto out;
	}

	if (q->properties.type == KCD_QUEUE_TYPE_SDMA) {
		dqm_lock(dqm);
		retval = allocate_sdma_queue(dqm, q, NULL);
		dqm_unlock(dqm);
		if (retval)
			goto out;
	}
	if (dqm->dev->kcd->has_doorbells) {
		retval = allocate_doorbell(qpd, q, NULL);
		if (retval)
			goto out_deallocate_sdma_queue;
	}

	mqd_mgr = dqm->mqd_mgrs[get_mqd_type_from_queue_type(
			q->properties.type)];

	q->mqd_mem_obj = mqd_mgr->allocate_mqd(mqd_mgr->dev, &q->properties);
	if (!q->mqd_mem_obj) {
		retval = -ENOMEM;
		goto out_deallocate_doorbell;
	}

	dqm_lock(dqm);
	/*
	 * Eviction state logic: mark all queues as evicted, even ones
	 * not currently active. Restoring inactive queues later only
	 * updates the is_evicted flag but is a no-op otherwise.
	 */
	q->properties.is_evicted = !!qpd->evicted;

	mqd_mgr->init_mqd(mqd_mgr, &q->mqd, q->mqd_mem_obj,
				&q->gart_mqd_addr, &q->properties, q->process->pasid);

	list_add(&q->list, &qpd->queues_list);
	qpd->queue_count++;

	if (q->properties.is_active) {
		increment_queue_count(dqm, qpd, q);

		retval = execute_queues_cpsch(dqm,
				KCD_UNMAP_QUEUES_FILTER_DYNAMIC_QUEUES, 0);
		if (retval)
			goto cleanup_queue;
	}

	/*
	 * Unconditionally increment this counter, regardless of the queue's
	 * type or whether the queue is active.
	 */
	dqm->total_queue_count++;

	pr_debug("Total of %d queues are accountable so far\n",
			dqm->total_queue_count);

	dqm_unlock(dqm);
	return retval;

cleanup_queue:
	qpd->queue_count--;
	list_del(&q->list);
	if (q->properties.is_active)
		decrement_queue_count(dqm, qpd, q);
	mqd_mgr->free_mqd(mqd_mgr, q->mqd, q->mqd_mem_obj);
	dqm_unlock(dqm);
out_deallocate_doorbell:
	if (dqm->dev->kcd->has_doorbells)
		deallocate_doorbell(qpd, q);
out_deallocate_sdma_queue:
	if (q->properties.type == KCD_QUEUE_TYPE_SDMA) {
		dqm_lock(dqm);
		deallocate_sdma_queue(dqm, q);
		dqm_unlock(dqm);
	}
out:
	return retval;
}

int lgkcd_fence_wait_timeout(uint64_t *fence_addr,
				uint64_t fence_value,
				unsigned int timeout_ms)
{
	unsigned long end_jiffies = msecs_to_jiffies(timeout_ms) + jiffies;

	while (*fence_addr != fence_value) {
		if (time_after(jiffies, end_jiffies)) {
			pr_err("qcm fence wait loop timeout expired\n");
			/* In HWS case, this is used to halt the driver thread
			 * in order not to mess up CP states before doing
			 * scandumps for FW debugging.
			 */
			while (halt_if_hws_hang)
				schedule();

			return -ETIME;
		}
		schedule();
	}

	return 0;
}

/* dqm->lock mutex has to be locked before calling this function */
static int map_queues_cpsch(struct device_queue_manager *dqm)
{
	int retval;

	if (!dqm->sched_running)
		return 0;
	if (dqm->active_queue_count <= 0 || dqm->processes_count <= 0)
		return 0;
	if (dqm->active_runlist)
		return 0;

	retval = pm_send_direct(&dqm->packet_mgr, &dqm->queues);
	pr_debug("%s sent runlist\n", __func__);
	if (retval) {
		pr_err("failed to execute runlist\n");
		return retval;
	}
	dqm->active_runlist = true;

	return retval;
}

/* dqm->lock mutex has to be locked before calling this function */
static int unmap_queues_cpsch(struct device_queue_manager *dqm,
				enum kcd_unmap_queues_filter filter,
				uint32_t filter_param,
				bool reset)
{
	int retval = 0;
	struct mqd_manager *mqd_mgr;

	if (!dqm->sched_running)
		return 0;
	if (dqm->is_hws_hang || dqm->is_resetting)
		return -EIO;
	if (!dqm->active_runlist)
		return retval;

	retval = pm_send_unmap_queue(&dqm->packet_mgr, filter, filter_param, reset);
	if (retval)
		return retval;

	*dqm->fence_addr = KCD_FENCE_INIT;
	pm_send_query_status(&dqm->packet_mgr, dqm->fence_gpu_addr,
				KCD_FENCE_COMPLETED);
	/* should be timed out */
	retval = lgkcd_fence_wait_timeout(dqm->fence_addr, KCD_FENCE_COMPLETED,
				queue_preemption_timeout_ms);
	if (retval) {
		pr_err("The cp might be in an unrecoverable state due to an unsuccessful queues preemption\n");
		kcd_hws_hang(dqm);
		return retval;
	}

	/* In the current MEC firmware implementation, if compute queue
	 * doesn't response to the preemption request in time, HIQ will
	 * abandon the unmap request without returning any timeout error
	 * to driver. Instead, MEC firmware will log the doorbell of the
	 * unresponding compute queue to HIQ.MQD.queue_doorbell_id fields.
	 * To make sure the queue unmap was successful, driver need to
	 * check those fields
	 */
	mqd_mgr = dqm->mqd_mgrs[KCD_MQD_TYPE_HIQ];
	if (dqm->dev->kcd->has_doorbells &&
		mqd_mgr->read_doorbell_id(dqm->packet_mgr.priv_queue->queue->mqd)) {
		pr_err("HIQ MQD's queue_doorbell_id0 is not 0, Queue preemption time out\n");
		while (halt_if_hws_hang)
			schedule();
		return -ETIME;
	}

	pm_release_ib(&dqm->packet_mgr);
	dqm->active_runlist = false;

	return retval;
}

/* only for compute queue */
static int reset_queues_cpsch(struct device_queue_manager *dqm,
			uint16_t pasid)
{
	int retval;

	dqm_lock(dqm);

	retval = unmap_queues_cpsch(dqm, KCD_UNMAP_QUEUES_FILTER_BY_PASID,
			pasid, true);

	dqm_unlock(dqm);
	return retval;
}

/* dqm->lock mutex has to be locked before calling this function */
static int execute_queues_cpsch(struct device_queue_manager *dqm,
				enum kcd_unmap_queues_filter filter,
				uint32_t filter_param)
{
	int retval;

	if (dqm->is_hws_hang)
		return -EIO;
	retval = unmap_queues_cpsch(dqm, filter, filter_param, false);
	if (retval)
		return retval;

	return map_queues_cpsch(dqm);
}

static int wait_on_destroy_queue(struct device_queue_manager *dqm,
				 struct queue *q)
{
	struct kcd_process_device *pdd = kcd_get_process_device_data(q->device,
								q->process);
	int ret = 0;

	if (pdd->qpd.is_debug)
		return ret;

	q->properties.is_being_destroyed = true;

	if (pdd->process->debug_trap_enabled && q->properties.is_suspended) {
		dqm_unlock(dqm);
		mutex_unlock(&q->process->mutex);
		ret = wait_event_interruptible(dqm->destroy_wait,
						!q->properties.is_suspended);

		mutex_lock(&q->process->mutex);
		dqm_lock(dqm);
	}

	return ret;
}

static int destroy_queue_cpsch(struct device_queue_manager *dqm,
				struct qcm_process_device *qpd,
				struct queue *q)
{
	int retval;
	struct mqd_manager *mqd_mgr;
	uint64_t sdma_val = 0;
	struct kcd_process_device *pdd = qpd_to_pdd(qpd);

	/* Get the SDMA queue stats */
	if (q->properties.type == KCD_QUEUE_TYPE_SDMA) {
		retval = read_sdma_queue_counter((uint64_t __user *)q->properties.read_ptr,
							&sdma_val);
		if (retval)
			pr_err("Failed to read SDMA queue counter for queue: %d\n",
				q->properties.queue_id);
	}

	/* remove queue from list to prevent rescheduling after preemption */
	dqm_lock(dqm);

	retval = wait_on_destroy_queue(dqm, q);

	if (retval) {
		dqm_unlock(dqm);
		return retval;
	}

	if (qpd->is_debug) {
		/*
		 * error, currently we do not allow to destroy a queue
		 * of a currently debugged process
		 */
		retval = -EBUSY;
		goto failed_try_destroy_debugged_queue;

	}

	mqd_mgr = dqm->mqd_mgrs[get_mqd_type_from_queue_type(
			q->properties.type)];

	if (dqm->dev->kcd->has_doorbells)
		deallocate_doorbell(qpd, q);

	if (q->properties.type == KCD_QUEUE_TYPE_SDMA) {
		deallocate_sdma_queue(dqm, q);
		pdd->sdma_past_activity_counter += sdma_val;
	}

	list_del(&q->list);
	qpd->queue_count--;
	if (q->properties.is_active) {
		decrement_queue_count(dqm, qpd, q);
		retval = execute_queues_cpsch(dqm,
					      KCD_UNMAP_QUEUES_FILTER_DYNAMIC_QUEUES, 0);
		if (retval == -ETIME)
			qpd->reset_wavefronts = true;
	}

	/*
	 * Unconditionally decrement this counter, regardless of the queue's
	 * type
	 */
	dqm->total_queue_count--;
	pr_debug("Total of %d queues are accountable so far\n",
			dqm->total_queue_count);

	dqm_unlock(dqm);

	/*
	 * Do free_mqd and raise delete event after dqm_unlock(dqm) to avoid
	 * circular locking
	 */
	kcd_dbg_ev_raise(KCD_EC_MASK(EC_DEVICE_QUEUE_DELETE),
				qpd->pqm->process, q->device,
				-1, false, NULL, 0);

	mqd_mgr->free_mqd(mqd_mgr, q->mqd, q->mqd_mem_obj);

	return retval;

failed_try_destroy_debugged_queue:

	dqm_unlock(dqm);
	return retval;
}

static int submit_queue_cpsch(struct device_queue_manager *dqm,
				struct qcm_process_device *qpd,
				struct queue *q)
{
	int ret;

	dqm_lock(dqm);
	ret = pm_submit_queue(&dqm->packet_mgr, q->process->pasid, q->properties.queue_id);
	dqm_unlock(dqm);
	return ret;
}

/*
 * Low bits must be 0000/FFFF as required by HW, high bits must be 0 to
 * stay in user mode.
 */
#define APE1_FIXED_BITS_MASK 0xFFFF80000000FFFFULL
/* APE1 limit is inclusive and 64K aligned. */
#define APE1_LIMIT_ALIGNMENT 0xFFFF

static int process_termination_cpsch(struct device_queue_manager *dqm,
		struct qcm_process_device *qpd)
{
	int retval;
	struct queue *q;
	struct kernel_queue *kq, *kq_next;
	struct mqd_manager *mqd_mgr;
	struct device_process_node *cur, *next_dpn;
	enum kcd_unmap_queues_filter filter =
		KCD_UNMAP_QUEUES_FILTER_DYNAMIC_QUEUES;
	bool found = false;

	retval = 0;

	dqm_lock(dqm);

	/* Clean all kernel queues */
	list_for_each_entry_safe(kq, kq_next, &qpd->priv_queue_list, list) {
		list_del(&kq->list);
		decrement_queue_count(dqm, qpd, kq->queue);
		qpd->is_debug = false;
		dqm->total_queue_count--;
		filter = KCD_UNMAP_QUEUES_FILTER_ALL_QUEUES;
	}

	/* Clear all user mode queues */
	list_for_each_entry(q, &qpd->queues_list, list) {
		if (q->properties.type == KCD_QUEUE_TYPE_SDMA)
			deallocate_sdma_queue(dqm, q);

		if (q->properties.is_active) {
			decrement_queue_count(dqm, qpd, q);
		}

		dqm->total_queue_count--;
	}

	/* Unregister process */
	list_for_each_entry_safe(cur, next_dpn, &dqm->queues, list) {
		if (qpd == cur->qpd) {
			list_del(&cur->list);
			kfree(cur);
			dqm->processes_count--;
			found = true;
			break;
		}
	}

	retval = execute_queues_cpsch(dqm, filter, 0);

	if ((!dqm->is_hws_hang) && (retval || qpd->reset_wavefronts)) {
		pr_warn("Resetting wave fronts (cpsch) on dev %p\n", dqm->dev);
		qpd->reset_wavefronts = false;
	}

	/* Lastly, free mqd resources.
	 * Do free_mqd() after dqm_unlock to avoid circular locking.
	 */
	while (!list_empty(&qpd->queues_list)) {
		q = list_first_entry(&qpd->queues_list, struct queue, list);
		mqd_mgr = dqm->mqd_mgrs[get_mqd_type_from_queue_type(
				q->properties.type)];
		list_del(&q->list);
		qpd->queue_count--;
		dqm_unlock(dqm);
		mqd_mgr->free_mqd(mqd_mgr, q->mqd, q->mqd_mem_obj);
		dqm_lock(dqm);
	}
	dqm_unlock(dqm);

	/* Outside the DQM lock because under the DQM lock we can't do
	 * reclaim or take other locks that others hold while reclaiming.
	 */
	if (found)
		kcd_dec_compute_active(dqm->dev);

	return retval;
}

static int init_mqd_managers(struct device_queue_manager *dqm)
{
	int i, j;
	struct mqd_manager *mqd_mgr;

	for (i = 0; i < KCD_MQD_TYPE_MAX; i++) {
		mqd_mgr = dqm->asic_ops.mqd_manager_init(i, dqm->dev);
		if (!mqd_mgr) {
			pr_err("mqd manager [%d] initialization failed\n", i);
			goto out_free;
		}
		dqm->mqd_mgrs[i] = mqd_mgr;
	}

	return 0;

out_free:
	for (j = 0; j < i; j++) {
		kfree(dqm->mqd_mgrs[j]);
		dqm->mqd_mgrs[j] = NULL;
	}

	return -ENOMEM;
}

/* Allocate one hiq mqd (HWS) and all SDMA mqd in a continuous trunk*/
static int allocate_hiq_sdma_mqd(struct device_queue_manager *dqm)
{
	int retval;
	struct kcd_node *dev = dqm->dev;
	struct kcd_mem_obj *mem_obj = &dqm->hiq_sdma_mqd;
	uint32_t size = dqm->mqd_mgrs[KCD_MQD_TYPE_SDMA]->mqd_size *
		get_num_all_sdma_engines(dqm) *
		dev->kcd->device_info.num_sdma_queues_per_engine +
		dqm->mqd_mgrs[KCD_MQD_TYPE_HIQ]->mqd_size;

	retval = loonggpu_lgkcd_alloc_gtt_mem(dev->adev, size,
		&(mem_obj->gtt_mem), &(mem_obj->gpu_addr),
		(void *)&(mem_obj->cpu_ptr), false);

	return retval;
}

struct device_queue_manager *device_queue_manager_init(struct kcd_node *dev)
{
	struct device_queue_manager *dqm;

	pr_debug("Loading device queue manager\n");

	dqm = kzalloc(sizeof(*dqm), GFP_KERNEL);
	if (!dqm)
		return NULL;

	switch (dev->adev->family_type) {
	case CHIP_LG200:
	case CHIP_LG210:
	case CHIP_LG211:
		dqm->sched_policy = KCD_SCHED_POLICY_HWS;
		break;
	default:
		dqm->sched_policy = sched_policy;
		break;
	}

	dqm->dev = dev;
	switch (dqm->sched_policy) {
	case KCD_SCHED_POLICY_HWS:
	case KCD_SCHED_POLICY_HWS_NO_OVERSUBSCRIPTION:
		/* initialize dqm for cp scheduling */
		dqm->ops.create_queue = create_queue_cpsch;
		dqm->ops.initialize = initialize_cpsch;
		dqm->ops.start = start_cpsch;
		dqm->ops.stop = stop_cpsch;
		dqm->ops.pre_reset = pre_reset;
		dqm->ops.destroy_queue = destroy_queue_cpsch;
		dqm->ops.update_queue = update_queue;
		dqm->ops.register_process = register_process;
		dqm->ops.unregister_process = unregister_process;
		dqm->ops.uninitialize = uninitialize;
		dqm->ops.process_termination = process_termination_cpsch;
		dqm->ops.evict_process_queues = evict_process_queues_cpsch;
		dqm->ops.restore_process_queues = restore_process_queues_cpsch;
		dqm->ops.reset_queues = reset_queues_cpsch;
		dqm->ops.submit_queue = submit_queue_cpsch;
		break;
	default:
		pr_err("Invalid scheduling policy %d\n", dqm->sched_policy);
		goto out_free;
	}

	switch (dev->adev->family_type) {
	case CHIP_LG200:
	case CHIP_LG210:
	case CHIP_LG211:
		device_queue_manager_init_lg2xx(&dqm->asic_ops);
		break;

	default:
		pr_info("Unexpected ASIC family %u",
			     dev->adev->family_type);
		goto out_free;
	}

	if (init_mqd_managers(dqm))
		goto out_free;

	if (allocate_hiq_sdma_mqd(dqm)) {
		pr_err("Failed to allocate hiq sdma mqd trunk buffer\n");
		goto out_free;
	}

	if (!dqm->ops.initialize(dqm)) {
		init_waitqueue_head(&dqm->destroy_wait);
		return dqm;
	}

out_free:
	kfree(dqm);
	return NULL;
}

static void deallocate_hiq_sdma_mqd(struct kcd_node *dev,
				    struct kcd_mem_obj *mqd)
{
	WARN(!mqd, "No hiq sdma mqd trunk to free");

	loonggpu_lgkcd_free_gtt_mem(dev->adev, mqd->gtt_mem);
}

void device_queue_manager_uninit(struct device_queue_manager *dqm)
{
	dqm->ops.stop(dqm);
	dqm->ops.uninitialize(dqm);
	deallocate_hiq_sdma_mqd(dqm->dev, &dqm->hiq_sdma_mqd);
	kfree(dqm);
}

int kcd_dqm_evict_pasid(struct device_queue_manager *dqm, u32 pasid)
{
	struct kcd_process_device *pdd;
	struct kcd_process *p = kcd_lookup_process_by_pasid(pasid);
	int ret = 0;

	if (!p)
		return -EINVAL;
	WARN(debug_evictions, "Evicting pid %d", p->lead_thread->pid);
	pdd = kcd_get_process_device_data(dqm->dev, p);
	if (pdd)
		ret = dqm->ops.evict_process_queues(dqm, &pdd->qpd);
	kcd_unref_process(p);

	return ret;
}

static void kcd_process_hw_exception(struct work_struct *work)
{
	struct device_queue_manager *dqm = container_of(work,
			struct device_queue_manager, hw_exception_work);
	loonggpu_lgkcd_gpu_reset(dqm->dev->adev);
}

#define QUEUE_NOT_FOUND		-1
/* invalidate queue operation in array */
static void q_array_invalidate(uint32_t num_queues, uint32_t *queue_ids)
{
	int i;

	for (i = 0; i < num_queues; i++)
		queue_ids[i] |= KCD_DBG_QUEUE_INVALID_MASK;
}

/* find queue index in array */
static int q_array_get_index(unsigned int queue_id,
		uint32_t num_queues,
		uint32_t *queue_ids)
{
	int i;

	for (i = 0; i < num_queues; i++)
		if (queue_id == (queue_ids[i] & ~KCD_DBG_QUEUE_INVALID_MASK))
			return i;

	return QUEUE_NOT_FOUND;
}

static uint32_t *get_queue_ids(uint32_t num_queues, uint32_t *usr_queue_id_array)
{
	size_t array_size = num_queues * sizeof(uint32_t);

	if (!usr_queue_id_array)
		return NULL;

	return memdup_user(usr_queue_id_array, array_size);
}

int resume_queues(struct kcd_process *p,
		uint32_t num_queues,
		uint32_t *usr_queue_id_array)
{
	uint32_t *queue_ids = NULL;
	int total_resumed = 0;
	int i;

	if (usr_queue_id_array) {
		queue_ids = get_queue_ids(num_queues, usr_queue_id_array);

		if (IS_ERR(queue_ids))
			return PTR_ERR(queue_ids);

		/* mask all queues as invalid.  unmask per successful request */
		q_array_invalidate(num_queues, queue_ids);
	}

	for (i = 0; i < p->n_pdds; i++) {
		struct kcd_process_device *pdd = p->pdds[i];
		struct device_queue_manager *dqm = pdd->dev->dqm;
		struct qcm_process_device *qpd = &pdd->qpd;
		struct queue *q;
		int r, per_device_resumed = 0;

		dqm_lock(dqm);

		if (p->debugger_process)
			kcd_process_set_trap_debug_flag(qpd, true);

		/* unmask queues that resume or already resumed as valid */
		list_for_each_entry(q, &qpd->queues_list, list) {
			int q_idx = QUEUE_NOT_FOUND;

			if (queue_ids)
				q_idx = q_array_get_index(
						q->properties.queue_id,
						num_queues,
						queue_ids);

			if (!queue_ids || q_idx != QUEUE_NOT_FOUND) {
				int err = resume_single_queue(dqm, &pdd->qpd, q);

				if (queue_ids) {
					if (!err) {
						queue_ids[q_idx] &=
							~KCD_DBG_QUEUE_INVALID_MASK;
					} else {
						queue_ids[q_idx] |=
							KCD_DBG_QUEUE_ERROR_MASK;
						break;
					}
				}

				per_device_resumed++;
			}
		}

		if (!per_device_resumed) {
			dqm_unlock(dqm);
			continue;
		}

		r = execute_queues_cpsch(dqm,
					KCD_UNMAP_QUEUES_FILTER_DYNAMIC_QUEUES,
					0);
		if (r) {
			pr_err("Failed to resume process queues\n");
			if (queue_ids) {
				list_for_each_entry(q, &qpd->queues_list, list) {
					int q_idx = q_array_get_index(
							q->properties.queue_id,
							num_queues,
							queue_ids);

					/* mask queue as error on resume fail */
					if (q_idx != QUEUE_NOT_FOUND)
						queue_ids[q_idx] |=
							KCD_DBG_QUEUE_ERROR_MASK;
				}
			}
		} else {
			wake_up_all(&dqm->destroy_wait);
			total_resumed += per_device_resumed;
		}

		dqm_unlock(dqm);
	}

	if (queue_ids) {
		if (copy_to_user((void __user *)usr_queue_id_array, queue_ids,
				num_queues * sizeof(uint32_t)))
			pr_err("copy_to_user failed on queue resume\n");

		kfree(queue_ids);
	}

	return total_resumed;
}

int suspend_queues(struct kcd_process *p,
			uint32_t num_queues,
			uint64_t exception_clear_mask,
			uint32_t *usr_queue_id_array)
{
	uint32_t *queue_ids = get_queue_ids(num_queues, usr_queue_id_array);
	int total_suspended = 0;
	int i;

	if (IS_ERR(queue_ids))
		return PTR_ERR(queue_ids);

	/* mask all queues as invalid.  umask on successful request */
	q_array_invalidate(num_queues, queue_ids);

	for (i = 0; i < p->n_pdds; i++) {
		struct kcd_process_device *pdd = p->pdds[i];
		struct device_queue_manager *dqm = pdd->dev->dqm;
		struct qcm_process_device *qpd = &pdd->qpd;
		struct queue *q;
		int r, per_device_suspended = 0;

		mutex_lock(&p->event_mutex);
		dqm_lock(dqm);

		if (p->debugger_process)
			kcd_process_set_trap_debug_flag(qpd, false);

		/* unmask queues that suspend or already suspended */
		list_for_each_entry(q, &qpd->queues_list, list) {
			int q_idx = q_array_get_index(q->properties.queue_id,
							num_queues,
							queue_ids);

			if (q_idx != QUEUE_NOT_FOUND) {
				int err = suspend_single_queue(dqm, pdd, q);

				if (!err) {
					queue_ids[q_idx] &= ~KCD_DBG_QUEUE_INVALID_MASK;
					per_device_suspended++;
				} else if (err != -EBUSY) {
					r = err;
					queue_ids[q_idx] |= KCD_DBG_QUEUE_ERROR_MASK;
					break;
				}
			}
		}

		if (!per_device_suspended) {
			dqm_unlock(dqm);
			mutex_unlock(&p->event_mutex);
			if (total_suspended)
				loonggpu_lgkcd_debug_mem_fence(dqm->dev->adev);
			continue;
		}

		r = execute_queues_cpsch(dqm,
			KCD_UNMAP_QUEUES_FILTER_DYNAMIC_QUEUES, 0);

		if (r)
			pr_err("Failed to suspend process queues.\n");
		else
			total_suspended += per_device_suspended;

		list_for_each_entry(q, &qpd->queues_list, list) {
			int q_idx = q_array_get_index(q->properties.queue_id,
						num_queues, queue_ids);

			if (q_idx == QUEUE_NOT_FOUND)
				continue;

			/* mask queue as error on suspend fail */
			if (r)
				queue_ids[q_idx] |= KCD_DBG_QUEUE_ERROR_MASK;
			else if (exception_clear_mask)
				q->properties.exception_status &=
							~exception_clear_mask;
		}

		dqm_unlock(dqm);
		mutex_unlock(&p->event_mutex);
		/* Fixme, flush ring ...*/
	}

	if (copy_to_user((void __user *)usr_queue_id_array, queue_ids,
			num_queues * sizeof(uint32_t)))
		pr_err("copy_to_user failed on queue suspend\n");

	kfree(queue_ids);

	return total_suspended;
}

static uint32_t set_queue_type_for_user(struct queue_properties *q_props)
{
	switch (q_props->type) {
	case KCD_QUEUE_TYPE_COMPUTE:
		return KCD_IOC_QUEUE_TYPE_COMPUTE;
	case KCD_QUEUE_TYPE_SDMA:
		return KCD_IOC_QUEUE_TYPE_SDMA;
	default:
		WARN_ONCE(true, "queue type not recognized!");
		return 0xffffffff;
	};
}

void set_queue_snapshot_entry(struct queue *q,
			      uint64_t exception_clear_mask,
			      struct kcd_queue_snapshot_entry *qss_entry)
{
	qss_entry->ring_base_address = q->properties.queue_address;
	qss_entry->write_pointer_address = (uint64_t)q->properties.write_ptr;
	qss_entry->read_pointer_address = (uint64_t)q->properties.read_ptr;
	qss_entry->ctx_save_restore_address =
				q->properties.ctx_save_restore_area_address;
	qss_entry->ctx_save_restore_area_size =
				q->properties.ctx_save_restore_area_size;
	qss_entry->exception_status = q->properties.exception_status;
	qss_entry->queue_id = q->properties.queue_id;
	qss_entry->gpu_id = q->device->id;
	qss_entry->ring_size = (uint32_t)q->properties.queue_size;
	qss_entry->queue_type = set_queue_type_for_user(&q->properties);
	q->properties.exception_status &= ~exception_clear_mask;
}

int debug_lock_and_unmap(struct device_queue_manager *dqm)
{
	int r;

	dqm_lock(dqm);

	r = unmap_queues_cpsch(dqm, KCD_UNMAP_QUEUES_FILTER_ALL_QUEUES, 0, false);
	if (r)
		dqm_unlock(dqm);

	return r;
}

int debug_map_and_unlock(struct device_queue_manager *dqm)
{
	int r;

	r = map_queues_cpsch(dqm);

	dqm_unlock(dqm);

	return r;
}

int debug_refresh_runlist(struct device_queue_manager *dqm)
{
	int r = debug_lock_and_unmap(dqm);

	if (r)
		return r;

	return debug_map_and_unlock(dqm);
}

#if defined(CONFIG_DEBUG_FS)

int dqm_debugfs_hang_hws(struct device_queue_manager *dqm)
{
	int r = 0;

	dqm_lock(dqm);
	r = pm_debugfs_hang_hws(&dqm->packet_mgr);
	if (r) {
		dqm_unlock(dqm);
		return r;
	}
	dqm->active_runlist = true;
	r = execute_queues_cpsch(dqm, KCD_UNMAP_QUEUES_FILTER_ALL_QUEUES,
				0);
	dqm_unlock(dqm);

	return r;
}

#endif
