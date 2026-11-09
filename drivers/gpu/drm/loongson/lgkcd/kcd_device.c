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

#include <linux/bsearch.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include "kcd_priv.h"
#include "kcd_device_queue_manager.h"
#include "kcd_pm_headers_lg2xx.h"
#include "loonggpu_lgkcd.h"
#include "kcd_smi_events.h"
#include "kcd_svm.h"
#include "kcd_migrate.h"
#include "loonggpu.h"
#include "cwsr_trap_handler.h"

#define MQD_SIZE_ALIGNED 768

int max_num_of_queues_per_device = KCD_MAX_NUM_OF_QUEUES_PER_DEVICE_DEFAULT;

/*
 * kcd_locked is used to lock the kcd driver during suspend or reset
 * once locked, kcd driver will stop any further GPU execution.
 * create process (open) will return -EAGAIN.
 */
static int kcd_locked;

extern const struct kcd2kgd_calls gfx_lg2xx_kcd2kgd;

static int kcd_gtt_sa_init(struct kcd_dev *kcd, unsigned int buf_size,
				unsigned int chunk_size);
static void kcd_gtt_sa_fini(struct kcd_dev *kcd);

static int kcd_resume(struct kcd_node *kcd);

static void kcd_device_info_set_sdma_info(struct kcd_dev *kcd)
{
	kcd->device_info.num_sdma_queues_per_engine = 8;
}

static void kcd_device_info_set_event_interrupt_class(struct kcd_dev *kcd)
{
	kcd->device_info.event_interrupt_class = &event_interrupt_class_lg2xx;
}

static void kcd_device_info_init(struct kcd_dev *kcd,
				 uint32_t gfx_target_version)
{
	kcd->device_info.max_pasid_bits = 16;
	kcd->device_info.max_no_of_hqd = 24;
	kcd->device_info.mqd_size_aligned = MQD_SIZE_ALIGNED;
	kcd->device_info.gfx_target_version = gfx_target_version;
	kcd->device_info.doorbell_size = 8;

	kcd->device_info.ih_ring_entry_size = 4 * sizeof(uint32_t);

	kcd_device_info_set_sdma_info(kcd);

	kcd_device_info_set_event_interrupt_class(kcd);

	kcd->device_info.needs_pci_atomics = true;
	kcd->device_info.supports_cwsr = kcd->adev->family_type >= CHIP_LG210;
	kcd->device_info.no_atomic_fw_version = 0;
}

struct kcd_dev *kgd2kcd_probe(struct loonggpu_device *adev)
{
	struct kcd_dev *kcd = NULL;
	const struct kcd2kgd_calls *f2g = NULL;
	uint32_t gfx_target_version = 0;

	switch (adev->family_type) {
	case CHIP_LG200:
		gfx_target_version = 20000;
		f2g = &gfx_lg2xx_kcd2kgd;
		break;
	case CHIP_LG210:
	case CHIP_LG211:
		gfx_target_version = 20100;
		f2g = &gfx_lg2xx_kcd2kgd;
		break;
	default:
		break;
	}

	if (!f2g) {
		dev_err(kcd_device, "%s not supported in kcd\n",
			loonggpu_family_name[adev->family_type]);
		return NULL;
	}

	kcd = kzalloc(sizeof(*kcd), GFP_KERNEL);
	if (!kcd)
		return NULL;

	kcd->adev = adev;
	kcd_device_info_init(kcd, gfx_target_version);
	kcd->init_complete = false;
	kcd->kcd2kgd = f2g;
	kcd->has_doorbells = adev->family_type >= CHIP_LG210 &&
		adev->family_type != CHIP_LG211;
	atomic_set(&kcd->compute_profile, 0);
	mutex_init(&kcd->doorbell_mutex);

	ida_init(&kcd->doorbell_ida);
	return kcd;
}

