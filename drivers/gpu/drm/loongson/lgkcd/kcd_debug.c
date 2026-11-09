/*
 * Copyright 2023 Advanced Micro Devices, Inc.
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

#include "kcd_device_queue_manager.h"
#include "kcd_topology.h"
#include <linux/file.h>
#include "kcd_ioctl.h"
#include "kcd_debug.h"

#define MAX_WATCH_ADDRESSES	4

int kcd_dbg_ev_query_debug_event(struct kcd_process *process,
		      unsigned int *queue_id,
		      unsigned int *gpu_id,
		      uint64_t exception_clear_mask,
		      uint64_t *event_status)
{
	struct process_queue_manager *pqm;
	struct process_queue_node *pqn;
	int i;

	if (!(process && process->debug_trap_enabled))
		return -ENODATA;

	mutex_lock(&process->event_mutex);
	*event_status = 0;
	*queue_id = 0;
	*gpu_id = 0;

	/* find and report queue events */
	pqm = &process->pqm;
	list_for_each_entry(pqn, &pqm->queues, process_queue_list) {
		uint64_t tmp = process->exception_enable_mask;

		if (!pqn->q)
			continue;

		tmp &= pqn->q->properties.exception_status;

		if (!tmp)
			continue;

		*event_status = pqn->q->properties.exception_status;
		*queue_id = pqn->q->properties.queue_id;
		*gpu_id = pqn->q->device->id;
		pqn->q->properties.exception_status &= ~exception_clear_mask;
		goto out;
	}

	/* find and report device events */
	for (i = 0; i < process->n_pdds; i++) {
		struct kcd_process_device *pdd = process->pdds[i];
		uint64_t tmp = process->exception_enable_mask
						& pdd->exception_status;

		if (!tmp)
			continue;

		*event_status = pdd->exception_status;
		*gpu_id = pdd->dev->id;
		pdd->exception_status &= ~exception_clear_mask;
		goto out;
	}

	/* report process events */
	if (process->exception_enable_mask & process->exception_status) {
		*event_status = process->exception_status;
		process->exception_status &= ~exception_clear_mask;
	}

out:
	mutex_unlock(&process->event_mutex);
	return *event_status ? 0 : -EAGAIN;
}

void debug_event_write_work_handler(struct work_struct *work)
{
	struct kcd_process *process;

	static const char write_data = '.';
	loff_t pos = 0;

	process = container_of(work,
			struct kcd_process,
			debug_event_workarea);

	kernel_write(process->dbg_ev_file, &write_data, 1, &pos);
}

/* update process/device/queue exception status, write to descriptor
 * only if exception_status is enabled.
 */
bool kcd_dbg_ev_raise(uint64_t event_mask,
			struct kcd_process *process, struct kcd_node *dev,
			unsigned int source_id, bool use_worker,
			void *exception_data, size_t exception_data_size)
{
	struct process_queue_manager *pqm;
	struct process_queue_node *pqn;
	int i;
	static const char write_data = '.';
	loff_t pos = 0;
	bool is_subscribed = true;

	if (!(process && process->debug_trap_enabled))
		return false;

	mutex_lock(&process->event_mutex);

	if (event_mask & KCD_EC_MASK_DEVICE) {
		for (i = 0; i < process->n_pdds; i++) {
			struct kcd_process_device *pdd = process->pdds[i];

			if (pdd->dev != dev)
				continue;

			pdd->exception_status |= event_mask & KCD_EC_MASK_DEVICE;

			if (event_mask & KCD_EC_MASK(EC_DEVICE_MEMORY_VIOLATION))
				pr_debug("Debugger exception data not saved\n");

			break;
		}
	} else if (event_mask & KCD_EC_MASK_PROCESS) {
		process->exception_status |= event_mask & KCD_EC_MASK_PROCESS;
	} else {
		pqm = &process->pqm;
		list_for_each_entry(pqn, &pqm->queues,
				process_queue_list) {
			int target_id;

			if (!pqn->q)
				continue;

			target_id = pqn->q->properties.queue_id;

			if (pqn->q->device != dev || target_id != source_id)
				continue;

			pqn->q->properties.exception_status |= event_mask;
			break;
		}
	}

	if (process->exception_enable_mask & event_mask) {
		if (use_worker)
			schedule_work(&process->debug_event_workarea);
		else
			kernel_write(process->dbg_ev_file,
					&write_data,
					1,
					&pos);
	} else {
		is_subscribed = false;
	}

	mutex_unlock(&process->event_mutex);

	return is_subscribed;
}

