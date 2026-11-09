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
 */

#include <linux/mutex.h>
#include <linux/log2.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/mmu_context.h>
#include <linux/slab.h>
#include <linux/notifier.h>
#include <linux/compat.h>
#include <linux/mman.h>
#include <linux/file.h>
#include <linux/pm_runtime.h>
#include "loonggpu_lgkcd.h"
#include "loonggpu.h"

struct mm_struct;

#include "kcd_priv.h"
#include "kcd_device_queue_manager.h"
#include "kcd_svm.h"
#include "kcd_smi_events.h"
#include "kcd_debug.h"

/*
 * List of struct kcd_process (field kcd_process).
 * Unique/indexed by mm_struct*
 */
DEFINE_HASHTABLE(kcd_processes_table, KCD_PROCESS_TABLE_SIZE);
DEFINE_MUTEX(kcd_processes_mutex);

DEFINE_SRCU(kcd_processes_srcu);

/* For process termination handling */
static struct workqueue_struct *kcd_process_wq;

/* Ordered, single-threaded workqueue for restoring evicted
 * processes. Restoring multiple processes concurrently under memory
 * pressure can lead to processes blocking each other from validating
 * their BOs and result in a live-lock situation where processes
 * remain evicted indefinitely.
 */
static struct workqueue_struct *kcd_restore_wq;

static struct kcd_process *find_process(const struct task_struct *thread,
					bool ref);
static void kcd_process_ref_release(struct kref *ref);
static struct kcd_process *create_process(const struct task_struct *thread);

static void evict_process_worker(struct work_struct *work);
static void restore_process_worker(struct work_struct *work);

static void kcd_process_device_destroy_cwsr_dgpu(struct kcd_process_device *pdd);

struct kcd_procfs_tree {
	struct kobject *kobj;
};

static struct kcd_procfs_tree procfs;

/*
 * Structure for SDMA activity tracking
 */
struct kcd_sdma_activity_handler_workarea {
	struct work_struct sdma_activity_work;
	struct kcd_process_device *pdd;
	uint64_t sdma_activity_counter;
};

struct temp_sdma_queue_list {
	uint64_t __user *rptr;
	uint64_t sdma_val;
	unsigned int queue_id;
	struct list_head list;
};

static void kcd_sdma_activity_worker(struct work_struct *work)
{
	struct kcd_sdma_activity_handler_workarea *workarea;
	struct kcd_process_device *pdd;
	uint64_t val;
	struct mm_struct *mm;
	struct queue *q;
	struct qcm_process_device *qpd;
	struct device_queue_manager *dqm;
	int ret = 0;
	struct temp_sdma_queue_list sdma_q_list;
	struct temp_sdma_queue_list *sdma_q, *next;

	workarea = container_of(work, struct kcd_sdma_activity_handler_workarea,
				sdma_activity_work);

	pdd = workarea->pdd;
	if (!pdd)
		return;
	dqm = pdd->dev->dqm;
	qpd = &pdd->qpd;
	if (!dqm || !qpd)
		return;
	/*
	 * Total SDMA activity is current SDMA activity + past SDMA activity
	 * Past SDMA count is stored in pdd.
	 * To get the current activity counters for all active SDMA queues,
	 * we loop over all SDMA queues and get their counts from user-space.
	 *
	 * We cannot call get_user() with dqm_lock held as it can cause
	 * a circular lock dependency situation. To read the SDMA stats,
	 * we need to do the following:
	 *
	 * 1. Create a temporary list of SDMA queue nodes from the qpd->queues_list,
	 *    with dqm_lock/dqm_unlock().
	 * 2. Call get_user() for each node in temporary list without dqm_lock.
	 *    Save the SDMA count for each node and also add the count to the total
	 *    SDMA count counter.
	 *    Its possible, during this step, a few SDMA queue nodes got deleted
	 *    from the qpd->queues_list.
	 * 3. Do a second pass over qpd->queues_list to check if any nodes got deleted.
	 *    If any node got deleted, its SDMA count would be captured in the sdma
	 *    past activity counter. So subtract the SDMA counter stored in step 2
	 *    for this node from the total SDMA count.
	 */
	INIT_LIST_HEAD(&sdma_q_list.list);

	/*
	 * Create the temp list of all SDMA queues
	 */
	dqm_lock(dqm);

	list_for_each_entry(q, &qpd->queues_list, list) {
		if (q->properties.type != KCD_QUEUE_TYPE_SDMA)
			continue;

		sdma_q = kzalloc(sizeof(struct temp_sdma_queue_list), GFP_KERNEL);
		if (!sdma_q) {
			dqm_unlock(dqm);
			goto cleanup;
		}

		INIT_LIST_HEAD(&sdma_q->list);
		sdma_q->rptr = (uint64_t __user *)q->properties.read_ptr;
		sdma_q->queue_id = q->properties.queue_id;
		list_add_tail(&sdma_q->list, &sdma_q_list.list);
	}

	/*
	 * If the temp list is empty, then no SDMA queues nodes were found in
	 * qpd->queues_list. Return the past activity count as the total sdma
	 * count
	 */
	if (list_empty(&sdma_q_list.list)) {
		workarea->sdma_activity_counter = pdd->sdma_past_activity_counter;
		dqm_unlock(dqm);
		return;
	}

	dqm_unlock(dqm);

	/*
	 * Get the usage count for each SDMA queue in temp_list.
	 */
	mm = get_task_mm(pdd->process->lead_thread);
	if (!mm)
		goto cleanup;

	lg_use_mm(mm);

	list_for_each_entry(sdma_q, &sdma_q_list.list, list) {
		val = 0;
		ret = read_sdma_queue_counter(sdma_q->rptr, &val);
		if (ret) {
			pr_debug("Failed to read SDMA queue active counter for queue id: %d",
				 sdma_q->queue_id);
		} else {
			sdma_q->sdma_val = val;
			workarea->sdma_activity_counter += val;
		}
	}

	lg_unuse_mm(mm);
	mmput(mm);

	/*
	 * Do a second iteration over qpd_queues_list to check if any SDMA
	 * nodes got deleted while fetching SDMA counter.
	 */
	dqm_lock(dqm);

	workarea->sdma_activity_counter += pdd->sdma_past_activity_counter;

	list_for_each_entry(q, &qpd->queues_list, list) {
		if (list_empty(&sdma_q_list.list))
			break;

		if (q->properties.type != KCD_QUEUE_TYPE_SDMA)
			continue;

		list_for_each_entry_safe(sdma_q, next, &sdma_q_list.list, list) {
			if (((uint64_t __user *)q->properties.read_ptr == sdma_q->rptr) &&
			     (sdma_q->queue_id == q->properties.queue_id)) {
				list_del(&sdma_q->list);
				kfree(sdma_q);
				break;
			}
		}
	}

	dqm_unlock(dqm);

	/*
	 * If temp list is not empty, it implies some queues got deleted
	 * from qpd->queues_list during SDMA usage read. Subtract the SDMA
	 * count for each node from the total SDMA count.
	 */
	list_for_each_entry_safe(sdma_q, next, &sdma_q_list.list, list) {
		workarea->sdma_activity_counter -= sdma_q->sdma_val;
		list_del(&sdma_q->list);
		kfree(sdma_q);
	}

	return;

cleanup:
	list_for_each_entry_safe(sdma_q, next, &sdma_q_list.list, list) {
		list_del(&sdma_q->list);
		kfree(sdma_q);
	}
}