static void kcd_cwsr_init(struct kcd_dev *kcd)
{
	if (loonggpu_cwsr_enable && kcd->device_info.supports_cwsr) {
		if (kcd->adev->family_type >= CHIP_LG210) {
			BUILD_BUG_ON(sizeof(cwsr_trap_lg210_hex) > PAGE_SIZE);
			kcd->cwsr_isa = cwsr_trap_lg210_hex;
			kcd->cwsr_isa_size = sizeof(cwsr_trap_lg210_hex);
			kcd->cwsr_enabled = true;
		}
	}
	dev_info(kcd_device, "CWSR trap %s\n", kcd->cwsr_enabled ? 
		"enabled" : "disabled");
}

static void kcd_smi_init(struct kcd_node *dev)
{
	INIT_LIST_HEAD(&dev->smi_clients);
	spin_lock_init(&dev->smi_lock);
}

static int kcd_init_node(struct kcd_node *node)
{
	int err = -1;

	if (kcd_interrupt_init(node)) {
		dev_err(kcd_device, "Error initializing interrupts\n");
		goto kcd_interrupt_error;
	}

	node->dqm = device_queue_manager_init(node);
	if (!node->dqm) {
		dev_err(kcd_device, "Error initializing queue manager\n");
		goto device_queue_manager_error;
	}

	if (kcd_resume(node))
		goto kcd_resume_error;

	if (kcd_topology_add_device(node)) {
		dev_err(kcd_device, "Error adding device to topology\n");
		goto kcd_topology_add_device_error;
	}

	kcd_smi_init(node);

	return 0;

kcd_topology_add_device_error:
kcd_resume_error:
	device_queue_manager_uninit(node->dqm);
device_queue_manager_error:
	kcd_interrupt_exit(node);
kcd_interrupt_error:

	/* Cleanup the node memory here */
	kfree(node);
	return err;
}

static void kcd_cleanup_nodes(struct kcd_dev *kcd, unsigned int num_nodes)
{
	struct kcd_node *knode;
	unsigned int i;

	for (i = 0; i < num_nodes; i++) {
		knode = kcd->nodes[i];
		device_queue_manager_uninit(knode->dqm);
		kcd_interrupt_exit(knode);
		kcd_topology_remove_device(knode);
		kfree(knode);
		kcd->nodes[i] = NULL;
	}
}