/* set pending event queue entry from ring entry  */
bool kcd_set_dbg_ev_from_interrupt(struct kcd_node *dev,
				   unsigned int pasid,
				   unsigned int queue_id,
				   uint64_t trap_mask,
				   void *exception_data,
				   size_t exception_data_size)
{
	struct kcd_process *p;
	bool signaled_to_debugger_or_runtime = false;

	p = kcd_lookup_process_by_pasid(pasid);

	if (!p)
		return false;

	if (!kcd_dbg_ev_raise(trap_mask, p, dev, queue_id, true,
			      exception_data, exception_data_size)) {
		struct process_queue_manager *pqm;
		struct process_queue_node *pqn;

		if (!!(trap_mask & KCD_EC_MASK_QUEUE) &&
		       p->runtime_info.runtime_state == DEBUG_RUNTIME_STATE_ENABLED) {
			mutex_lock(&p->mutex);

			pqm = &p->pqm;
			list_for_each_entry(pqn, &pqm->queues,
							process_queue_list) {

				if (!(pqn->q && pqn->q->device == dev &&
				      pqn->q->queue == queue_id))
					continue;

				kcd_send_exception_to_runtime(p, pqn->q->properties.queue_id,
							      trap_mask);

				signaled_to_debugger_or_runtime = true;

				break;
			}

			mutex_unlock(&p->mutex);
		} else if (trap_mask & KCD_EC_MASK(EC_DEVICE_MEMORY_VIOLATION)) {
			kcd_dqm_evict_pasid(dev->dqm, p->pasid);
			kcd_signal_vm_fault_event(dev, p->pasid, NULL,
							exception_data);

			signaled_to_debugger_or_runtime = true;
		}
	} else {
		signaled_to_debugger_or_runtime = true;
	}

	kcd_unref_process(p);

	return signaled_to_debugger_or_runtime;
}

int kcd_dbg_send_exception_to_runtime(struct kcd_process *p,
					unsigned int dev_id,
					unsigned int queue_id,
					uint64_t error_reason)
{
	if (error_reason & KCD_EC_MASK(EC_DEVICE_MEMORY_VIOLATION)) {
		struct kcd_process_device *pdd = NULL;
		struct kcd_vdd_memory_exception_data *data;
		int i;

		for (i = 0; i < p->n_pdds; i++) {
			if (p->pdds[i]->dev->id == dev_id) {
				pdd = p->pdds[i];
				break;
			}
		}

		if (!pdd)
			return -ENODEV;

		data = NULL;

		kcd_dqm_evict_pasid(pdd->dev->dqm, p->pasid);
		kcd_signal_vm_fault_event(pdd->dev, p->pasid, NULL, data);
		error_reason &= ~KCD_EC_MASK(EC_DEVICE_MEMORY_VIOLATION);
	}

	if (error_reason & (KCD_EC_MASK(EC_PROCESS_RUNTIME))) {
		/*
		 * block should only happen after the debugger receives runtime
		 * enable notice.
		 */
		up(&p->runtime_enable_sema);
		error_reason &= ~KCD_EC_MASK(EC_PROCESS_RUNTIME);
	}

	if (error_reason)
		return kcd_send_exception_to_runtime(p, queue_id, error_reason);

	return 0;
}