static ssize_t kcd_procfs_show(struct kobject *kobj, struct attribute *attr,
			       char *buffer)
{
	if (strcmp(attr->name, "pasid") == 0) {
		struct kcd_process *p = container_of(attr, struct kcd_process,
						     attr_pasid);

		return snprintf(buffer, PAGE_SIZE, "%d\n", p->pasid);
	} else if (strncmp(attr->name, "vram_", 5) == 0) {
		struct kcd_process_device *pdd = container_of(attr, struct kcd_process_device,
							      attr_vram);
		return snprintf(buffer, PAGE_SIZE, "%llu\n", READ_ONCE(pdd->vram_usage));
	} else if (strncmp(attr->name, "sdma_", 5) == 0) {
		struct kcd_process_device *pdd = container_of(attr, struct kcd_process_device,
							      attr_sdma);
		struct kcd_sdma_activity_handler_workarea sdma_activity_work_handler;

		INIT_WORK(&sdma_activity_work_handler.sdma_activity_work,
					kcd_sdma_activity_worker);

		sdma_activity_work_handler.pdd = pdd;
		sdma_activity_work_handler.sdma_activity_counter = 0;

		schedule_work(&sdma_activity_work_handler.sdma_activity_work);

		flush_work(&sdma_activity_work_handler.sdma_activity_work);

		return snprintf(buffer, PAGE_SIZE, "%llu\n",
				(sdma_activity_work_handler.sdma_activity_counter)/
				 SDMA_ACTIVITY_DIVISOR);
	} else {
		pr_err("Invalid attribute");
		return -EINVAL;
	}

	return 0;
}

static void kcd_procfs_kobj_release(struct kobject *kobj)
{
	kfree(kobj);
}

static const struct sysfs_ops kcd_procfs_ops = {
	.show = kcd_procfs_show,
};

static struct kobj_type procfs_type = {
	.release = kcd_procfs_kobj_release,
	.sysfs_ops = &kcd_procfs_ops,
};

void kcd_procfs_init(void)
{
	int ret = 0;

	procfs.kobj = kcd_alloc_struct(procfs.kobj);
	if (!procfs.kobj)
		return;

	ret = kobject_init_and_add(procfs.kobj, &procfs_type,
				   &kcd_device->kobj, "proc");
	if (ret) {
		pr_warn("Could not create procfs proc folder");
		/* If we fail to create the procfs, clean up */
		kcd_procfs_shutdown();
	}
}

void kcd_procfs_shutdown(void)
{
	if (procfs.kobj) {
		kobject_del(procfs.kobj);
		kobject_put(procfs.kobj);
		procfs.kobj = NULL;
	}
}

static ssize_t kcd_procfs_queue_show(struct kobject *kobj,
				     struct attribute *attr, char *buffer)
{
	struct queue *q = container_of(kobj, struct queue, kobj);

	if (!strcmp(attr->name, "size"))
		return snprintf(buffer, PAGE_SIZE, "%llu",
				q->properties.queue_size);
	else if (!strcmp(attr->name, "type"))
		return snprintf(buffer, PAGE_SIZE, "%d", q->properties.type);
	else if (!strcmp(attr->name, "gpuid"))
		return snprintf(buffer, PAGE_SIZE, "%u", q->device->id);
	else
		pr_err("Invalid attribute");

	return 0;
}

static ssize_t kcd_procfs_stats_show(struct kobject *kobj,
				     struct attribute *attr, char *buffer)
{
	if (strcmp(attr->name, "evicted_ms") == 0) {
		struct kcd_process_device *pdd = container_of(attr,
				struct kcd_process_device,
				attr_evict);
		uint64_t evict_jiffies;

		evict_jiffies = atomic64_read(&pdd->evict_duration_counter);

		return snprintf(buffer,
				PAGE_SIZE,
				"%llu\n",
				jiffies64_to_nsecs(evict_jiffies)/1000);

	} else {
		pr_err("Invalid attribute");
	}

	return 0;
}

static ssize_t kcd_sysfs_counters_show(struct kobject *kobj,
				       struct attribute *attr, char *buf)
{
	struct kcd_process_device *pdd;

	if (!strcmp(attr->name, "faults")) {
		pdd = container_of(attr, struct kcd_process_device,
				   attr_faults);
		return lg_sysfs_emit(buf, "%llu\n", READ_ONCE(pdd->faults));
	}
	if (!strcmp(attr->name, "page_in")) {
		pdd = container_of(attr, struct kcd_process_device,
				   attr_page_in);
		return lg_sysfs_emit(buf, "%llu\n", READ_ONCE(pdd->page_in));
	}
	if (!strcmp(attr->name, "page_out")) {
		pdd = container_of(attr, struct kcd_process_device,
				   attr_page_out);
		return lg_sysfs_emit(buf, "%llu\n", READ_ONCE(pdd->page_out));
	}
	return 0;
}

static struct attribute attr_queue_size = {
	.name = "size",
	.mode = KCD_SYSFS_FILE_MODE
};

static struct attribute attr_queue_type = {
	.name = "type",
	.mode = KCD_SYSFS_FILE_MODE
};

static struct attribute attr_queue_gpuid = {
	.name = "gpuid",
	.mode = KCD_SYSFS_FILE_MODE
};

static struct attribute *procfs_queue_attrs[] = {
	&attr_queue_size,
	&attr_queue_type,
	&attr_queue_gpuid,
	NULL
};
ATTRIBUTE_GROUPS(procfs_queue);

static const struct sysfs_ops procfs_queue_ops = {
	.show = kcd_procfs_queue_show,
};

static struct kobj_type procfs_queue_type = {
	.sysfs_ops = &procfs_queue_ops,
	.default_groups = procfs_queue_groups,
};

static const struct sysfs_ops procfs_stats_ops = {
	.show = kcd_procfs_stats_show,
};

static struct kobj_type procfs_stats_type = {
	.sysfs_ops = &procfs_stats_ops,
	.release = kcd_procfs_kobj_release,
};

static const struct sysfs_ops sysfs_counters_ops = {
	.show = kcd_sysfs_counters_show,
};

static struct kobj_type sysfs_counters_type = {
	.sysfs_ops = &sysfs_counters_ops,
	.release = kcd_procfs_kobj_release,
};

int kcd_procfs_add_queue(struct queue *q)
{
	struct kcd_process *proc;
	int ret;

	if (!q || !q->process)
		return -EINVAL;
	proc = q->process;

	/* Create proc/<pid>/queues/<queue id> folder */
	if (!proc->kobj_queues)
		return -EFAULT;
	ret = kobject_init_and_add(&q->kobj, &procfs_queue_type,
			proc->kobj_queues, "%u", q->properties.queue_id);
	if (ret < 0) {
		pr_warn("Creating proc/<pid>/queues/%u failed",
			q->properties.queue_id);
		kobject_put(&q->kobj);
		return ret;
	}

	return 0;
}

static void kcd_sysfs_create_file(struct kobject *kobj, struct attribute *attr,
				 char *name)
{
	int ret;

	if (!kobj || !attr || !name)
		return;

	attr->name = name;
	attr->mode = KCD_SYSFS_FILE_MODE;
	sysfs_attr_init(attr);

	ret = sysfs_create_file(kobj, attr);
	if (ret)
		pr_warn("Create sysfs %s/%s failed %d", kobj->name, name, ret);
}

static void kcd_procfs_add_sysfs_stats(struct kcd_process *p)
{
	int ret;
	int i;
	char stats_dir_filename[MAX_SYSFS_FILENAME_LEN];

	if (!p || !p->kobj)
		return;

	/*
	 * Create sysfs files for each GPU:
	 * - proc/<pid>/stats_<gpuid>/
	 * - proc/<pid>/stats_<gpuid>/evicted_ms
	 * - proc/<pid>/stats_<gpuid>/cu_occupancy
	 */
	for (i = 0; i < p->n_pdds; i++) {
		struct kcd_process_device *pdd = p->pdds[i];

		snprintf(stats_dir_filename, MAX_SYSFS_FILENAME_LEN,
				"stats_%u", pdd->dev->id);
		pdd->kobj_stats = kcd_alloc_struct(pdd->kobj_stats);
		if (!pdd->kobj_stats)
			return;

		ret = kobject_init_and_add(pdd->kobj_stats,
					   &procfs_stats_type,
					   p->kobj,
					   stats_dir_filename);

		if (ret) {
			pr_warn("Creating KCD proc/stats_%s folder failed",
				stats_dir_filename);
			kobject_put(pdd->kobj_stats);
			pdd->kobj_stats = NULL;
			return;
		}

		kcd_sysfs_create_file(pdd->kobj_stats, &pdd->attr_evict,
				      "evicted_ms");
	}
}