bool kgd2kcd_device_init(struct kcd_dev *kcd,
			 const struct kgd2kcd_shared_resources *gpu_resources)
{
	unsigned int size, map_process_packet_size, i;
	struct kcd_node *node;
	uint32_t first_vmid_kcd, last_vmid_kcd, vmid_num_kcd;
	unsigned int max_proc_per_quantum;

	kcd->fw_version = loonggpu_lgkcd_get_fw_version(kcd->adev,
			KGD_ENGINE_MEC1);
	kcd->sdma_fw_version = loonggpu_lgkcd_get_fw_version(kcd->adev,
			KGD_ENGINE_SDMA1);
	kcd->shared_resources = *gpu_resources;

	kcd->num_nodes = 1;

	if (kcd->num_nodes == 0) {
		dev_err(kcd_device,
			"KCD num nodes cannot be 0\n");
		goto out;
	}

	/* Allow BIF to recode atomics to PCIe 3.0 AtomicOps.
	 * 32 and 64-bit requests are possible and must be
	 * supported.
	 */
	kcd->pci_atomic_requested = loonggpu_lgkcd_have_atomics_support(kcd->adev);

	first_vmid_kcd = ffs(gpu_resources->compute_vmid_bitmap)-1;
	last_vmid_kcd = fls(gpu_resources->compute_vmid_bitmap)-1;
	vmid_num_kcd = last_vmid_kcd - first_vmid_kcd + 1;

	/* Verify module parameters regarding mapped process number*/
	max_proc_per_quantum = vmid_num_kcd;

	/* calculate max size of mqds needed for queues */
	size = max_num_of_queues_per_device *
			kcd->device_info.mqd_size_aligned;

	/*
	 * calculate max size of runlist packet.
	 * There can be only 2 packets at once
	 */
	map_process_packet_size = sizeof(struct pm_map_process);
	size += (KCD_MAX_NUM_OF_PROCESSES * map_process_packet_size +
		max_num_of_queues_per_device * sizeof(struct pm_map_queues)) * 2;

	/* Add size of HIQ */
	size += KCD_KERNEL_QUEUE_SIZE * 2;

	/* add another 512KB for all other allocations on gart (HPD, fences) */
	size += 512 * 1024;

	if (loonggpu_lgkcd_alloc_gtt_mem(
			kcd->adev, size, &kcd->gtt_mem,
			&kcd->gtt_start_gpu_addr, &kcd->gtt_start_cpu_ptr,
			false)) {
		dev_err(kcd_device, "Could not allocate %d bytes\n", size);
		goto alloc_gtt_mem_failure;
	}

	dev_info(kcd_device, "Allocated %d bytes on gart\n", size);

	/* Initialize GTT sa with 512 byte chunk size */
	if (kcd_gtt_sa_init(kcd, size, 512) != 0) {
		dev_err(kcd_device, "Error initializing gtt sub-allocator\n");
		goto kcd_gtt_sa_init_error;
	}

	if (kcd->has_doorbells && kcd_doorbell_init(kcd)) {
		dev_err(kcd_device,
			"Error initializing doorbell aperture\n");
		goto kcd_doorbell_error;
	}

	kcd->noretry = kcd->adev->gmc.noretry;

	kcd_cwsr_init(kcd);

	dev_info(kcd_device, "Total number of KCD nodes to be created: %d\n",
				kcd->num_nodes);

	/* Allocate the KCD nodes */
	for (i = 0; i < kcd->num_nodes; i++) {
		node = kzalloc(sizeof(struct kcd_node), GFP_KERNEL);
		if (!node)
			goto node_alloc_error;

		node->node_id = i;
		node->adev = kcd->adev;
		node->kcd = kcd;
		node->kcd2kgd = kcd->kcd2kgd;
		node->vm_info.vmid_num_kcd = vmid_num_kcd;
		node->vm_info.first_vmid_kcd = first_vmid_kcd;
		node->vm_info.last_vmid_kcd = last_vmid_kcd;
		node->compute_vmid_bitmap =
			gpu_resources->compute_vmid_bitmap;
		node->max_proc_per_quantum = max_proc_per_quantum;
		loonggpu_lgkcd_get_local_mem_info(kcd->adev,
					&node->local_mem_info);

		/* Initialize the KCD node */
		if (kcd_init_node(node)) {
			dev_err(kcd_device, "Error initializing KCD node\n");
			goto node_init_error;
		}
		kcd->nodes[i] = node;
	}

	svm_range_set_max_pages(kcd->adev);

	kcd->init_complete = true;
	dev_info(kcd_device, "added device %x:%x\n", kcd->adev->pdev->vendor,
		 kcd->adev->pdev->device);

	pr_debug("Starting kcd with the following scheduling policy %d\n",
		node->dqm->sched_policy);

	goto out;

node_init_error:
node_alloc_error:
	kcd_cleanup_nodes(kcd, i);
	if (kcd->has_doorbells)
		kcd_doorbell_fini(kcd);
kcd_doorbell_error:
	kcd_gtt_sa_fini(kcd);
kcd_gtt_sa_init_error:
	loonggpu_lgkcd_free_gtt_mem(kcd->adev, kcd->gtt_mem);
alloc_gtt_mem_failure:
	dev_err(kcd_device,
		"device %x:%x NOT added due to errors\n",
		kcd->adev->pdev->vendor, kcd->adev->pdev->device);
out:
	return kcd->init_complete;
}

void kgd2kcd_device_exit(struct kcd_dev *kcd)
{
	if (kcd->init_complete) {
		/* Cleanup KCD nodes */
		kcd_cleanup_nodes(kcd, kcd->num_nodes);
		/* Cleanup common/shared resources */
		if (kcd->has_doorbells)
			kcd_doorbell_fini(kcd);
		ida_destroy(&kcd->doorbell_ida);
		kcd_gtt_sa_fini(kcd);
		loonggpu_lgkcd_free_gtt_mem(kcd->adev, kcd->gtt_mem);
	}

	kfree(kcd);
}