int kcd_dbg_trap_set_flags(struct kcd_process *target, uint32_t *flags)
{
	target->dbg_flags = *flags & KCD_DBG_TRAP_FLAG_SINGLE_MEM_OP;

	return 0;
}

/* kcd_dbg_trap_deactivate:
 *	target: target process
 *	unwind: If this is unwinding a failed kcd_dbg_trap_enable()
 *	unwind_count:
 *		If unwind == true, how far down the pdd list we need
 *				to unwind
 *		else: ignored
 */
void kcd_dbg_trap_deactivate(struct kcd_process *target, bool unwind, int unwind_count)
{
	int i;

	if (!unwind) {
		uint32_t flags = 0;
		int resume_count = resume_queues(target, 0, NULL);

		if (resume_count)
			pr_debug("Resumed %d queues\n", resume_count);

		cancel_work_sync(&target->debug_event_workarea);

		kcd_dbg_trap_set_flags(target, &flags);
	}

	for (i = 0; i < target->n_pdds; i++) {
		struct kcd_process_device *pdd = target->pdds[i];

		/* If this is an unwind, and we have unwound the required
		 * enable calls on the pdd list, we need to stop now
		 * otherwise we may mess up another debugger session.
		 */
		if (unwind && i == unwind_count)
			break;

		kcd_process_set_trap_debug_flag(&pdd->qpd, false);

		debug_refresh_runlist(pdd->dev->dqm);
	}
}

static void kcd_dbg_clean_exception_status(struct kcd_process *target)
{
	struct process_queue_manager *pqm;
	struct process_queue_node *pqn;
	int i;

	for (i = 0; i < target->n_pdds; i++) {
		struct kcd_process_device *pdd = target->pdds[i];

		kcd_process_drain_interrupts(pdd);

		pdd->exception_status = 0;
	}

	pqm = &target->pqm;
	list_for_each_entry(pqn, &pqm->queues, process_queue_list) {
		if (!pqn->q)
			continue;

		pqn->q->properties.exception_status = 0;
	}

	target->exception_status = 0;
}

int kcd_dbg_trap_disable(struct kcd_process *target)
{
	if (!target->debug_trap_enabled)
		return 0;

	/*
	 * Defer deactivation to runtime if runtime not enabled otherwise reset
	 * attached running target runtime state to enable for re-attach.
	 */
	if (target->runtime_info.runtime_state == DEBUG_RUNTIME_STATE_ENABLED)
		kcd_dbg_trap_deactivate(target, false, 0);
	else if (target->runtime_info.runtime_state != DEBUG_RUNTIME_STATE_DISABLED)
		target->runtime_info.runtime_state = DEBUG_RUNTIME_STATE_ENABLED;

	fput(target->dbg_ev_file);
	target->dbg_ev_file = NULL;

	if (target->debugger_process) {
		atomic_dec(&target->debugger_process->debugged_process_count);
		target->debugger_process = NULL;
	}

	target->debug_trap_enabled = false;
	kcd_dbg_clean_exception_status(target);
	kcd_unref_process(target);

	return 0;
}

int kcd_dbg_trap_activate(struct kcd_process *target)
{
	int i, r = 0;

	for (i = 0; i < target->n_pdds; i++) {
		struct kcd_process_device *pdd = target->pdds[i];

		/*
		 * Setting the debug flag in the trap handler requires that the TMA has been
		 * allocated, which occurs during CWSR initialization.
		 * In the event that CWSR has not been initialized at this point, setting the
		 * flag will be called again during CWSR initialization if the target process
		 * is still debug enabled.
		 */
		kcd_process_set_trap_debug_flag(&pdd->qpd, true);


		r = debug_refresh_runlist(pdd->dev->dqm);

		if (r) {
			target->runtime_info.runtime_state =
					DEBUG_RUNTIME_STATE_ENABLED_ERROR;
			goto unwind_err;
		}
	}

	return 0;

unwind_err:
	/* Enabling debug failed, we need to disable on
	 * all GPUs so the enable is all or nothing.
	 */
	kcd_dbg_trap_deactivate(target, true, i);
	return r;
}