static void kcd_procfs_add_sysfs_counters(struct kcd_process *p)
{
	int ret = 0;
	int i;
	char counters_dir_filename[MAX_SYSFS_FILENAME_LEN];

	if (!p || !p->kobj)
		return;

	/*
	 * Create sysfs files for each GPU which supports SVM
	 * - proc/<pid>/counters_<gpuid>/
	 * - proc/<pid>/counters_<gpuid>/faults
	 * - proc/<pid>/counters_<gpuid>/page_in
	 * - proc/<pid>/counters_<gpuid>/page_out
	 */
	for_each_set_bit(i, p->svms.bitmap_supported, p->n_pdds) {
		struct kcd_process_device *pdd = p->pdds[i];
		struct kobject *kobj_counters;

		snprintf(counters_dir_filename, MAX_SYSFS_FILENAME_LEN,
			"counters_%u", pdd->dev->id);
		kobj_counters = kcd_alloc_struct(kobj_counters);
		if (!kobj_counters)
			return;

		ret = kobject_init_and_add(kobj_counters, &sysfs_counters_type,
					   p->kobj, counters_dir_filename);
		if (ret) {
			pr_warn("Creating KCD proc/%s folder failed",
				counters_dir_filename);
			kobject_put(kobj_counters);
			return;
		}

		pdd->kobj_counters = kobj_counters;
		kcd_sysfs_create_file(kobj_counters, &pdd->attr_faults,
				      "faults");
		kcd_sysfs_create_file(kobj_counters, &pdd->attr_page_in,
				      "page_in");
		kcd_sysfs_create_file(kobj_counters, &pdd->attr_page_out,
				      "page_out");
	}
}

static void kcd_procfs_add_sysfs_files(struct kcd_process *p)
{
	int i;

	if (!p || !p->kobj)
		return;

	/*
	 * Create sysfs files for each GPU:
	 * - proc/<pid>/vram_<gpuid>
	 * - proc/<pid>/sdma_<gpuid>
	 */
	for (i = 0; i < p->n_pdds; i++) {
		struct kcd_process_device *pdd = p->pdds[i];

		snprintf(pdd->vram_filename, MAX_SYSFS_FILENAME_LEN, "vram_%u",
			 pdd->dev->id);
		kcd_sysfs_create_file(p->kobj, &pdd->attr_vram,
				      pdd->vram_filename);

		snprintf(pdd->sdma_filename, MAX_SYSFS_FILENAME_LEN, "sdma_%u",
			 pdd->dev->id);
		kcd_sysfs_create_file(p->kobj, &pdd->attr_sdma,
					    pdd->sdma_filename);
	}
}

void kcd_procfs_del_queue(struct queue *q)
{
	if (!q)
		return;

	kobject_del(&q->kobj);
	kobject_put(&q->kobj);
}

int kcd_process_create_wq(void)
{
	if (!kcd_process_wq)
		kcd_process_wq = alloc_workqueue("kcd_process_wq",
						 WQ_UNBOUND, 0);

	if (!kcd_restore_wq)
		kcd_restore_wq = alloc_ordered_workqueue("kcd_restore_wq", 0);

	if (!kcd_process_wq || !kcd_restore_wq) {
		kcd_process_destroy_wq();
		return -ENOMEM;
	}

	return 0;
}

void kcd_process_destroy_wq(void)
{
	if (kcd_process_wq) {
		destroy_workqueue(kcd_process_wq);
		kcd_process_wq = NULL;
	}
	if (kcd_restore_wq) {
		destroy_workqueue(kcd_restore_wq);
		kcd_restore_wq = NULL;
	}
}

static void kcd_process_free_gpuvm(struct kgd_mem *mem,
			struct kcd_process_device *pdd, void **kptr)
{
	struct kcd_node *dev = pdd->dev;

	if (kptr && *kptr) {
		loonggpu_lgkcd_gpuvm_unmap_gtt_bo_from_kernel(mem);
		*kptr = NULL;
	}

	loonggpu_lgkcd_gpuvm_unmap_memory_from_gpu(dev->adev, mem, pdd->drm_priv);
	loonggpu_lgkcd_gpuvm_free_memory_of_gpu(dev->adev, mem, pdd->drm_priv,
					       NULL);
}

/* kcd_process_alloc_gpuvm - Allocate GPU VM for the KCD process
 *	This function should be only called right after the process
 *	is created and when kcd_processes_mutex is still being held
 *	to avoid concurrency. Because of that exclusiveness, we do
 *	not need to take p->mutex.
 */
static int kcd_process_alloc_gpuvm(struct kcd_process_device *pdd,
				   uint64_t gpu_va, uint32_t size,
				   uint32_t flags, struct kgd_mem **mem, void **kptr)
{
	struct kcd_node *kdev = pdd->dev;
	int err;

	err = loonggpu_lgkcd_gpuvm_alloc_memory_of_gpu(kdev->adev, gpu_va, size,
						 pdd->drm_priv, mem, NULL,
						 flags);
	if (err)
		goto err_alloc_mem;

	err = loonggpu_lgkcd_gpuvm_map_memory_to_gpu(kdev->adev, *mem,
			pdd->drm_priv);
	if (err)
		goto err_map_mem;

	err = loonggpu_lgkcd_gpuvm_sync_memory(kdev->adev, *mem, true);
	if (err) {
		pr_debug("Sync memory failed, wait interrupted by user signal\n");
		goto sync_memory_failed;
	}

	if (kptr) {
		err = loonggpu_lgkcd_gpuvm_map_gtt_bo_to_kernel(
				(struct kgd_mem *)*mem, kptr, NULL);
		if (err) {
			pr_debug("Map GTT BO to kernel failed\n");
			goto sync_memory_failed;
		}
	}

	return err;

sync_memory_failed:
	loonggpu_lgkcd_gpuvm_unmap_memory_from_gpu(kdev->adev, *mem, pdd->drm_priv);

err_map_mem:
	loonggpu_lgkcd_gpuvm_free_memory_of_gpu(kdev->adev, *mem, pdd->drm_priv,
					       NULL);
err_alloc_mem:
	*mem = NULL;
	*kptr = NULL;
	return err;
}

struct kcd_process *kcd_create_process(struct task_struct *thread)
{
	struct kcd_process *process;
	int ret;

	if (!(thread->mm && mmget_not_zero(thread->mm)))
		return ERR_PTR(-EINVAL);

	/* Only the pthreads threading model is supported. */
	if (thread->group_leader->mm != thread->mm) {
		mmput(thread->mm);
		return ERR_PTR(-EINVAL);
	}

	/*
	 * take kcd processes mutex before starting of process creation
	 * so there won't be a case where two threads of the same process
	 * create two kcd_process structures
	 */
	mutex_lock(&kcd_processes_mutex);

	if (kcd_is_locked()) {
		mutex_unlock(&kcd_processes_mutex);
		pr_debug("KCD is locked! Cannot create process");
		return ERR_PTR(-EINVAL);
	}

	/* A prior open of /dev/kcd could have already created the process. */
	process = find_process(thread, false);
	if (process) {
		pr_debug("Process already found\n");
	} else {
		process = create_process(thread);
		if (IS_ERR(process))
			goto out;

		if (!procfs.kobj)
			goto out;

		process->kobj = kcd_alloc_struct(process->kobj);
		if (!process->kobj) {
			pr_warn("Creating procfs kobject failed");
			goto out;
		}
		ret = kobject_init_and_add(process->kobj, &procfs_type,
					   procfs.kobj, "%d",
					   (int)process->lead_thread->pid);
		if (ret) {
			pr_warn("Creating procfs pid directory failed");
			kobject_put(process->kobj);
			goto out;
		}

		kcd_sysfs_create_file(process->kobj, &process->attr_pasid,
				      "pasid");

		process->kobj_queues = kobject_create_and_add("queues",
							process->kobj);
		if (!process->kobj_queues)
			pr_warn("Creating KCD proc/queues folder failed");

		kcd_procfs_add_sysfs_stats(process);
		kcd_procfs_add_sysfs_files(process);
		kcd_procfs_add_sysfs_counters(process);

		init_waitqueue_head(&process->wait_irq_drain);
	}
out:
	if (!IS_ERR(process))
		kref_get(&process->ref);
	mutex_unlock(&kcd_processes_mutex);
	mmput(thread->mm);