int kgd2kcd_pre_reset(struct kcd_dev *kcd)
{
	struct kcd_node *node;
	int i;

	if (!kcd->init_complete)
		return 0;

	for (i = 0; i < kcd->num_nodes; i++) {
		node = kcd->nodes[i];
		kcd_smi_event_update_gpu_reset(node, false);
		node->dqm->ops.pre_reset(node->dqm);
	}

	kgd2kcd_suspend(kcd, false);

	for (i = 0; i < kcd->num_nodes; i++)
		kcd_signal_reset_event(kcd->nodes[i]);

	return 0;
}

/*
 * Fix me. KCD won't be able to resume existing process for now.
 * We will keep all existing process in a evicted state and
 * wait the process to be terminated.
 */

int kgd2kcd_post_reset(struct kcd_dev *kcd)
{
	int ret;
	struct kcd_node *node;
	int i;

	if (!kcd->init_complete)
		return 0;

	for (i = 0; i < kcd->num_nodes; i++) {
		ret = kcd_resume(kcd->nodes[i]);
		if (ret)
			return ret;
	}

	mutex_lock(&kcd_processes_mutex);
	--kcd_locked;
	mutex_unlock(&kcd_processes_mutex);

	for (i = 0; i < kcd->num_nodes; i++) {
		node = kcd->nodes[i];
		kcd_smi_event_update_gpu_reset(node, true);
	}

	return 0;
}

bool kcd_is_locked(void)
{
	lockdep_assert_held(&kcd_processes_mutex);
	return  (kcd_locked > 0);
}

void kgd2kcd_suspend(struct kcd_dev *kcd, bool run_pm)
{
	struct kcd_node *node;
	int i;
	int count;

	if (!kcd->init_complete)
		return;

	/* for runtime suspend, skip locking kcd */
	if (!run_pm) {
		mutex_lock(&kcd_processes_mutex);
		count = ++kcd_locked;
		mutex_unlock(&kcd_processes_mutex);

		/* For first KCD device suspend all the KCD processes */
		if (count == 1)
			kcd_suspend_all_processes();
	}

	for (i = 0; i < kcd->num_nodes; i++) {
		node = kcd->nodes[i];
		node->dqm->ops.stop(node->dqm);
	}
}

int kgd2kcd_resume(struct kcd_dev *kcd, bool run_pm)
{
	int ret, count, i;

	if (!kcd->init_complete)
		return 0;

	for (i = 0; i < kcd->num_nodes; i++) {
		ret = kcd_resume(kcd->nodes[i]);
		if (ret)
			return ret;
	}

	/* for runtime resume, skip unlocking kcd */
	if (!run_pm) {
		mutex_lock(&kcd_processes_mutex);
		count = --kcd_locked;
		mutex_unlock(&kcd_processes_mutex);

		WARN_ONCE(count < 0, "KCD suspend / resume ref. error");
		if (count == 0)
			ret = kcd_resume_all_processes();
	}

	return ret;
}

static int kcd_resume(struct kcd_node *node)
{
	int err = 0;

	err = node->dqm->ops.start(node->dqm);
	if (err)
		dev_err(kcd_device,
			"Error starting queue manager for device %x:%x\n",
			node->adev->pdev->vendor, node->adev->pdev->device);

	return err;
}

static inline void kcd_queue_work(struct workqueue_struct *wq,
				  struct work_struct *work)
{
	int cpu, new_cpu;

	cpu = new_cpu = smp_processor_id();
	do {
		new_cpu = cpumask_next(new_cpu, cpu_online_mask) % nr_cpu_ids;
		if (cpu_to_node(new_cpu) == numa_node_id())
			break;
	} while (cpu != new_cpu);

	queue_work_on(new_cpu, wq, work);
}

