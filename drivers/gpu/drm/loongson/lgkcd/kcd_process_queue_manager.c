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
#include <linux/list.h>
#include "kcd_device_queue_manager.h"
#include "kcd_priv.h"
#include "kcd_kernel_queue.h"
#include "loonggpu_lgkcd.h"

static inline struct process_queue_node *get_queue_by_qid(
			struct process_queue_manager *pqm, unsigned int qid)
{
	struct process_queue_node *pqn;

	list_for_each_entry(pqn, &pqm->queues, process_queue_list) {
		if ((pqn->q && pqn->q->properties.queue_id == qid) ||
		    (pqn->kq && pqn->kq->queue->properties.queue_id == qid))
			return pqn;
	}

	return NULL;
}

static int find_available_queue_slot(struct process_queue_manager *pqm,
					unsigned int *qid)
{
	unsigned long found;

	found = find_first_zero_bit(pqm->queue_slot_bitmap,
			KCD_MAX_NUM_OF_QUEUES_PER_PROCESS);

	pr_debug("The new slot id %lu\n", found);

	if (found >= KCD_MAX_NUM_OF_QUEUES_PER_PROCESS) {
		pr_info("Cannot open more queues for process with pasid 0x%x\n",
				pqm->process->pasid);
		return -ENOMEM;
	}

	set_bit(found, pqm->queue_slot_bitmap);
	*qid = found;

	return 0;
}

void kcd_process_dequeue_from_device(struct kcd_process_device *pdd)
{
	struct kcd_node *dev = pdd->dev;

	if (pdd->already_dequeued)
		return;

	dev->dqm->ops.process_termination(dev->dqm, &pdd->qpd);
	pdd->already_dequeued = true;
}

void kcd_process_dequeue_from_all_devices(struct kcd_process *p)
{
	int i;

	for (i = 0; i < p->n_pdds; i++)
		kcd_process_dequeue_from_device(p->pdds[i]);
}

int pqm_init(struct process_queue_manager *pqm, struct kcd_process *p)
{
	INIT_LIST_HEAD(&pqm->queues);
	pqm->queue_slot_bitmap = bitmap_zalloc(KCD_MAX_NUM_OF_QUEUES_PER_PROCESS,
					       GFP_KERNEL);
	if (!pqm->queue_slot_bitmap)
		return -ENOMEM;
	pqm->process = p;

	return 0;
}

void pqm_uninit(struct process_queue_manager *pqm)
{
	struct process_queue_node *pqn, *next;

	list_for_each_entry_safe(pqn, next, &pqm->queues, process_queue_list) {
		kcd_procfs_del_queue(pqn->q);
		uninit_queue(pqn->q);
		list_del(&pqn->process_queue_list);
		kfree(pqn);
	}

	bitmap_free(pqm->queue_slot_bitmap);
	pqm->queue_slot_bitmap = NULL;
}

static int init_user_queue(struct process_queue_manager *pqm,
				struct kcd_node *dev, struct queue **q,
				struct queue_properties *q_properties,
				struct file *f,
				unsigned int qid)
{
	int retval;

	/* Doorbell initialized in user space*/
	q_properties->doorbell_ptr = NULL;
	q_properties->exception_status = KCD_EC_MASK(EC_QUEUE_NEW);

	/* let DQM handle it*/
	q_properties->vmid = 0;
	q_properties->queue_id = qid;

	retval = init_queue(q, q_properties);
	if (retval != 0)
		return retval;

	(*q)->device = dev;
	(*q)->process = pqm->process;

	pr_debug("PQM After init queue");
	return 0;
}