	return process;
}

struct kcd_process *kcd_get_process(const struct task_struct *thread)
{
	struct kcd_process *process;

	if (!thread->mm)
		return ERR_PTR(-EINVAL);

	/* Only the pthreads threading model is supported. */
	if (thread->group_leader->mm != thread->mm)
		return ERR_PTR(-EINVAL);

	process = find_process(thread, false);
	if (!process)
		return ERR_PTR(-EINVAL);

	return process;
}

static struct kcd_process *find_process_by_mm(const struct mm_struct *mm)
{
	struct kcd_process *process;

	hash_for_each_possible_rcu(kcd_processes_table, process,
					kcd_processes, (uintptr_t)mm)
		if (process->mm == mm)
			return process;

	return NULL;
}

static struct kcd_process *find_process(const struct task_struct *thread,
					bool ref)
{
	struct kcd_process *p;
	int idx;

	idx = srcu_read_lock(&kcd_processes_srcu);
	p = find_process_by_mm(thread->mm);
	if (p && ref)
		kref_get(&p->ref);
	srcu_read_unlock(&kcd_processes_srcu, idx);

	return p;
}

void kcd_unref_process(struct kcd_process *p)
{
	kref_put(&p->ref, kcd_process_ref_release);
}

/* This increments the process->ref counter. */
struct kcd_process *kcd_lookup_process_by_pid(struct pid *pid)
{
	struct task_struct *task = NULL;
	struct kcd_process *p    = NULL;

	if (!pid) {
		task = current;
		get_task_struct(task);
	} else {
		task = get_pid_task(pid, PIDTYPE_PID);
	}

	if (task) {
		p = find_process(task, true);
		put_task_struct(task);
	}

	return p;
}

static void kcd_process_device_free_bos(struct kcd_process_device *pdd)
{
	struct kcd_process *p = pdd->process;
	struct kcd_bo *buf_obj;
	int id;
	int i;

	/*
	 * Remove all handles from idr and release appropriate
	 * local memory object
	 */
	idr_for_each_entry(&pdd->alloc_idr, buf_obj, id) {

		for (i = 0; i < p->n_pdds; i++) {
			struct kcd_process_device *peer_pdd = p->pdds[i];

			if (!peer_pdd->drm_priv)
				continue;
			loonggpu_lgkcd_gpuvm_unmap_memory_from_gpu(
				peer_pdd->dev->adev, buf_obj->mem, peer_pdd->drm_priv);
		}

		loonggpu_lgkcd_gpuvm_free_memory_of_gpu(pdd->dev->adev, buf_obj->mem,
						       pdd->drm_priv, NULL);
		kcd_process_device_remove_obj_handle(pdd, id);
	}
}

/*
 * Just kunmap and unpin signal BO here. It will be freed in
 * kcd_process_free_outstanding_kcd_bos()
 */
static void kcd_process_kunmap_signal_bo(struct kcd_process *p)
{
	struct kcd_process_device *pdd;
	struct kcd_node *kdev;
	void *mem;

	kdev = kcd_device_by_id(GET_GPU_ID(p->signal_handle));
	if (!kdev)
		return;

	mutex_lock(&p->mutex);

	pdd = kcd_get_process_device_data(kdev, p);
	if (!pdd)
		goto out;

	mem = kcd_process_device_translate_handle(
		pdd, GET_IDR_HANDLE(p->signal_handle));
	if (!mem)
		goto out;

	loonggpu_lgkcd_gpuvm_unmap_gtt_bo_from_kernel(mem);

out:
	mutex_unlock(&p->mutex);
}

static void kcd_process_free_outstanding_kcd_bos(struct kcd_process *p)
{
	int i;

	for (i = 0; i < p->n_pdds; i++)
		kcd_process_device_free_bos(p->pdds[i]);
}

static void kcd_process_destroy_pdds(struct kcd_process *p)
{
	int i;

	for (i = 0; i < p->n_pdds; i++) {
		struct kcd_process_device *pdd = p->pdds[i];

		pr_debug("Releasing pdd (topology id %d) for process (pasid 0x%x)\n",
				pdd->dev->id, p->pasid);

		kcd_process_device_destroy_cwsr_dgpu(pdd);

		if (pdd->drm_file) {
			loonggpu_lgkcd_gpuvm_release_process_vm(
					pdd->dev->adev, pdd->drm_priv);
			fput(pdd->drm_file);
		}

		idr_destroy(&pdd->alloc_idr);

		if (pdd->dev->kcd->has_doorbells)
			kcd_free_process_doorbells(pdd->dev->kcd, pdd);

		/*
		 * before destroying pdd, make sure to report availability
		 * for auto suspend
		 */
		if (pdd->runtime_inuse) {
			pm_runtime_mark_last_busy(adev_to_drm(pdd->dev->adev)->dev);
			pm_runtime_put_autosuspend(adev_to_drm(pdd->dev->adev)->dev);
			pdd->runtime_inuse = false;
		}

		kfree(pdd);
		p->pdds[i] = NULL;
	}
	p->n_pdds = 0;
}

static void kcd_process_remove_sysfs(struct kcd_process *p)
{
	struct kcd_process_device *pdd;
	int i;

	if (!p->kobj)
		return;

	sysfs_remove_file(p->kobj, &p->attr_pasid);
	kobject_del(p->kobj_queues);
	kobject_put(p->kobj_queues);
	p->kobj_queues = NULL;

	for (i = 0; i < p->n_pdds; i++) {
		pdd = p->pdds[i];

		sysfs_remove_file(p->kobj, &pdd->attr_vram);
		sysfs_remove_file(p->kobj, &pdd->attr_sdma);

		sysfs_remove_file(pdd->kobj_stats, &pdd->attr_evict);
		kobject_del(pdd->kobj_stats);
		kobject_put(pdd->kobj_stats);
		pdd->kobj_stats = NULL;
	}

	for_each_set_bit(i, p->svms.bitmap_supported, p->n_pdds) {
		pdd = p->pdds[i];

		sysfs_remove_file(pdd->kobj_counters, &pdd->attr_faults);
		sysfs_remove_file(pdd->kobj_counters, &pdd->attr_page_in);
		sysfs_remove_file(pdd->kobj_counters, &pdd->attr_page_out);
		kobject_del(pdd->kobj_counters);
		kobject_put(pdd->kobj_counters);
		pdd->kobj_counters = NULL;
	}

	kobject_del(p->kobj);
	kobject_put(p->kobj);
	p->kobj = NULL;
}

/* No process locking is needed in this function, because the process
 * is not findable any more. We must assume that no other thread is
 * using it any more, otherwise we couldn't safely free the process
 * structure in the end.
 */
static void kcd_process_wq_release(struct work_struct *work)
{
	struct kcd_process *p = container_of(work, struct kcd_process,
					     release_work);

	kcd_process_dequeue_from_all_devices(p);
	pqm_uninit(&p->pqm);

	/* Signal the eviction fence after user mode queues are
	 * destroyed. This allows any BOs to be freed without
	 * triggering pointless evictions or waiting for fences.
	 */
	dma_fence_signal(p->ef);

	kcd_process_remove_sysfs(p);

	kcd_process_kunmap_signal_bo(p);
	kcd_process_free_outstanding_kcd_bos(p);
	svm_range_list_fini(p);

	kcd_process_destroy_pdds(p);
	dma_fence_put(p->ef);

	kcd_event_free_process(p);

	kcd_pasid_free(p->pasid);
	mutex_destroy(&p->mutex);

	put_task_struct(p->lead_thread);

	kfree(p);
}