/* This is called directly from KGD at ISR. */
void kgd2kcd_interrupt(struct kcd_dev *kcd, const void *ih_ring_entry)
{
	uint32_t patched_ihre[KCD_MAX_RING_ENTRY_SIZE], i;
	bool is_patched = false;
	unsigned long flags;
	struct kcd_node *node;

	if (!kcd->init_complete)
		return;

	if (kcd->device_info.ih_ring_entry_size > sizeof(patched_ihre)) {
		dev_err_once(kcd_device, "Ring entry too small\n");
		return;
	}

	for (i = 0; i < kcd->num_nodes; i++) {
		node = kcd->nodes[i];
		spin_lock_irqsave(&node->interrupt_lock, flags);

		if (node->interrupts_active
		    && interrupt_is_wanted(node, ih_ring_entry,
			    	patched_ihre, &is_patched)
		    && enqueue_ih_ring_entry(node,
			    	is_patched ? patched_ihre : ih_ring_entry)) {
			kcd_queue_work(node->ih_wq, &node->interrupt_work);
			spin_unlock_irqrestore(&node->interrupt_lock, flags);
			return;
		}
		spin_unlock_irqrestore(&node->interrupt_lock, flags);
	}

}

int kgd2kcd_quiesce_mm(struct mm_struct *mm, uint32_t trigger)
{
	struct kcd_process *p;
	int r;

	/* Because we are called from arbitrary context (workqueue) as opposed
	 * to process context, kcd_process could attempt to exit while we are
	 * running so the lookup function increments the process ref count.
	 */
	p = kcd_lookup_process_by_mm(mm);
	if (!p)
		return -ESRCH;

	WARN(debug_evictions, "Evicting pid %d", p->lead_thread->pid);
	r = kcd_process_evict_queues(p, trigger);

	kcd_unref_process(p);
	return r;
}

int kgd2kcd_resume_mm(struct mm_struct *mm)
{
	struct kcd_process *p;
	int r;

	/* Because we are called from arbitrary context (workqueue) as opposed
	 * to process context, kcd_process could attempt to exit while we are
	 * running so the lookup function increments the process ref count.
	 */
	p = kcd_lookup_process_by_mm(mm);
	if (!p)
		return -ESRCH;

	r = kcd_process_restore_queues(p);

	kcd_unref_process(p);
	return r;
}

/** kgd2kcd_schedule_evict_and_restore_process - Schedules work queue that will
 *   prepare for safe eviction of KCD BOs that belong to the specified
 *   process.
 *
 * @mm: mm_struct that identifies the specified KCD process
 * @fence: eviction fence attached to KCD process BOs
 *
 */
int kgd2kcd_schedule_evict_and_restore_process(struct mm_struct *mm,
					       struct dma_fence *fence)
{
	struct kcd_process *p;
	unsigned long active_time;
	unsigned long delay_jiffies = msecs_to_jiffies(PROCESS_ACTIVE_TIME_MS);

	if (!fence)
		return -EINVAL;

	if (dma_fence_is_signaled(fence))
		return 0;

	p = kcd_lookup_process_by_mm(mm);
	if (!p)
		return -ENODEV;

	if (fence->seqno == p->last_eviction_seqno)
		goto out;

	p->last_eviction_seqno = fence->seqno;

	/* Avoid KCD process starvation. Wait for at least
	 * PROCESS_ACTIVE_TIME_MS before evicting the process again
	 */
	active_time = get_jiffies_64() - p->last_restore_timestamp;
	if (delay_jiffies > active_time)
		delay_jiffies -= active_time;
	else
		delay_jiffies = 0;

	/* During process initialization eviction_work.dwork is initialized
	 * to kcd_evict_bo_worker
	 */
	WARN(debug_evictions, "Scheduling eviction of pid %d in %ld jiffies",
	     p->lead_thread->pid, delay_jiffies);
	schedule_delayed_work(&p->eviction_work, delay_jiffies);
out:
	kcd_unref_process(p);
	return 0;
}