int pqm_create_queue(struct process_queue_manager *pqm,
			    struct kcd_node *dev,
			    struct file *f,
			    struct queue_properties *properties,
			    unsigned int *qid,
			    const void *restore_mqd,
			    uint32_t *p_doorbell_offset_in_process)
{
	int retval;
	struct kcd_process_device *pdd;
	struct queue *q;
	struct process_queue_node *pqn;
	struct kernel_queue *kq;
	enum kcd_queue_type type = properties->type;
	unsigned int max_queues = get_num_sdma_queues(dev->dqm) +
				get_pipes_per_mec(dev->dqm) * get_queues_per_pipe(dev->dqm);

	q = NULL;
	kq = NULL;

	pdd = kcd_get_process_device_data(dev, pqm->process);
	if (!pdd) {
		pr_err("Process device data doesn't exist\n");
		return -1;
	}

	/*
	 * for debug process, verify that it is within the static queues limit
	 * currently limit is set to half of the total avail HQD slots
	 * If we are just about to create DIQ, the is_debug flag is not set yet
	 * Hence we also check the type as well
	 */
	if (pdd->qpd.is_debug)
		max_queues = dev->kcd->device_info.max_no_of_hqd/2;

	if (pdd->qpd.queue_count >= max_queues)
		return -ENOSPC;

	retval = find_available_queue_slot(pqm, qid);

	if (retval != 0)
		return retval;

	if (list_empty(&pdd->qpd.queues_list) &&
	    list_empty(&pdd->qpd.priv_queue_list))
		dev->dqm->ops.register_process(dev->dqm, &pdd->qpd);

	pqn = kzalloc(sizeof(*pqn), GFP_KERNEL);
	if (!pqn) {
		retval = -ENOMEM;
		goto err_allocate_pqn;
	}

	switch (type) {
	case KCD_QUEUE_TYPE_SDMA:
		/* SDMA queues are always allocated statically no matter
		 * which scheduler mode is used. We also do not need to
		 * check whether a SDMA queue can be allocated here, because
		 * allocate_sdma_queue() in create_queue() has the
		 * corresponding check logic.
		 */
		retval = init_user_queue(pqm, dev, &q, properties, f, *qid);
		if (retval != 0)
			goto err_create_queue;
		pqn->q = q;
		pqn->kq = NULL;
		retval = dev->dqm->ops.create_queue(dev->dqm, q, &pdd->qpd,
						    restore_mqd);
		print_queue(q);
		break;

	case KCD_QUEUE_TYPE_COMPUTE:
		/* check if there is over subscription */
		if ((dev->dqm->sched_policy ==
		     KCD_SCHED_POLICY_HWS_NO_OVERSUBSCRIPTION) &&
		((dev->dqm->processes_count >= dev->vm_info.vmid_num_kcd) ||
		(dev->dqm->active_queue_count >= get_cp_queues_num(dev->dqm)))) {
			pr_debug("Over-subscription is not allowed when lgkcd.sched_policy == 1\n");
			retval = -EPERM;
			goto err_create_queue;
		}

		retval = init_user_queue(pqm, dev, &q, properties, f, *qid);
		if (retval != 0)
			goto err_create_queue;
		pqn->q = q;
		pqn->kq = NULL;
		retval = dev->dqm->ops.create_queue(dev->dqm, q, &pdd->qpd,
						    restore_mqd);
		print_queue(q);
		break;
	default:
		WARN(1, "Invalid queue type %d", type);
		retval = -EINVAL;
	}

	if (retval != 0) {
		pr_err("Pasid 0x%x DQM create queue type %d failed. ret %d\n",
			pqm->process->pasid, type, retval);
		goto err_create_queue;
	}

	if (dev->kcd->has_doorbells && q && p_doorbell_offset_in_process) {
		/* Return the doorbell offset within the doorbell page
		 * to the caller so it can be passed up to user mode
		 * (in bytes).
		 * relative doorbell index = Absolute doorbell index -
		 * absolute index of first doorbell in the page.
		 */
		uint32_t first_db_index = loonggpu_doorbell_index_on_bar(pdd->dev->adev,
								       pdd->qpd.proc_doorbells,
								       0);

		*p_doorbell_offset_in_process = (q->properties.doorbell_off
						- first_db_index) * sizeof(uint64_t);
	}

	pr_debug("PQM After DQM create queue\n");

	list_add(&pqn->process_queue_list, &pqm->queues);

	if (q) {
		pr_debug("PQM done creating queue\n");
		kcd_procfs_add_queue(q);
		print_queue_properties(&q->properties);
	}

	return retval;

err_create_queue:
	uninit_queue(q);
	if (kq)
		kernel_queue_uninit(kq, false);
	kfree(pqn);
err_allocate_pqn:
	/* check if queues list is empty unregister process from device */
	clear_bit(*qid, pqm->queue_slot_bitmap);
	if (list_empty(&pdd->qpd.queues_list) &&
	    list_empty(&pdd->qpd.priv_queue_list))
		dev->dqm->ops.unregister_process(dev->dqm, &pdd->qpd);
	return retval;
}