static void kcd_process_ref_release(struct kref *ref)
{
	struct kcd_process *p = container_of(ref, struct kcd_process, ref);

	INIT_WORK(&p->release_work, kcd_process_wq_release);
	queue_work(kcd_process_wq, &p->release_work);
}

static void kcd_process_destroy_delayed(struct rcu_head *rcu)
{
	struct kcd_process *p = container_of(rcu, struct kcd_process, rcu);
	kcd_unref_process(p);
}

static void kcd_process_notifier_release_internal(struct kcd_process *p)
{
	void *mm;

	cancel_delayed_work_sync(&p->eviction_work);
	cancel_delayed_work_sync(&p->restore_work);

	/* Indicate to other users that MM is no longer valid */
	mm = p->mm;
	p->mm = NULL;
	kcd_dbg_trap_disable(p);

	if (atomic_read(&p->debugged_process_count) > 0) {
		struct kcd_process *target;
		unsigned int temp;
		int idx = srcu_read_lock(&kcd_processes_srcu);

		hash_for_each_rcu(kcd_processes_table, temp, target, kcd_processes) {
			if (target->debugger_process && target->debugger_process == p) {
				mutex_lock_nested(&target->mutex, 1);
				kcd_dbg_trap_disable(target);
				mutex_unlock(&target->mutex);
				if (atomic_read(&p->debugged_process_count) == 0)
					break;
			}
		}

		srcu_read_unlock(&kcd_processes_srcu, idx);
	}

#if defined(LG_MMU_NOTIFIER_UNREGISTER_NO_RELEASE)
	mmu_notifier_unregister_no_release(&p->mmu_notifier, mm);
	mmu_notifier_call_srcu(&p->rcu, &kcd_process_destroy_delayed);
#else
	mmu_notifier_put(&p->mmu_notifier);
#endif
}

static void kcd_process_notifier_release(struct mmu_notifier *mn,
					struct mm_struct *mm)
{
	struct kcd_process *p;

	/*
	 * The kcd_process structure can not be free because the
	 * mmu_notifier srcu is read locked
	 */
	p = container_of(mn, struct kcd_process, mmu_notifier);
	if (WARN_ON(p->mm != mm))
		return;

	mutex_lock(&kcd_processes_mutex);
	/*
	 * Do early return if table is empty.
	 *
	 * This could potentially happen if this function is called concurrently
	 * by mmu_notifier and by kcd_cleanup_pocesses.
	 *
	 */
	if (hash_empty(kcd_processes_table)) {
		mutex_unlock(&kcd_processes_mutex);
		return;
	}
	hash_del_rcu(&p->kcd_processes);
	mutex_unlock(&kcd_processes_mutex);
	synchronize_srcu(&kcd_processes_srcu);

	kcd_process_notifier_release_internal(p);
}

static struct mmu_notifier *kcd_process_alloc_notifier(struct mm_struct *mm)
{
	int idx = srcu_read_lock(&kcd_processes_srcu);
	struct kcd_process *p = find_process_by_mm(mm);

	srcu_read_unlock(&kcd_processes_srcu, idx);
	return p ? &p->mmu_notifier : ERR_PTR(-ESRCH);
}

static void kcd_process_free_notifier(struct mmu_notifier *mn)
{
	kcd_unref_process(container_of(mn, struct kcd_process, mmu_notifier));
}

static const struct mmu_notifier_ops kcd_process_mmu_notifier_ops = {
	.release = kcd_process_notifier_release,
	.alloc_notifier = kcd_process_alloc_notifier,
	.free_notifier = kcd_process_free_notifier,
};

/*
 * This code handles the case when driver is being unloaded before all
 * mm_struct are released.  We need to safely free the kcd_process and
 * avoid race conditions with mmu_notifier that might try to free them.
 *
 */
void kcd_cleanup_processes(void)
{
	struct kcd_process *p;
	struct hlist_node *p_temp;
	unsigned int temp;
	HLIST_HEAD(cleanup_list);

	/*
	 * Move all remaining kcd_process from the process table to a
	 * temp list for processing.   Once done, callback from mmu_notifier
	 * release will not see the kcd_process in the table and do early return,
	 * avoiding double free issues.
	 */
	mutex_lock(&kcd_processes_mutex);
	hash_for_each_safe(kcd_processes_table, temp, p_temp, p, kcd_processes) {
		hash_del_rcu(&p->kcd_processes);
		synchronize_srcu(&kcd_processes_srcu);
		hlist_add_head(&p->kcd_processes, &cleanup_list);
	}
	mutex_unlock(&kcd_processes_mutex);

	hlist_for_each_entry_safe(p, p_temp, &cleanup_list, kcd_processes)
		kcd_process_notifier_release_internal(p);

	/*
	 * Ensures that all outstanding free_notifier get called, triggering
	 * the release of the kcd_process struct.
	 */
	mmu_notifier_synchronize();
}

bool kcd_process_xnack_mode(struct kcd_process *p, bool supported)
{
	int i;

	for (i = 0; i < p->n_pdds; i++) {
		struct kcd_node *dev = p->pdds[i]->dev;
		if (dev->kcd->noretry)
			return false;
	}

	return true;
}

void kcd_process_set_trap_handler(struct qcm_process_device *qpd,
				  uint64_t tba_addr,
				  uint64_t tma_addr)
{
	if (qpd->cwsr_kaddr) {
		/* KFD trap handler is bound, record as second-level TBA/TMA
		 * in first-level TMA. First-level trap will jump to second.
		 */
		uint64_t *tma =
			(uint64_t *)(qpd->cwsr_kaddr + KCD_CWSR_TMA_OFFSET);
		tma[0] = tba_addr;
		tma[1] = tma_addr;
	} else {
		/* No trap handler bound, bind as first-level TBA/TMA. */
		qpd->tba_addr = tba_addr;
		qpd->tma_addr = tma_addr;
	}
}

void kcd_process_set_trap_debug_flag(struct qcm_process_device *qpd,
				     bool enabled)
{
	if (qpd->cwsr_kaddr) {
		uint64_t *tma =
			(uint64_t *)(qpd->cwsr_kaddr + KCD_CWSR_TMA_OFFSET);
		tma[2] = enabled;
	}
}

/*
 * On return the kcd_process is fully operational and will be freed when the
 * mm is released
 */
static struct kcd_process *create_process(const struct task_struct *thread)
{
	struct kcd_process *process;
	int err = -ENOMEM;

	process = kzalloc(sizeof(*process), GFP_KERNEL);
	if (!process)
		goto err_alloc_process;

	kref_init(&process->ref);
	mutex_init(&process->mutex);
	process->mm = thread->mm;
	process->lead_thread = thread->group_leader;
	process->n_pdds = 0;
	INIT_DELAYED_WORK(&process->eviction_work, evict_process_worker);
	INIT_DELAYED_WORK(&process->restore_work, restore_process_worker);
	process->last_restore_timestamp = get_jiffies_64();
	err = kcd_event_init_process(process);
	if (err)
		goto err_event_init;
	process->is_32bit_user_mode = in_compat_syscall();
	process->debug_trap_enabled = false;
	process->debugger_process = NULL;
	process->exception_enable_mask = 0;
	atomic_set(&process->debugged_process_count, 0);
	sema_init(&process->runtime_enable_sema, 0);

	process->pasid = kcd_pasid_alloc();
	if (process->pasid == 0) {
		err = -ENOSPC;
		goto err_alloc_pasid;
	}

	err = pqm_init(&process->pqm, process);
	if (err != 0)
		goto err_process_pqm_init;

	/* init process apertures*/
	err = kcd_init_apertures(process);
	if (err != 0)
		goto err_init_apertures;

	/* Check XNACK support after PDDs are created in kfd_init_apertures */
	process->xnack_enabled = kcd_process_xnack_mode(process, false);