static int kcd_gtt_sa_init(struct kcd_dev *kcd, unsigned int buf_size,
				unsigned int chunk_size)
{
	if (WARN_ON(buf_size < chunk_size))
		return -EINVAL;
	if (WARN_ON(buf_size == 0))
		return -EINVAL;
	if (WARN_ON(chunk_size == 0))
		return -EINVAL;

	kcd->gtt_sa_chunk_size = chunk_size;
	kcd->gtt_sa_num_of_chunks = buf_size / chunk_size;

	kcd->gtt_sa_bitmap = bitmap_zalloc(kcd->gtt_sa_num_of_chunks,
					   GFP_KERNEL);
	if (!kcd->gtt_sa_bitmap)
		return -ENOMEM;

	pr_debug("gtt_sa_num_of_chunks = %d, gtt_sa_bitmap = %p\n",
			kcd->gtt_sa_num_of_chunks, kcd->gtt_sa_bitmap);

	mutex_init(&kcd->gtt_sa_lock);

	return 0;
}

static void kcd_gtt_sa_fini(struct kcd_dev *kcd)
{
	mutex_destroy(&kcd->gtt_sa_lock);
	bitmap_free(kcd->gtt_sa_bitmap);
}

static inline uint64_t kcd_gtt_sa_calc_gpu_addr(uint64_t start_addr,
						unsigned int bit_num,
						unsigned int chunk_size)
{
	return start_addr + bit_num * chunk_size;
}

static inline uint32_t *kcd_gtt_sa_calc_cpu_addr(void *start_addr,
						unsigned int bit_num,
						unsigned int chunk_size)
{
	return (uint32_t *) ((uint64_t) start_addr + bit_num * chunk_size);
}

int kcd_gtt_sa_allocate(struct kcd_node *node, unsigned int size,
			struct kcd_mem_obj **mem_obj)
{
	unsigned int found, start_search, cur_size;
	struct kcd_dev *kcd = node->kcd;

	if (size == 0)
		return -EINVAL;

	if (size > kcd->gtt_sa_num_of_chunks * kcd->gtt_sa_chunk_size)
		return -ENOMEM;

	*mem_obj = kzalloc(sizeof(struct kcd_mem_obj), GFP_KERNEL);
	if (!(*mem_obj))
		return -ENOMEM;

	pr_debug("Allocated mem_obj = %p for size = %d\n", *mem_obj, size);

	start_search = 0;

	mutex_lock(&kcd->gtt_sa_lock);

kcd_gtt_restart_search:
	/* Find the first chunk that is free */
	found = find_next_zero_bit(kcd->gtt_sa_bitmap,
					kcd->gtt_sa_num_of_chunks,
					start_search);

	pr_debug("Found = %d\n", found);

	/* If there wasn't any free chunk, bail out */
	if (found == kcd->gtt_sa_num_of_chunks)
		goto kcd_gtt_no_free_chunk;

	/* Update fields of mem_obj */
	(*mem_obj)->range_start = found;
	(*mem_obj)->range_end = found;
	(*mem_obj)->gpu_addr = kcd_gtt_sa_calc_gpu_addr(
					kcd->gtt_start_gpu_addr,
					found,
					kcd->gtt_sa_chunk_size);
	(*mem_obj)->cpu_ptr = kcd_gtt_sa_calc_cpu_addr(
					kcd->gtt_start_cpu_ptr,
					found,
					kcd->gtt_sa_chunk_size);

	pr_debug("gpu_addr = %p, cpu_addr = %p\n",
			(uint64_t *) (*mem_obj)->gpu_addr, (*mem_obj)->cpu_ptr);

	/* If we need only one chunk, mark it as allocated and get out */
	if (size <= kcd->gtt_sa_chunk_size) {
		pr_debug("Single bit\n");
		__set_bit(found, kcd->gtt_sa_bitmap);
		goto kcd_gtt_out;
	}