int kcd_dbg_trap_enable(struct kcd_process *target, uint32_t fd,
			void __user *runtime_info, uint32_t *runtime_size)
{
	struct file *f;
	uint32_t copy_size;
	int i, r = 0;

	if (target->debug_trap_enabled)
		return -EALREADY;

	/* Enable pre-checks */
	for (i = 0; i < target->n_pdds; i++) {
		struct kcd_process_device *pdd = target->pdds[i];
		if (pdd->dev->adev->family_type < CHIP_LG210)
			return -ENODEV;
	}

	copy_size = min((size_t)(*runtime_size), sizeof(target->runtime_info));

	f = fget(fd);
	if (!f) {
		pr_err("Failed to get file for (%i)\n", fd);
		return -EBADF;
	}

	target->dbg_ev_file = f;

	/* defer activation to runtime if not runtime enabled */
	if (target->runtime_info.runtime_state == DEBUG_RUNTIME_STATE_ENABLED)
		kcd_dbg_trap_activate(target);

	/* We already hold the process reference but hold another one for the
	 * debug session.
	 */
	kref_get(&target->ref);
	target->debug_trap_enabled = true;

	if (target->debugger_process)
		atomic_inc(&target->debugger_process->debugged_process_count);

	if (copy_to_user(runtime_info, (void *)&target->runtime_info, copy_size)) {
		kcd_dbg_trap_deactivate(target, false, 0);
		r = -EFAULT;
	}

	*runtime_size = sizeof(target->runtime_info);

	return r;
}

int kcd_dbg_trap_query_exception_info(struct kcd_process *target,
		uint32_t source_id,
		uint32_t exception_code,
		bool clear_exception,
		void __user *info,
		uint32_t *info_size)
{
	bool found = false;
	int r = 0;
	uint32_t copy_size, actual_info_size = 0;
	uint64_t *exception_status_ptr = NULL;

	if (!target)
		return -EINVAL;

	if (!info || !info_size)
		return -EINVAL;

	mutex_lock(&target->event_mutex);

	if (KCD_DBG_EC_TYPE_IS_QUEUE(exception_code)) {
		/* Per queue exceptions */
		struct queue *queue = NULL;
		int i;

		for (i = 0; i < target->n_pdds; i++) {
			struct kcd_process_device *pdd = target->pdds[i];
			struct qcm_process_device *qpd = &pdd->qpd;

			list_for_each_entry(queue, &qpd->queues_list, list) {
				if (!found && queue->properties.queue_id == source_id) {
					found = true;
					break;
				}
			}
			if (found)
				break;
		}

		if (!found) {
			r = -EINVAL;
			goto out;
		}

		if (!(queue->properties.exception_status & KCD_EC_MASK(exception_code))) {
			r = -ENODATA;
			goto out;
		}
		exception_status_ptr = &queue->properties.exception_status;
	} else if (KCD_DBG_EC_TYPE_IS_PROCESS(exception_code)) {
		/* Per process exceptions */
		if (!(target->exception_status & KCD_EC_MASK(exception_code))) {
			r = -ENODATA;
			goto out;
		}

		if (exception_code == EC_PROCESS_RUNTIME) {
			copy_size = min((size_t)(*info_size), sizeof(target->runtime_info));

			if (copy_to_user(info, (void *)&target->runtime_info, copy_size)) {
				r = -EFAULT;
				goto out;
			}

			actual_info_size = sizeof(target->runtime_info);
		}

		exception_status_ptr = &target->exception_status;
	} else {
		pr_debug("Bad exception type [%i]\n", exception_code);
		r = -EINVAL;
		goto out;
	}

	*info_size = actual_info_size;
	if (clear_exception)
		*exception_status_ptr &= ~KCD_EC_MASK(exception_code);
out:
	mutex_unlock(&target->event_mutex);
	return r;
}