	err = svm_range_list_init(process);
	if (err)
		goto err_init_svm_range_list;

	/* alloc_notifier needs to find the process in the hash table */
	hash_add_rcu(kcd_processes_table, &process->kcd_processes,
			(uintptr_t)process->mm);

	/* Avoid free_notifier to start kcd_process_wq_release if
	 * mmu_notifier_get failed because of pending signal.
	 */
	kref_get(&process->ref);

	/* Must be last, have to use release destruction after this */
	process->mmu_notifier.ops = &kcd_process_mmu_notifier_ops;
	err = mmu_notifier_register(&process->mmu_notifier, process->mm);
	if (err)
		goto err_register_notifier;

	kcd_unref_process(process);
	get_task_struct(process->lead_thread);

	INIT_WORK(&process->debug_event_workarea, debug_event_write_work_handler);

	return process;

err_register_notifier:
	hash_del_rcu(&process->kcd_processes);
	svm_range_list_fini(process);
err_init_svm_range_list:
	kcd_process_free_outstanding_kcd_bos(process);
	kcd_process_destroy_pdds(process);
err_init_apertures:
	pqm_uninit(&process->pqm);
err_process_pqm_init:
	kcd_pasid_free(process->pasid);
err_alloc_pasid:
	kcd_event_free_process(process);
err_event_init:
	mutex_destroy(&process->mutex);
	kfree(process);
err_alloc_process:
	return ERR_PTR(err);
}

struct kcd_process_device *kcd_get_process_device_data(struct kcd_node *dev,
							struct kcd_process *p)
{
	int i;

	for (i = 0; i < p->n_pdds; i++)
		if (p->pdds[i]->dev == dev)
			return p->pdds[i];

	return NULL;
}

struct kcd_process_device *kcd_create_process_device_data(struct kcd_node *dev,
							struct kcd_process *p)
{
	struct kcd_process_device *pdd = NULL;

	if (WARN_ON_ONCE(p->n_pdds >= MAX_GPU_INSTANCE))
		return NULL;
	pdd = kzalloc(sizeof(*pdd), GFP_KERNEL);
	if (!pdd)
		return NULL;

	pdd->dev = dev;
	INIT_LIST_HEAD(&pdd->qpd.queues_list);
	INIT_LIST_HEAD(&pdd->qpd.priv_queue_list);
	pdd->qpd.dqm = dev->dqm;
	pdd->qpd.pqm = &p->pqm;
	pdd->qpd.evicted = 0;
	pdd->process = p;
	pdd->bound = PDD_UNBOUND;
	pdd->already_dequeued = false;
	pdd->runtime_inuse = false;
	pdd->vram_usage = 0;
	pdd->sdma_past_activity_counter = 0;
	pdd->user_gpu_id = dev->id;
	atomic64_set(&pdd->evict_duration_counter, 0);

	p->pdds[p->n_pdds++] = pdd;

	/* Init idr used for memory handle translation */
	idr_init(&pdd->alloc_idr);

	return pdd;
}

static int kcd_process_device_init_cwsr_dgpu(struct kcd_process_device *pdd)
{
	struct kcd_node *dev = pdd->dev;
	struct qcm_process_device *qpd = &pdd->qpd;
	uint32_t flags = KCD_IOC_ALLOC_MEM_FLAGS_GTT |
			KCD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE |
			KCD_IOC_ALLOC_MEM_FLAGS_WRITABLE;
	struct kgd_mem *mem;
	void *kaddr;
	int ret;

	if (!dev->kcd->cwsr_enabled || qpd->cwsr_kaddr || !qpd->cwsr_base)
		return 0;

	/* cwsr_base is only set for dGPU */
	ret = kcd_process_alloc_gpuvm(pdd, qpd->cwsr_base,
				      KCD_CWSR_TBA_TMA_SIZE, flags, &mem, &kaddr);
	if (ret)
		return ret;

	qpd->cwsr_mem = mem;
	qpd->cwsr_kaddr = kaddr;
	qpd->tba_addr = qpd->cwsr_base;

	memcpy(qpd->cwsr_kaddr, dev->kcd->cwsr_isa, dev->kcd->cwsr_isa_size);

	kcd_process_set_trap_debug_flag(&pdd->qpd,
					pdd->process->debug_trap_enabled);

	qpd->tma_addr = qpd->tba_addr + KCD_CWSR_TMA_OFFSET;
	pr_debug("set tba :0x%llx, tma:0x%llx, cwsr_kaddr:%p for pqm.\n",
		 qpd->tba_addr, qpd->tma_addr, qpd->cwsr_kaddr);

	return 0;
}

static void kcd_process_device_destroy_cwsr_dgpu(struct kcd_process_device *pdd)
{
	struct kcd_node *dev = pdd->dev;
	struct qcm_process_device *qpd = &pdd->qpd;

	if (!dev->kcd->cwsr_enabled || !qpd->cwsr_kaddr || !qpd->cwsr_base)
		return;

	kcd_process_free_gpuvm(qpd->cwsr_mem, pdd, &qpd->cwsr_kaddr);
}

/**
 * kcd_process_device_init_vm - Initialize a VM for a process-device
 *
 * @pdd: The process-device
 * @drm_file: Optional pointer to a DRM file descriptor
 *
 * If @drm_file is specified, it will be used to acquire the VM from
 * that file descriptor. If successful, the @pdd takes ownership of
 * the file descriptor.
 *
 * If @drm_file is NULL, a new VM is created.
 *
 * Returns 0 on success, -errno on failure.
 */
int kcd_process_device_init_vm(struct kcd_process_device *pdd,
			       struct file *drm_file)
{
	struct loonggpu_fpriv *drv_priv;
	struct loonggpu_vm *avm;
	struct kcd_process *p;
	struct kcd_node *dev;
	struct drm_file *filep;
	int ret;

	if (!drm_file)
		return -EINVAL;

	if (pdd->drm_priv)
		return -EBUSY;

	filep = drm_file->private_data;
	drv_priv = filep->driver_priv;
	avm = &drv_priv->vm;

	p = pdd->process;
	dev = pdd->dev;

	ret = loonggpu_lgkcd_gpuvm_acquire_process_vm(dev->adev, avm,
						     &p->kgd_process_info,
						     &p->ef);
	if (ret) {
		pr_err("Failed to create process VM object\n");
		return ret;
	}
	pdd->drm_priv = drm_file->private_data;

	ret = kcd_process_device_init_cwsr_dgpu(pdd);
	if (ret)
		goto err_init_cwsr;

	ret = loonggpu_lgkcd_gpuvm_set_vm_pasid(dev->adev, avm, p->pasid);
	if (ret)
		goto err_set_pasid;

	pdd->drm_file = drm_file;

	return 0;

err_set_pasid:
	kcd_process_device_destroy_cwsr_dgpu(pdd);
err_init_cwsr:
	pdd->drm_priv = NULL;
	loonggpu_lgkcd_gpuvm_destroy_cb(dev->adev, avm);

	return ret;
}

/*
 * Direct the IOMMU to bind the process (specifically the pasid->mm)
 * to the device.
 * Unbinding occurs when the process dies or the device is removed.
 *
 * Assumes that the process lock is held.
 */
struct kcd_process_device *kcd_bind_process_to_device(struct kcd_node *dev,
							struct kcd_process *p)
{
	struct kcd_process_device *pdd;
	int err;

	pdd = kcd_get_process_device_data(dev, p);
	if (!pdd) {
		pr_err("Process device data doesn't exist\n");
		return ERR_PTR(-ENOMEM);
	}

	if (!pdd->drm_priv)
		return ERR_PTR(-ENODEV);

	/*
	 * signal runtime-pm system to auto resume and prevent
	 * further runtime suspend once device pdd is created until
	 * pdd is destroyed.
	 */
	if (!pdd->runtime_inuse) {
		err = pm_runtime_get_sync(adev_to_drm(dev->adev)->dev);
		if (err < 0) {
			pm_runtime_put_autosuspend(adev_to_drm(dev->adev)->dev);
			return ERR_PTR(err);
		}
	}