	/* Otherwise, try to see if we have enough contiguous chunks */
	cur_size = size - kcd->gtt_sa_chunk_size;
	do {
		(*mem_obj)->range_end =
			find_next_zero_bit(kcd->gtt_sa_bitmap,
					kcd->gtt_sa_num_of_chunks, ++found);
		/*
		 * If next free chunk is not contiguous than we need to
		 * restart our search from the last free chunk we found (which
		 * wasn't contiguous to the previous ones
		 */
		if ((*mem_obj)->range_end != found) {
			start_search = found;
			goto kcd_gtt_restart_search;
		}

		/*
		 * If we reached end of buffer, bail out with error
		 */
		if (found == kcd->gtt_sa_num_of_chunks)
			goto kcd_gtt_no_free_chunk;

		/* Check if we don't need another chunk */
		if (cur_size <= kcd->gtt_sa_chunk_size)
			cur_size = 0;
		else
			cur_size -= kcd->gtt_sa_chunk_size;

	} while (cur_size > 0);

	pr_debug("range_start = %d, range_end = %d\n",
		(*mem_obj)->range_start, (*mem_obj)->range_end);

	/* Mark the chunks as allocated */
	bitmap_set(kcd->gtt_sa_bitmap, (*mem_obj)->range_start,
		   (*mem_obj)->range_end - (*mem_obj)->range_start + 1);

kcd_gtt_out:
	mutex_unlock(&kcd->gtt_sa_lock);
	return 0;

kcd_gtt_no_free_chunk:
	pr_debug("Allocation failed with mem_obj = %p\n", *mem_obj);
	mutex_unlock(&kcd->gtt_sa_lock);
	kfree(*mem_obj);
	return -ENOMEM;
}

int kcd_gtt_sa_free(struct kcd_node *node, struct kcd_mem_obj *mem_obj)
{
	struct kcd_dev *kcd = node->kcd;

	/* Act like kfree when trying to free a NULL object */
	if (!mem_obj)
		return 0;

	pr_debug("Free mem_obj = %p, range_start = %d, range_end = %d\n",
			mem_obj, mem_obj->range_start, mem_obj->range_end);

	mutex_lock(&kcd->gtt_sa_lock);

	/* Mark the chunks as free */
	bitmap_clear(kcd->gtt_sa_bitmap, mem_obj->range_start,
		     mem_obj->range_end - mem_obj->range_start + 1);

	mutex_unlock(&kcd->gtt_sa_lock);

	kfree(mem_obj);
	return 0;
}

void kcd_inc_compute_active(struct kcd_node *node)
{
	if (atomic_inc_return(&node->kcd->compute_profile) == 1)
		loonggpu_lgkcd_set_compute_idle(node->adev, false);
}

void kcd_dec_compute_active(struct kcd_node *node)
{
	int count = atomic_dec_return(&node->kcd->compute_profile);

	if (count == 0)
		loonggpu_lgkcd_set_compute_idle(node->adev, true);
	WARN_ONCE(count < 0, "Compute profile ref. count error");
}

/* kcd_get_num_sdma_engines returns the number of PCIe optimized SDMA.
 */
unsigned int kcd_get_num_sdma_engines(struct kcd_node *node)
{
	return node->adev->xdma.num_instances/(int)node->kcd->num_nodes;
}

int kgd2kcd_check_and_lock_kcd(void)
{
	mutex_lock(&kcd_processes_mutex);
	if (!hash_empty(kcd_processes_table) || kcd_is_locked()) {
		mutex_unlock(&kcd_processes_mutex);
		return -EBUSY;
	}

	++kcd_locked;
	mutex_unlock(&kcd_processes_mutex);

	return 0;
}

void kgd2kcd_unlock_kcd(void)
{
	mutex_lock(&kcd_processes_mutex);
	--kcd_locked;
	mutex_unlock(&kcd_processes_mutex);
}

#if defined(CONFIG_DEBUG_FS)

/* This function will send a package to HIQ to hang the HWS
 * which will trigger a GPU reset and bring the HWS back to normal state
 */
int kcd_debugfs_hang_hws(struct kcd_node *dev)
{
	if (dev->dqm->sched_policy != KCD_SCHED_POLICY_HWS) {
		pr_err("HWS is not enabled");
		return -EINVAL;
	}

	return dqm_debugfs_hang_hws(dev->dqm);
}

#endif
