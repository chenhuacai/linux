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

#include <linux/slab.h>
#include <linux/mutex.h>
#include "kcd_device_queue_manager.h"
#include "kcd_kernel_queue.h"
#include "kcd_priv.h"

static inline void inc_wptr(unsigned int *wptr, unsigned int increment_bytes,
				unsigned int buffer_size_bytes)
{
	unsigned int temp = *wptr + increment_bytes / sizeof(uint32_t);

	WARN((temp * sizeof(uint32_t)) > buffer_size_bytes,
	     "buffer overflow");
	*wptr = temp;
}

static void pm_calc_map_queues_size(struct packet_manager *pm,
				unsigned int *buf_size)
{
	unsigned int process_count, queue_count;
	unsigned int map_queue_size;

	process_count = pm->dqm->processes_count;
	queue_count = pm->dqm->active_queue_count;
	map_queue_size = pm->pmf->map_queues_size;

	/* calculate map-queues buffer allocation size */
	*buf_size = (process_count * pm->pmf->map_process_size +
		     queue_count * map_queue_size) / 4;

	pr_debug("Map queues buf size %d\n", *buf_size);
}

static int pm_fill_map_queues_buffer(struct packet_manager *pm,
				struct list_head *queues,
				uint32_t *rl_buffer,
				uint32_t rl_buffer_size,
				uint32_t *wptr)
{

	unsigned int rl_wptr, i;
	int retval, processes_mapped;
	struct device_process_node *cur;
	struct qcm_process_device *qpd;
	struct queue *q;
	struct kernel_queue *kq;

	rl_wptr = retval = processes_mapped = 0;
	pr_debug("Building map queues buffer: process count: %d queues count %d\n",
		pm->dqm->processes_count, pm->dqm->active_queue_count);

	/* build packet for each process */
	list_for_each_entry(cur, queues, list) {
		qpd = cur->qpd;

		/* build process info packet */
		retval = pm->pmf->map_process(pm, &rl_buffer[rl_wptr], qpd);
		if (retval)
			return retval;

		processes_mapped++;
		inc_wptr(&rl_wptr, pm->pmf->map_process_size,
				rl_buffer_size);

		/* build packet for each kernel queue of process */
		list_for_each_entry(kq, &qpd->priv_queue_list, list) {
			if (!kq->queue->properties.is_active)
				continue;

			pr_debug("static_queue, mapping kernel q %d, is debug status %d\n",
				kq->queue->queue, qpd->is_debug);

			/* build map queue packet */
			retval = pm->pmf->map_queues(pm,
						&rl_buffer[rl_wptr],
						kq->queue,
						qpd->is_debug);
			if (retval)
				return retval;

			inc_wptr(&rl_wptr,
				pm->pmf->map_queues_size,
				rl_buffer_size);
		}

		/* build packet for each user queue of process */
		list_for_each_entry(q, &qpd->queues_list, list) {
			if (!q->properties.is_active)
				continue;

			pr_debug("static_queue, mapping user queue %d, is debug status %d\n",
				q->queue, qpd->is_debug);

			/* build map queue packet */
			retval = pm->pmf->map_queues(pm,
						&rl_buffer[rl_wptr],
						q,
						qpd->is_debug);

			if (retval)
				return retval;

			inc_wptr(&rl_wptr,
				pm->pmf->map_queues_size,
				rl_buffer_size);
		}
	}

	pr_debug("Finished map process and queues to buffer\n");

	for (i = 0; i < rl_buffer_size / sizeof(uint32_t); i++)
		pr_debug("0x%2X ", rl_buffer[i]);
	pr_debug("\n");

	*wptr = rl_wptr;

	return retval;
}

int pm_init(struct packet_manager *pm, struct device_queue_manager *dqm)
{
	switch (dqm->dev->adev->family_type) {
	case CHIP_LG200:
	case CHIP_LG210:
	case CHIP_LG211:
		pm->pmf = &kcd_lg2xx_pm_funcs;
		break;
	default:
		WARN(1, "Unexpected ASIC family %u",
			  dqm->dev->adev->family_type);
		return -EINVAL;
	}

	pm->dqm = dqm;
	mutex_init(&pm->lock);
	pm->priv_queue = kernel_queue_init(dqm->dev, KCD_QUEUE_TYPE_HIQ);
	if (!pm->priv_queue) {
		mutex_destroy(&pm->lock);
		return -ENOMEM;
	}
	pm->allocated = false;

	return 0;
}

void pm_uninit(struct packet_manager *pm, bool hanging)
{
	mutex_destroy(&pm->lock);
	kernel_queue_uninit(pm->priv_queue, hanging);
	pm->priv_queue = NULL;
}