int pqm_destroy_queue(struct process_queue_manager *pqm, unsigned int qid)
{
	struct process_queue_node *pqn;
	struct kcd_process_device *pdd;
	struct device_queue_manager *dqm;
	struct kcd_node *dev;
	int retval;

	dqm = NULL;

	retval = 0;

	pqn = get_queue_by_qid(pqm, qid);
	if (!pqn) {
		pr_err("Queue id does not match any known queue\n");
		return -EINVAL;
	}

	dev = NULL;
	if (pqn->kq)
		dev = pqn->kq->dev;
	if (pqn->q)
		dev = pqn->q->device;
	if (WARN_ON(!dev))
		return -ENODEV;

	pdd = kcd_get_process_device_data(dev, pqm->process);
	if (!pdd) {
		pr_err("Process device data doesn't exist\n");
		return -1;
	}

	if (pqn->kq) {
		/* destroy kernel queue (DIQ) */
		dqm = pqn->kq->dev->dqm;
		dqm->ops.destroy_kernel_queue(dqm, pqn->kq, &pdd->qpd);
		kernel_queue_uninit(pqn->kq, false);
	}

	if (pqn->q) {
		kcd_procfs_del_queue(pqn->q);
		dqm = pqn->q->device->dqm;
		retval = dqm->ops.destroy_queue(dqm, &pdd->qpd, pqn->q);
		if (retval) {
			pr_err("Pasid 0x%x destroy queue %d failed, ret %d\n",
				pqm->process->pasid,
				pqn->q->properties.queue_id, retval);
			if (retval != -ETIME)
				goto err_destroy_queue;
		}

		uninit_queue(pqn->q);
	}

	list_del(&pqn->process_queue_list);
	kfree(pqn);
	clear_bit(qid, pqm->queue_slot_bitmap);

	if (list_empty(&pdd->qpd.queues_list) &&
	    list_empty(&pdd->qpd.priv_queue_list))
		dqm->ops.unregister_process(dqm, &pdd->qpd);

err_destroy_queue:
	return retval;
}

int pqm_update_queue_properties(struct process_queue_manager *pqm,
				unsigned int qid, struct queue_properties *p)
{
	int retval;
	struct process_queue_node *pqn;

	pqn = get_queue_by_qid(pqm, qid);
	if (!pqn) {
		pr_debug("No queue %d exists for update operation\n", qid);
		return -EFAULT;
	}

	pqn->q->properties.queue_address = p->queue_address;
	pqn->q->properties.queue_size = p->queue_size;
	pqn->q->properties.queue_percent = p->queue_percent;
	pqn->q->properties.priority = p->priority;

	retval = pqn->q->device->dqm->ops.update_queue(pqn->q->device->dqm,
							pqn->q);
	if (retval != 0)
		return retval;

	return 0;
}

int pqm_update_mqd(struct process_queue_manager *pqm,
				unsigned int qid)
{
	int retval;
	struct process_queue_node *pqn;

	pqn = get_queue_by_qid(pqm, qid);
	if (!pqn) {
		pr_debug("No queue %d exists for update operation\n", qid);
		return -EFAULT;
	}

	retval = pqn->q->device->dqm->ops.update_queue(pqn->q->device->dqm,
							pqn->q);
	if (retval != 0)
		return retval;

	return 0;
}

struct kernel_queue *pqm_get_kernel_queue(
					struct process_queue_manager *pqm,
					unsigned int qid)
{
	struct process_queue_node *pqn;

	pqn = get_queue_by_qid(pqm, qid);
	if (pqn && pqn->kq)
		return pqn->kq;

	return NULL;
}

struct queue *pqm_get_user_queue(struct process_queue_manager *pqm,
					unsigned int qid)
{
	struct process_queue_node *pqn;

	pqn = get_queue_by_qid(pqm, qid);
	return pqn ? pqn->q : NULL;
}