	/*
	 * make sure that runtime_usage counter is incremented just once
	 * per pdd
	 */
	pdd->runtime_inuse = true;

	return pdd;
}

/* Create specific handle mapped to mem from process local memory idr
 * Assumes that the process lock is held.
 */
int kcd_process_device_create_obj_handle(struct kcd_process_device *pdd,
					void *mem, uint64_t start,
					uint64_t length, uint64_t cpuva,
					unsigned int mem_type,
					int preferred_id)
{
	int handle;
	struct kcd_bo *buf_obj;
	struct kcd_process *p;

	p = pdd->process;

	buf_obj = kzalloc(sizeof(*buf_obj), GFP_KERNEL);

	if (!buf_obj)
		return -ENOMEM;

	buf_obj->it.start = start;
	buf_obj->it.last = start + length - 1;
	interval_tree_insert(&buf_obj->it, &p->bo_interval_tree);

	buf_obj->mem = mem;
	buf_obj->dev = pdd->dev;
	buf_obj->cpuva = cpuva;
	buf_obj->mem_type = mem_type;

	if (preferred_id < 0)
		handle = idr_alloc(&pdd->alloc_idr, buf_obj, 0, 0, GFP_KERNEL);
	else
		handle = idr_alloc(&pdd->alloc_idr, buf_obj, preferred_id,
						preferred_id + 1, GFP_KERNEL);

	if (handle < 0)
		kfree(buf_obj);

	return handle;
}

struct kcd_bo *kcd_process_device_find_bo(struct kcd_process_device *pdd,
					int handle)
{
	if (handle < 0)
		return NULL;

	return (struct kcd_bo *)idr_find(&pdd->alloc_idr, handle);
}

/* Translate specific handle from process local memory idr
 * Assumes that the process lock is held.
 */
void *kcd_process_device_translate_handle(struct kcd_process_device *pdd,
					int handle)
{
	struct kcd_bo *buf_obj;

	buf_obj = kcd_process_device_find_bo(pdd, handle);

	return buf_obj->mem;
}

/* Remove specific handle from process local memory idr
 * Assumes that the process lock is held.
 */
void kcd_process_device_remove_obj_handle(struct kcd_process_device *pdd,
					int handle)
{
	struct kcd_bo *buf_obj;
	struct kcd_process *p;

	p = pdd->process;

	if (handle < 0)
		return;

	buf_obj = kcd_process_device_find_bo(pdd, handle);

	idr_remove(&pdd->alloc_idr, handle);

	interval_tree_remove(&buf_obj->it, &p->bo_interval_tree);

	kfree(buf_obj);
}

/* This increments the process->ref counter. */
struct kcd_process *kcd_lookup_process_by_pasid(u32 pasid)
{
	struct kcd_process *p, *ret_p = NULL;
	unsigned int temp;

	int idx = srcu_read_lock(&kcd_processes_srcu);

	hash_for_each_rcu(kcd_processes_table, temp, p, kcd_processes) {
		if (p->pasid == pasid) {
			kref_get(&p->ref);
			ret_p = p;
			break;
		}
	}

	srcu_read_unlock(&kcd_processes_srcu, idx);

	return ret_p;
}

/* This increments the process->ref counter. */
struct kcd_process *kcd_lookup_process_by_mm(const struct mm_struct *mm)
{
	struct kcd_process *p;

	int idx = srcu_read_lock(&kcd_processes_srcu);

	p = find_process_by_mm(mm);
	if (p)
		kref_get(&p->ref);

	srcu_read_unlock(&kcd_processes_srcu, idx);

	return p;
}

/* kcd_process_evict_queues - Evict all user queues of a process
 *
 * Eviction is reference-counted per process-device. This means multiple
 * evictions from different sources can be nested safely.
 */
int kcd_process_evict_queues(struct kcd_process *p, uint32_t trigger)
{
	int r = 0;
	int i;
	unsigned int n_evicted = 0;

	for (i = 0; i < p->n_pdds; i++) {
		struct kcd_process_device *pdd = p->pdds[i];

		kcd_smi_event_queue_eviction(pdd->dev, p->lead_thread->pid,
					     trigger);

		r = pdd->dev->dqm->ops.evict_process_queues(pdd->dev->dqm,
							    &pdd->qpd);
		/* evict return -EIO if HWS is hang or asic is resetting, in this case
		 * we would like to set all the queues to be in evicted state to prevent
		 * them been add back since they actually not be saved right now.
		 */
		if (r && r != -EIO) {
			pr_err("Failed to evict process queues\n");
			goto fail;
		}
		n_evicted++;
	}

	return r;

fail:
	/* To keep state consistent, roll back partial eviction by
	 * restoring queues
	 */
	for (i = 0; i < p->n_pdds; i++) {
		struct kcd_process_device *pdd = p->pdds[i];

		if (n_evicted == 0)
			break;

		kcd_smi_event_queue_restore(pdd->dev, p->lead_thread->pid);

		if (pdd->dev->dqm->ops.restore_process_queues(pdd->dev->dqm,
							      &pdd->qpd))
			pr_err("Failed to restore queues\n");

		n_evicted--;
	}

	return r;
}

/* kcd_process_restore_queues - Restore all user queues of a process */
int kcd_process_restore_queues(struct kcd_process *p)
{
	int r, ret = 0;
	int i;

	for (i = 0; i < p->n_pdds; i++) {
		struct kcd_process_device *pdd = p->pdds[i];

		kcd_smi_event_queue_restore(pdd->dev, p->lead_thread->pid);

		r = pdd->dev->dqm->ops.restore_process_queues(pdd->dev->dqm,
							      &pdd->qpd);
		if (r) {
			pr_err("Failed to restore process queues\n");
			if (!ret)
				ret = r;
		}
	}

	return ret;
}

int kcd_process_gpuidx_from_gpuid(struct kcd_process *p, uint32_t gpu_id)
{
	int i;

	for (i = 0; i < p->n_pdds; i++)
		if (p->pdds[i] && gpu_id == p->pdds[i]->user_gpu_id)
			return i;
	return -EINVAL;
}

int
kcd_process_gpuid_from_node(struct kcd_process *p, struct kcd_node *node,
			    uint32_t *gpuid, uint32_t *gpuidx)
{
	int i;

	for (i = 0; i < p->n_pdds; i++)
		if (p->pdds[i] && p->pdds[i]->dev == node) {
			*gpuid = p->pdds[i]->user_gpu_id;
			*gpuidx = i;
			return 0;
		}
	return -EINVAL;
}

static void evict_process_worker(struct work_struct *work)
{
	int ret;
	struct kcd_process *p;
	struct delayed_work *dwork;

	dwork = to_delayed_work(work);

	/* Process termination destroys this worker thread. So during the
	 * lifetime of this thread, kcd_process p will be valid
	 */
	p = container_of(dwork, struct kcd_process, eviction_work);
	WARN_ONCE(p->last_eviction_seqno != p->ef->seqno,
		  "Eviction fence mismatch\n");

	/* Narrow window of overlap between restore and evict work
	 * item is possible. Once loonggpu_lgkcd_gpuvm_restore_process_bos
	 * unreserves KCD BOs, it is possible to evicted again. But
	 * restore has few more steps of finish. So lets wait for any
	 * previous restore work to complete
	 */
	flush_delayed_work(&p->restore_work);

	pr_debug("Started evicting pasid 0x%x\n", p->pasid);
	ret = kcd_process_evict_queues(p, KCD_QUEUE_EVICTION_TRIGGER_TTM);
	if (!ret) {
		dma_fence_signal(p->ef);
		dma_fence_put(p->ef);
		p->ef = NULL;
		queue_delayed_work(kcd_restore_wq, &p->restore_work,
				msecs_to_jiffies(PROCESS_RESTORE_TIME_MS));

		pr_debug("Finished evicting pasid 0x%x\n", p->pasid);
	} else
		pr_err("Failed to evict queues of pasid 0x%x\n", p->pasid);
}