int pm_send_direct(struct packet_manager *pm, struct list_head *dqm_queues)
{
	unsigned int packet_size_4bytes, rl_wptr;
	unsigned int *rl_buffer;
	int retval;

	pm_calc_map_queues_size(pm, &packet_size_4bytes);

	mutex_lock(&pm->lock);
	retval = kq_acquire_packet_buffer(pm->priv_queue,
					packet_size_4bytes, &rl_buffer);
	if (retval) {
		mutex_unlock(&pm->lock);
		return retval;
	}

	retval = pm_fill_map_queues_buffer(pm, dqm_queues, rl_buffer,
						packet_size_4bytes * 4,
						&rl_wptr);
	if (retval) {
		kq_rollback_packet(pm->priv_queue);
		mutex_unlock(&pm->lock);
		return retval;
	}
	kq_submit_packet(pm->priv_queue);
	mutex_unlock(&pm->lock);

	return retval;
}

int pm_send_query_status(struct packet_manager *pm, uint64_t fence_address,
			uint64_t fence_value)
{
	uint32_t *buffer, size;
	int retval = 0;

	if (WARN_ON(!fence_address))
		return -EFAULT;

	size = pm->pmf->query_status_size;
	mutex_lock(&pm->lock);
	kq_acquire_packet_buffer(pm->priv_queue,
			size / sizeof(uint32_t), (unsigned int **)&buffer);
	if (!buffer) {
		pr_err("Failed to allocate buffer on kernel queue\n");
		retval = -ENOMEM;
		goto out;
	}

	retval = pm->pmf->query_status(pm, buffer, fence_address, fence_value);
	if (!retval)
		kq_submit_packet(pm->priv_queue);
	else
		kq_rollback_packet(pm->priv_queue);

out:
	mutex_unlock(&pm->lock);
	return retval;
}

int pm_send_unmap_queue(struct packet_manager *pm,
			enum kcd_unmap_queues_filter filter,
			uint32_t filter_param, bool reset)
{
	uint32_t *buffer, size;
	int retval = 0;

	size = pm->pmf->unmap_queues_size;
	mutex_lock(&pm->lock);
	kq_acquire_packet_buffer(pm->priv_queue,
			size / sizeof(uint32_t), (unsigned int **)&buffer);
	if (!buffer) {
		pr_err("Failed to allocate buffer on kernel queue\n");
		retval = -ENOMEM;
		goto out;
	}

	retval = pm->pmf->unmap_queues(pm, buffer, filter, filter_param, reset);
	if (!retval)
		kq_submit_packet(pm->priv_queue);
	else
		kq_rollback_packet(pm->priv_queue);

out:
	mutex_unlock(&pm->lock);
	return retval;
}

int pm_submit_queue(struct packet_manager *pm,
			uint32_t pasid,
			uint32_t queue_id)
{
	uint32_t *buffer, size;
	int retval = 0;

	size = pm->pmf->submit_queue_size;
	mutex_lock(&pm->lock);
	kq_acquire_packet_buffer(pm->priv_queue,
			size / sizeof(uint32_t), (unsigned int **)&buffer);
	if (!buffer) {
		pr_err("Failed to allocate buffer on kernel queue\n");
		retval = -EBUSY;
		goto out;
	}

	retval = pm->pmf->submit_queue(pm, buffer, pasid, queue_id);
	if (!retval)
		kq_submit_packet(pm->priv_queue);
	else
		kq_rollback_packet(pm->priv_queue);

out:
	mutex_unlock(&pm->lock);
	return retval;
}

void pm_release_ib(struct packet_manager *pm)
{
	mutex_lock(&pm->lock);
	if (pm->allocated) {
		kcd_gtt_sa_free(pm->dqm->dev, pm->ib_buffer_obj);
		pm->allocated = false;
	}
	mutex_unlock(&pm->lock);
}

#if defined(CONFIG_DEBUG_FS)

int pm_debugfs_hang_hws(struct packet_manager *pm)
{
	uint32_t *buffer, size;
	int r = 0;

	if (!pm->priv_queue)
		return -EAGAIN;

	size = pm->pmf->query_status_size;
	mutex_lock(&pm->lock);
	kq_acquire_packet_buffer(pm->priv_queue,
			size / sizeof(uint32_t), (unsigned int **)&buffer);
	if (!buffer) {
		pr_err("Failed to allocate buffer on kernel queue\n");
		r = -ENOMEM;
		goto out;
	}
	memset(buffer, 0x55, size);
	kq_submit_packet(pm->priv_queue);

	pr_info("Submitting %x %x %x %x %x %x %x to HIQ to hang the HWS.",
		buffer[0], buffer[1], buffer[2], buffer[3],
		buffer[4], buffer[5], buffer[6]);
out:
	mutex_unlock(&pm->lock);
	return r;
}


#endif