int pqm_get_queue_snapshot(struct process_queue_manager *pqm,
			   uint64_t exception_clear_mask,
			   void __user *buf,
			   int *num_qss_entries,
			   uint32_t *entry_size)
{
	struct process_queue_node *pqn;
	struct kcd_queue_snapshot_entry src;
	uint32_t tmp_entry_size = *entry_size, tmp_qss_entries = *num_qss_entries;
	int r = 0;

	*num_qss_entries = 0;
	if (!(*entry_size))
		return -EINVAL;

	*entry_size = min_t(size_t, *entry_size, sizeof(struct kcd_queue_snapshot_entry));
	mutex_lock(&pqm->process->event_mutex);

	memset(&src, 0, sizeof(src));

	list_for_each_entry(pqn, &pqm->queues, process_queue_list) {
		if (!pqn->q)
			continue;

		if (*num_qss_entries < tmp_qss_entries) {
			set_queue_snapshot_entry(pqn->q, exception_clear_mask, &src);

			if (copy_to_user(buf, &src, *entry_size)) {
				r = -EFAULT;
				break;
			}
			buf += tmp_entry_size;
		}
		*num_qss_entries += 1;
	}

	mutex_unlock(&pqm->process->event_mutex);
	return r;
}

static int get_queue_data_sizes(struct kcd_process_device *pdd,
				struct queue *q,
				uint32_t *mqd_size)
{
	return 0;
}

int kcd_process_get_queue_info(struct kcd_process *p,
			       uint32_t *num_queues,
			       uint64_t *priv_data_sizes)
{
	uint32_t extra_data_sizes = 0;
	struct queue *q;
	int i;
	int ret;

	*num_queues = 0;

	/* Run over all PDDs of the process */
	for (i = 0; i < p->n_pdds; i++) {
		struct kcd_process_device *pdd = p->pdds[i];

		list_for_each_entry(q, &pdd->qpd.queues_list, list) {
			if (q->properties.type == KCD_QUEUE_TYPE_COMPUTE ||
				q->properties.type == KCD_QUEUE_TYPE_SDMA) {
				uint32_t mqd_size;

				*num_queues = *num_queues + 1;

				ret = get_queue_data_sizes(pdd, q, &mqd_size);
				if (ret)
					return ret;

				extra_data_sizes += mqd_size;
			} else {
				pr_err("Unsupported queue type (%d)\n", q->properties.type);
				return -EOPNOTSUPP;
			}
		}
	}
	*priv_data_sizes = extra_data_sizes;

	return 0;
}

#if defined(CONFIG_DEBUG_FS)

int pqm_debugfs_mqds(struct seq_file *m, void *data)
{
	struct process_queue_manager *pqm = data;
	struct process_queue_node *pqn;
	struct queue *q;
	enum KCD_MQD_TYPE mqd_type;
	struct mqd_manager *mqd_mgr;
	int r = 0;
	uint64_t size = 0;

	list_for_each_entry(pqn, &pqm->queues, process_queue_list) {
		if (pqn->q) {
			q = pqn->q;
			switch (q->properties.type) {
			case KCD_QUEUE_TYPE_SDMA:
				seq_printf(m, "  SDMA queue on device %x\n",
					   q->device->id);
				mqd_type = KCD_MQD_TYPE_SDMA;
				break;
			case KCD_QUEUE_TYPE_COMPUTE:
				seq_printf(m, "  Compute queue on device %x\n",
					   q->device->id);
				mqd_type = KCD_MQD_TYPE_CP;
				break;
			default:
				seq_printf(m,
				"  Bad user queue type %d on device %x\n",
					   q->properties.type, q->device->id);
				continue;
			}
			mqd_mgr = q->device->dqm->mqd_mgrs[mqd_type];
			size = mqd_mgr->mqd_stride(mqd_mgr,
							&q->properties);
		} else if (pqn->kq) {
			q = pqn->kq->queue;
			mqd_mgr = pqn->kq->mqd_mgr;
			switch (q->properties.type) {
			default:
				seq_printf(m,
				"  Bad kernel queue type %d on device %x\n",
					   q->properties.type,
					   pqn->kq->dev->id);
				continue;
			}
		} else {
			seq_printf(m,
		"  Weird: Queue node with neither kernel nor user queue\n");
			continue;
		}
	}

	return r;
}

#endif