static void restore_process_worker(struct work_struct *work)
{
	struct delayed_work *dwork;
	struct kcd_process *p;
	int ret = 0;

	dwork = to_delayed_work(work);

	/* Process termination destroys this worker thread. So during the
	 * lifetime of this thread, kcd_process p will be valid
	 */
	p = container_of(dwork, struct kcd_process, restore_work);
	pr_debug("Started restoring pasid 0x%x\n", p->pasid);

	/* Setting last_restore_timestamp before successful restoration.
	 * Otherwise this would have to be set by KGD (restore_process_bos)
	 * before KCD BOs are unreserved. If not, the process can be evicted
	 * again before the timestamp is set.
	 * If restore fails, the timestamp will be set again in the next
	 * attempt. This would mean that the minimum GPU quanta would be
	 * PROCESS_ACTIVE_TIME_MS - (time to execute the following two
	 * functions)
	 */

	p->last_restore_timestamp = get_jiffies_64();
	/* VMs may not have been acquired yet during debugging. */
	if (p->kgd_process_info)
		ret = loonggpu_lgkcd_gpuvm_restore_process_bos(p->kgd_process_info,
							     &p->ef);
	if (ret) {
		pr_debug("Failed to restore BOs of pasid 0x%x, retry after %d ms\n",
			 p->pasid, PROCESS_BACK_OFF_TIME_MS);
		ret = queue_delayed_work(kcd_restore_wq, &p->restore_work,
				msecs_to_jiffies(PROCESS_BACK_OFF_TIME_MS));
		WARN(!ret, "reschedule restore work failed\n");
		return;
	}

	ret = kcd_process_restore_queues(p);
	if (!ret)
		pr_debug("Finished restoring pasid 0x%x\n", p->pasid);
	else
		pr_err("Failed to restore queues of pasid 0x%x\n", p->pasid);
}

void kcd_suspend_all_processes(void)
{
	struct kcd_process *p;
	unsigned int temp;
	int idx = srcu_read_lock(&kcd_processes_srcu);

	WARN(debug_evictions, "Evicting all processes");
	hash_for_each_rcu(kcd_processes_table, temp, p, kcd_processes) {
		cancel_delayed_work_sync(&p->eviction_work);
		flush_delayed_work(&p->restore_work);

		if (kcd_process_evict_queues(p, KCD_QUEUE_EVICTION_TRIGGER_SUSPEND))
			pr_err("Failed to suspend process 0x%x\n", p->pasid);
		dma_fence_signal(p->ef);
		dma_fence_put(p->ef);
		p->ef = NULL;
	}
	srcu_read_unlock(&kcd_processes_srcu, idx);
}

int kcd_resume_all_processes(void)
{
	struct kcd_process *p;
	unsigned int temp;
	int ret = 0, idx = srcu_read_lock(&kcd_processes_srcu);

	hash_for_each_rcu(kcd_processes_table, temp, p, kcd_processes) {
		if (!queue_delayed_work(kcd_restore_wq, &p->restore_work, 0)) {
			pr_err("Restore process %d failed during resume\n",
			       p->pasid);
			ret = -EFAULT;
		}
	}
	srcu_read_unlock(&kcd_processes_srcu, idx);
	return ret;
}

void kcd_flush_tlb(struct kcd_process_device *pdd, enum TLB_FLUSH_TYPE type)
{
	struct kcd_node *dev = pdd->dev;

	loonggpu_lgkcd_flush_gpu_tlb_pasid(
			dev->adev, pdd->process->pasid, type);
}

/* assumes caller holds process lock. */
int kcd_process_drain_interrupts(struct kcd_process_device *pdd)
{
	uint32_t irq_drain_fence[8];
	int r = 0;

	pdd->process->irq_drain_is_open = true;

	memset(irq_drain_fence, 0, sizeof(irq_drain_fence));
	irq_drain_fence[0] = (KCD_IRQ_FENCE_SOURCEID << 8) |
							KCD_IRQ_FENCE_CLIENTID;
	irq_drain_fence[3] = pdd->process->pasid;

	/* ensure stale irqs scheduled KCD interrupts and send drain fence. */
	if (loonggpu_lgkcd_send_close_event_drain_irq(pdd->dev->adev,
						     irq_drain_fence)) {
		pdd->process->irq_drain_is_open = false;
		return 0;
	}

	r = wait_event_interruptible(pdd->process->wait_irq_drain,
				     !READ_ONCE(pdd->process->irq_drain_is_open));
	if (r)
		pdd->process->irq_drain_is_open = false;

	return r;
}

void kcd_process_close_interrupt_drain(unsigned int pasid)
{
	struct kcd_process *p;

	p = kcd_lookup_process_by_pasid(pasid);

	if (!p)
		return;

	WRITE_ONCE(p->irq_drain_is_open, false);
	wake_up_all(&p->wait_irq_drain);
	kcd_unref_process(p);
}

struct send_exception_work_handler_workarea {
	struct work_struct work;
	struct kcd_process *p;
	unsigned int queue_id;
	uint64_t error_reason;
};

static void send_exception_work_handler(struct work_struct *work)
{
	struct send_exception_work_handler_workarea *workarea;
	struct kcd_process *p;
	struct queue *q;
	struct mm_struct *mm;
	uint64_t __user *err_payload_ptr;
	uint64_t cur_err;
	uint32_t ev_id;

	workarea = container_of(work,
				struct send_exception_work_handler_workarea,
				work);
	p = workarea->p;

	mm = get_task_mm(p->lead_thread);

	if (!mm)
		return;

	lg_use_mm(mm);

	q = pqm_get_user_queue(&p->pqm, workarea->queue_id);

	if (!q)
		goto out;

	get_user(cur_err, err_payload_ptr);
	cur_err |= workarea->error_reason;
	put_user(cur_err, err_payload_ptr);

	kcd_set_event(p, ev_id);

out:
	lg_unuse_mm(mm);
	mmput(mm);
}

int kcd_send_exception_to_runtime(struct kcd_process *p,
			unsigned int queue_id,
			uint64_t error_reason)
{
	struct send_exception_work_handler_workarea worker;

	INIT_WORK_ONSTACK(&worker.work, send_exception_work_handler);

	worker.p = p;
	worker.queue_id = queue_id;
	worker.error_reason = error_reason;

	schedule_work(&worker.work);
	flush_work(&worker.work);
	destroy_work_on_stack(&worker.work);

	return 0;
}

struct kcd_process_device *kcd_process_device_data_by_id(struct kcd_process *p, uint32_t gpu_id)
{
	int i;

	if (gpu_id) {
		for (i = 0; i < p->n_pdds; i++) {
			struct kcd_process_device *pdd = p->pdds[i];

			if (pdd->user_gpu_id == gpu_id)
				return pdd;
		}
	}
	return NULL;
}

int kcd_process_get_user_gpu_id(struct kcd_process *p, uint32_t actual_gpu_id)
{
	int i;

	if (!actual_gpu_id)
		return 0;

	for (i = 0; i < p->n_pdds; i++) {
		struct kcd_process_device *pdd = p->pdds[i];

		if (pdd->dev->id == actual_gpu_id)
			return pdd->user_gpu_id;
	}
	return -EINVAL;
}

#if defined(CONFIG_DEBUG_FS)

int kcd_debugfs_mqds_by_process(struct seq_file *m, void *data)
{
	struct kcd_process *p;
	unsigned int temp;
	int r = 0;

	int idx = srcu_read_lock(&kcd_processes_srcu);

	hash_for_each_rcu(kcd_processes_table, temp, p, kcd_processes) {
		seq_printf(m, "Process %d PASID 0x%x:\n",
			   p->lead_thread->tgid, p->pasid);

		mutex_lock(&p->mutex);
		r = pqm_debugfs_mqds(m, &p->pqm);
		mutex_unlock(&p->mutex);

		if (r)
			break;
	}

	srcu_read_unlock(&kcd_processes_srcu, idx);

	return r;
}

#endif