int kcd_dbg_trap_device_snapshot(struct kcd_process *target,
		uint64_t exception_clear_mask,
		void __user *user_info,
		uint32_t *number_of_device_infos,
		uint32_t *entry_size)
{
	struct kcd_dbg_device_info_entry device_info;
	uint32_t tmp_entry_size = *entry_size, tmp_num_devices;
	int i, r = 0;

	if (!(target && user_info && number_of_device_infos && entry_size))
		return -EINVAL;

	tmp_num_devices = min_t(size_t, *number_of_device_infos, target->n_pdds);
	*number_of_device_infos = target->n_pdds;
	*entry_size = min_t(size_t, *entry_size, sizeof(device_info));

	if (!tmp_num_devices)
		return 0;

	memset(&device_info, 0, sizeof(device_info));

	mutex_lock(&target->event_mutex);

	/* Run over all pdd of the process */
	for (i = 0; i < tmp_num_devices; i++) {
		struct kcd_process_device *pdd = target->pdds[i];
		struct kcd_topology_device *topo_dev = kcd_topology_device_by_id(pdd->dev->id);

		device_info.gpu_id = pdd->dev->id;
		device_info.exception_status = pdd->exception_status;
		device_info.lds_base = pdd->lds_base;
		device_info.lds_limit = pdd->lds_limit;
		device_info.scratch_base = pdd->scratch_base;
		device_info.scratch_limit = pdd->scratch_limit;
		device_info.gpuvm_base = pdd->gpuvm_base;
		device_info.gpuvm_limit = pdd->gpuvm_limit;
		device_info.location_id = topo_dev->node_props.location_id;
		device_info.vendor_id = topo_dev->node_props.vendor_id;
		device_info.device_id = topo_dev->node_props.device_id;
		device_info.revision_id = pdd->dev->adev->pdev->revision;
		device_info.subsystem_vendor_id = pdd->dev->adev->pdev->subsystem_vendor;
		device_info.subsystem_device_id = pdd->dev->adev->pdev->subsystem_device;
		device_info.fw_version = pdd->dev->kcd->fw_version;
		device_info.gfx_target_version =
			topo_dev->node_props.gfx_target_version;
		device_info.simd_count = topo_dev->node_props.simd_count;
		device_info.max_waves_per_simd =
			topo_dev->node_props.max_waves_per_simd;
		device_info.array_count = topo_dev->node_props.array_count;
		device_info.simd_arrays_per_engine =
			topo_dev->node_props.simd_arrays_per_engine;
		device_info.num_xcc = 1;
		device_info.capability = topo_dev->node_props.capability;
		device_info.debug_prop = topo_dev->node_props.debug_prop;

		if (exception_clear_mask)
			pdd->exception_status &= ~exception_clear_mask;

		if (copy_to_user(user_info, &device_info, *entry_size)) {
			r = -EFAULT;
			break;
		}

		user_info += tmp_entry_size;
	}

	mutex_unlock(&target->event_mutex);

	return r;
}

void kcd_dbg_set_enabled_debug_exception_mask(struct kcd_process *target,
					uint64_t exception_set_mask)
{
	uint64_t found_mask = 0;
	struct process_queue_manager *pqm;
	struct process_queue_node *pqn;
	static const char write_data = '.';
	loff_t pos = 0;
	int i;

	mutex_lock(&target->event_mutex);

	found_mask |= target->exception_status;

	pqm = &target->pqm;
	list_for_each_entry(pqn, &pqm->queues, process_queue_list) {
		if (!pqn->q)
			continue;

		found_mask |= pqn->q->properties.exception_status;
	}

	for (i = 0; i < target->n_pdds; i++) {
		struct kcd_process_device *pdd = target->pdds[i];

		found_mask |= pdd->exception_status;
	}

	if (exception_set_mask & found_mask)
		kernel_write(target->dbg_ev_file, &write_data, 1, &pos);

	target->exception_enable_mask = exception_set_mask;

	mutex_unlock(&target->event_mutex);
}
