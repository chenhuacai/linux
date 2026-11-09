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

#include <linux/device.h>
#include <linux/export.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/compat.h>
#include <linux/time.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/ptrace.h>
#include <linux/dma-buf.h>
#include <linux/fdtable.h>
#include <linux/processor.h>
#include "kcd_priv.h"
#include "kcd_device_queue_manager.h"
#include "kcd_svm.h"
#include "kcd_ipc.h"
#include "loonggpu_lgkcd.h"
#include "kcd_smi_events.h"
#include "loonggpu_dma_buf.h"
#include "kcd_debug.h"

static long kcd_ioctl(struct file *, unsigned int, unsigned long);
static int kcd_open(struct inode *, struct file *);
static int kcd_release(struct inode *, struct file *);
static int kcd_mmap(struct file *, struct vm_area_struct *);

static const char kcd_dev_name[] = "kcd";

static const struct file_operations kcd_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = kcd_ioctl,
	.compat_ioctl = kcd_ioctl,
	.open = kcd_open,
	.release = kcd_release,
	.mmap = kcd_mmap,
};

static int kcd_char_dev_major = -1;
static struct class *kcd_class;
struct device *kcd_device;

static inline struct kcd_process_device *kcd_lock_pdd_by_id(struct kcd_process *p, __u32 gpu_id)
{
	struct kcd_process_device *pdd;

	mutex_lock(&p->mutex);
	pdd = kcd_process_device_data_by_id(p, gpu_id);

	if (pdd)
		return pdd;

	mutex_unlock(&p->mutex);
	return NULL;
}

static inline void kcd_unlock_pdd(struct kcd_process_device *pdd)
{
	mutex_unlock(&pdd->process->mutex);
}

int kcd_chardev_init(void)
{
	int err = 0;

	kcd_char_dev_major = register_chrdev(0, kcd_dev_name, &kcd_fops);
	err = kcd_char_dev_major;
	if (err < 0)
		goto err_register_chrdev;
	kcd_class = class_create(kcd_dev_name);
	err = PTR_ERR(kcd_class);
	if (IS_ERR(kcd_class))
		goto err_class_create;

	kcd_device = device_create(kcd_class, NULL,
					MKDEV(kcd_char_dev_major, 0),
					NULL, kcd_dev_name);
	err = PTR_ERR(kcd_device);
	if (IS_ERR(kcd_device))
		goto err_device_create;

	return 0;

err_device_create:
	class_destroy(kcd_class);
err_class_create:
	unregister_chrdev(kcd_char_dev_major, kcd_dev_name);
err_register_chrdev:
	return err;
}

void kcd_chardev_exit(void)
{
	device_destroy(kcd_class, MKDEV(kcd_char_dev_major, 0));
	class_destroy(kcd_class);
	unregister_chrdev(kcd_char_dev_major, kcd_dev_name);
	kcd_device = NULL;
}


static int kcd_open(struct inode *inode, struct file *filep)
{
	struct kcd_process *process;
	bool is_32bit_user_mode;

	if (iminor(inode) != 0)
		return -ENODEV;

	is_32bit_user_mode = in_compat_syscall();

	if (is_32bit_user_mode) {
		dev_warn(kcd_device,
			"Process %d (32-bit) failed to open /dev/kcd\n"
			"32-bit processes are not supported by lgkcd\n",
			current->pid);
		return -EPERM;
	}

	process = kcd_create_process(current);
	if (IS_ERR(process))
		return PTR_ERR(process);

	/* filep now owns the reference returned by kcd_create_process */
	filep->private_data = process;

	dev_dbg(kcd_device, "process %d opened, compat mode (32 bit) - %d\n",
		process->pasid, process->is_32bit_user_mode);

	return 0;
}

static int kcd_release(struct inode *inode, struct file *filep)
{
	struct kcd_process *process = filep->private_data;

	if (process)
		kcd_unref_process(process);

	return 0;
}

static int kcd_ioctl_get_version(struct file *filep, struct kcd_process *p,
					void *data)
{
	struct kcd_ioctl_get_version_args *args = data;

	args->major_version = KCD_IOCTL_MAJOR_VERSION;
	args->minor_version = KCD_IOCTL_MINOR_VERSION;

	return 0;
}

static int set_queue_properties_from_user(struct queue_properties *q_properties,
				struct kcd_ioctl_create_queue_args *args)
{
	/*
	 * Repurpose queue percentage to accommodate new features:
	 * bit 0-7: queue percentage
	 * bit 8-15: pm4_target_xcc
	 */
	if ((args->queue_percentage & 0xFF) > KCD_MAX_QUEUE_PERCENTAGE) {
		pr_err("Queue percentage must be between 0 to KCD_MAX_QUEUE_PERCENTAGE\n");
		return -EINVAL;
	}

	if (args->queue_priority > KCD_MAX_QUEUE_PRIORITY) {
		pr_err("Queue priority must be between 0 to KCD_MAX_QUEUE_PRIORITY\n");
		return -EINVAL;
	}

	if ((args->ring_base_address) &&
		(!lg_access_ok(VERIFY_WRITE, (const void __user *) args->ring_base_address,
			sizeof(uint64_t)))) {
		pr_err("Can't access ring base address\n");
		return -EFAULT;
	}

	if (!is_power_of_2(args->ring_size) && (args->ring_size != 0)) {
		pr_err("Ring size must be a power of 2 or 0\n");
		return -EINVAL;
	}

	if (!lg_access_ok(VERIFY_WRITE, (const void __user *) args->read_pointer_address,
			sizeof(uint32_t))) {
		pr_err("Can't access read pointer\n");
		return -EFAULT;
	}

	if (!lg_access_ok(VERIFY_WRITE, (const void __user *) args->write_pointer_address,
			sizeof(uint32_t))) {
		pr_err("Can't access write pointer\n");
		return -EFAULT;
	}

	if (args->ctx_save_restore_address &&
		!lg_access_ok(VERIFY_WRITE, (const void __user *) args->ctx_save_restore_address,
			sizeof(uint32_t))) {
		pr_debug("Can't access ctx save restore buffer");
		return -EFAULT;
	}

	q_properties->is_interop = false;
	q_properties->queue_percent = args->queue_percentage & 0xFF;
	q_properties->priority = args->queue_priority;
	q_properties->queue_address = args->ring_base_address;
	q_properties->queue_size = args->ring_size;
	q_properties->read_ptr = (uint32_t *) args->read_pointer_address;
	q_properties->write_ptr = (uint32_t *) args->write_pointer_address;
	q_properties->ctx_save_restore_area_address =
			args->ctx_save_restore_address;
	q_properties->ctx_save_restore_area_size = args->ctx_save_restore_size;

	if (args->queue_type == KCD_IOC_QUEUE_TYPE_COMPUTE)
		q_properties->type = KCD_QUEUE_TYPE_COMPUTE;
	else if (args->queue_type == KCD_IOC_QUEUE_TYPE_SDMA)
		q_properties->type = KCD_QUEUE_TYPE_SDMA;
	else
		return -ENOTSUPP;

	q_properties->format = KCD_QUEUE_FORMAT_PM4;

	pr_debug("Queue Percentage: %d, %d\n",
			q_properties->queue_percent, args->queue_percentage);

	pr_debug("Queue Priority: %d, %d\n",
			q_properties->priority, args->queue_priority);

	pr_debug("Queue Address: 0x%llX, 0x%llX\n",
			q_properties->queue_address, args->ring_base_address);

	pr_debug("Queue Size: 0x%llX, %u\n",
			q_properties->queue_size, args->ring_size);

	pr_debug("Queue r/w Pointers: %px, %px\n",
			q_properties->read_ptr,
			q_properties->write_ptr);

	pr_debug("Queue Format: %d\n", q_properties->format);

	pr_debug("Queue CTX save area: 0x%llX size %u\n",
			q_properties->ctx_save_restore_area_address,
			q_properties->ctx_save_restore_area_size);

	return 0;
}

static int kcd_ioctl_create_queue(struct file *filep, struct kcd_process *p,
					void *data)
{
	struct kcd_ioctl_create_queue_args *args = data;
	struct kcd_node *dev;
	int err = 0;
	unsigned int queue_id;
	struct kcd_process_device *pdd;
	uint32_t doorbell_offset_in_process = 0;
	struct queue_properties q_properties;

	memset(&q_properties, 0, sizeof(struct queue_properties));

	pr_debug("Creating queue ioctl\n");

	err = set_queue_properties_from_user(&q_properties, args);
	if (err)
		return err;

	pr_debug("Looking for gpu id 0x%x\n", args->gpu_id);

	mutex_lock(&p->mutex);

	pdd = kcd_process_device_data_by_id(p, args->gpu_id);
	if (!pdd) {
		pr_debug("Could not find gpu id 0x%x\n", args->gpu_id);
		err = -EINVAL;
		goto err_pdd;
	}
	dev = pdd->dev;

	pdd = kcd_bind_process_to_device(dev, p);
	if (IS_ERR(pdd)) {
		err = -ESRCH;
		goto err_bind_process;
	}

	if (pdd->dev->kcd->has_doorbells && !pdd->qpd.proc_doorbells) {
		err = kcd_alloc_process_doorbells(dev->kcd, pdd);
		if (err) {
			pr_debug("failed to allocate process doorbells\n");
			goto err_bind_process;
		}
	}

	pr_debug("Creating queue for PASID 0x%x on gpu 0x%x\n",
			p->pasid,
			dev->id);

	err = pqm_create_queue(&p->pqm, dev, filep, &q_properties, &queue_id,
			NULL, &doorbell_offset_in_process);
	if (err != 0)
		goto err_create_queue;

	args->queue_id = queue_id;

	/* Return gpu_id as doorbell offset for mmap usage */
	args->doorbell_offset = 0;
	if (dev->kcd->has_doorbells) {
		args->doorbell_offset = KCD_MMAP_TYPE_DOORBELL;
		args->doorbell_offset |= KCD_MMAP_GPU_ID(args->gpu_id);
		args->doorbell_offset |= doorbell_offset_in_process;
	}

	mutex_unlock(&p->mutex);

	pr_debug("Queue id %d was created successfully\n", args->queue_id);

	pr_debug("Ring buffer address == 0x%016llX\n",
			args->ring_base_address);

	pr_debug("Read ptr address    == 0x%016llX\n",
			args->read_pointer_address);

	pr_debug("Write ptr address   == 0x%016llX\n",
			args->write_pointer_address);

	kcd_dbg_ev_raise(KCD_EC_MASK(EC_QUEUE_NEW), p, dev, queue_id, false, NULL, 0);
	return 0;

err_create_queue:
err_bind_process:
err_pdd:
	mutex_unlock(&p->mutex);
	return err;
}

static int kcd_ioctl_destroy_queue(struct file *filp, struct kcd_process *p,
					void *data)
{
	int retval;
	struct kcd_ioctl_destroy_queue_args *args = data;

	pr_debug("Destroying queue id %d for pasid 0x%x\n",
				args->queue_id,
				p->pasid);

	mutex_lock(&p->mutex);

	retval = pqm_destroy_queue(&p->pqm, args->queue_id);

	mutex_unlock(&p->mutex);
	return retval;
}

static int kcd_ioctl_update_queue(struct file *filp, struct kcd_process *p,
					void *data)
{
	int retval;
	struct kcd_ioctl_update_queue_args *args = data;
	struct queue_properties properties;

	/*
	 * Repurpose queue percentage to accommodate new features:
	 * bit 0-7: queue percentage
	 * bit 8-15: pm4_target_xcc
	 */
	if ((args->queue_percentage & 0xFF) > KCD_MAX_QUEUE_PERCENTAGE) {
		pr_err("Queue percentage must be between 0 to KCD_MAX_QUEUE_PERCENTAGE\n");
		return -EINVAL;
	}

	if (args->queue_priority > KCD_MAX_QUEUE_PRIORITY) {
		pr_err("Queue priority must be between 0 to KCD_MAX_QUEUE_PRIORITY\n");
		return -EINVAL;
	}

	if ((args->ring_base_address) &&
		(!lg_access_ok(VERIFY_WRITE, (const void __user *) args->ring_base_address,
			sizeof(uint64_t)))) {
		pr_err("Can't access ring base address\n");
		return -EFAULT;
	}

	if (!is_power_of_2(args->ring_size) && (args->ring_size != 0)) {
		pr_err("Ring size must be a power of 2 or 0\n");
		return -EINVAL;
	}

	properties.queue_address = args->ring_base_address;
	properties.queue_size = args->ring_size;
	properties.queue_percent = args->queue_percentage & 0xFF;
	properties.priority = args->queue_priority;

	pr_debug("Updating queue id %d for pasid 0x%x\n",
			args->queue_id, p->pasid);

	mutex_lock(&p->mutex);

	retval = pqm_update_queue_properties(&p->pqm, args->queue_id, &properties);

	mutex_unlock(&p->mutex);

	return retval;
}

static int kcd_ioctl_set_trap_handler(struct file *filep,
					struct kcd_process *p, void *data)
{
	struct kcd_ioctl_set_trap_handler_args *args = data;
	int err = 0;
	struct kcd_process_device *pdd;

	mutex_lock(&p->mutex);

	pdd = kcd_process_device_data_by_id(p, args->gpu_id);
	if (!pdd) {
		err = -EINVAL;
		goto err_pdd;
	}

	pdd = kcd_bind_process_to_device(pdd->dev, p);
	if (IS_ERR(pdd)) {
		err = -ESRCH;
		goto out;
	}

	kcd_process_set_trap_handler(&pdd->qpd, args->tba_addr, args->tma_addr);

out:
err_pdd:
	mutex_unlock(&p->mutex);

	return err;
}

static int kcd_ioctl_get_clock_counters(struct file *filep,
				struct kcd_process *p, void *data)
{
	struct kcd_ioctl_get_clock_counters_args *args = data;
	struct kcd_process_device *pdd;

	mutex_lock(&p->mutex);
	pdd = kcd_process_device_data_by_id(p, args->gpu_id);
	mutex_unlock(&p->mutex);
	if (pdd)
		/* Reading GPU clock counter from KGD */
		args->gpu_clock_counter = loonggpu_lgkcd_get_gpu_clock_counter(pdd->dev->adev);
	else
		/* Node without GPU resource */
		args->gpu_clock_counter = 0;

	/* No access to rdtsc. Using raw monotonic time */
	args->cpu_clock_counter = ktime_get_raw_ns();
	args->system_clock_counter = ktime_to_ns(ktime_get_boottime());

	/* Since the counter is in nano-seconds we use 1GHz frequency */
	args->system_clock_freq = 1000000000;

	return 0;
}


static int kcd_ioctl_get_process_apertures(struct file *filp,
				struct kcd_process *p, void *data)
{
	struct kcd_ioctl_get_process_apertures_args *args = data;
	struct kcd_process_device_apertures *pAperture;
	int i;

	dev_dbg(kcd_device, "get apertures for PASID 0x%x", p->pasid);

	args->num_of_nodes = 0;

	mutex_lock(&p->mutex);
	/* Run over all pdd of the process */
	for (i = 0; i < p->n_pdds; i++) {
		struct kcd_process_device *pdd = p->pdds[i];

		pAperture =
			&args->process_apertures[args->num_of_nodes];
		pAperture->gpu_id = pdd->dev->id;
		pAperture->lds_base = pdd->lds_base;
		pAperture->lds_limit = pdd->lds_limit;
		pAperture->gpuvm_base = pdd->gpuvm_base;
		pAperture->gpuvm_limit = pdd->gpuvm_limit;
		pAperture->scratch_base = pdd->scratch_base;
		pAperture->scratch_limit = pdd->scratch_limit;

		dev_dbg(kcd_device,
			"node id %u\n", args->num_of_nodes);
		dev_dbg(kcd_device,
			"gpu id %u\n", pdd->dev->id);
		dev_dbg(kcd_device,
			"lds_base %llX\n", pdd->lds_base);
		dev_dbg(kcd_device,
			"lds_limit %llX\n", pdd->lds_limit);
		dev_dbg(kcd_device,
			"gpuvm_base %llX\n", pdd->gpuvm_base);
		dev_dbg(kcd_device,
			"gpuvm_limit %llX\n", pdd->gpuvm_limit);
		dev_dbg(kcd_device,
			"scratch_base %llX\n", pdd->scratch_base);
		dev_dbg(kcd_device,
			"scratch_limit %llX\n", pdd->scratch_limit);

		if (++args->num_of_nodes >= NUM_OF_SUPPORTED_GPUS)
			break;
	}
	mutex_unlock(&p->mutex);

	return 0;
}

static int kcd_ioctl_get_process_apertures_new(struct file *filp,
				struct kcd_process *p, void *data)
{
	struct kcd_ioctl_get_process_apertures_new_args *args = data;
	struct kcd_process_device_apertures *pa;
	int ret;
	int i;

	dev_dbg(kcd_device, "get apertures for PASID 0x%x", p->pasid);

	if (args->num_of_nodes == 0) {
		/* Return number of nodes, so that user space can alloacate
		 * sufficient memory
		 */
		mutex_lock(&p->mutex);
		args->num_of_nodes = p->n_pdds;
		goto out_unlock;
	}

	/* Fill in process-aperture information for all available
	 * nodes, but not more than args->num_of_nodes as that is
	 * the amount of memory allocated by user
	 */
	pa = kzalloc((sizeof(struct kcd_process_device_apertures) *
				args->num_of_nodes), GFP_KERNEL);
	if (!pa)
		return -ENOMEM;

	mutex_lock(&p->mutex);

	if (!p->n_pdds) {
		args->num_of_nodes = 0;
		kfree(pa);
		goto out_unlock;
	}

	/* Run over all pdd of the process */
	for (i = 0; i < min(p->n_pdds, args->num_of_nodes); i++) {
		struct kcd_process_device *pdd = p->pdds[i];

		pa[i].gpu_id = pdd->dev->id;
		pa[i].lds_base = pdd->lds_base;
		pa[i].lds_limit = pdd->lds_limit;
		pa[i].gpuvm_base = pdd->gpuvm_base;
		pa[i].gpuvm_limit = pdd->gpuvm_limit;
		pa[i].scratch_base = pdd->scratch_base;
		pa[i].scratch_limit = pdd->scratch_limit;

		dev_dbg(kcd_device,
			"gpu id %u\n", pdd->dev->id);
		dev_dbg(kcd_device,
			"lds_base %llX\n", pdd->lds_base);
		dev_dbg(kcd_device,
			"lds_limit %llX\n", pdd->lds_limit);
		dev_dbg(kcd_device,
			"gpuvm_base %llX\n", pdd->gpuvm_base);
		dev_dbg(kcd_device,
			"gpuvm_limit %llX\n", pdd->gpuvm_limit);
		dev_dbg(kcd_device,
			"scratch_base %llX\n", pdd->scratch_base);
		dev_dbg(kcd_device,
			"scratch_limit %llX\n", pdd->scratch_limit);
	}
	mutex_unlock(&p->mutex);

	args->num_of_nodes = i;
	ret = copy_to_user(
			(void __user *)args->kcd_process_device_apertures_ptr,
			pa,
			(i * sizeof(struct kcd_process_device_apertures)));
	kfree(pa);
	return ret ? -EFAULT : 0;

out_unlock:
	mutex_unlock(&p->mutex);
	return 0;
}

static int kcd_ioctl_create_event(struct file *filp, struct kcd_process *p,
					void *data)
{
	struct kcd_ioctl_create_event_args *args = data;
	int err;

	/* For dGPUs the event page is allocated in user mode. The
	 * handle is passed to KCD with the first call to this IOCTL
	 * through the event_page_offset field.
	 */
	if (args->event_page_offset) {
		mutex_lock(&p->mutex);
		err = kcd_kmap_event_page(p, args->event_page_offset);
		mutex_unlock(&p->mutex);
		if (err)
			return err;
	}

	err = kcd_event_create(filp, p, args->event_type,
				args->auto_reset != 0, args->node_id,
				&args->event_id, &args->event_trigger_data,
				&args->event_page_offset,
				&args->event_slot_index);

	pr_debug("Created event (id:0x%08x) (%s)\n", args->event_id, __func__);
	return err;
}

static int kcd_ioctl_destroy_event(struct file *filp, struct kcd_process *p,
					void *data)
{
	struct kcd_ioctl_destroy_event_args *args = data;

	return kcd_event_destroy(p, args->event_id);
}

static int kcd_ioctl_set_event(struct file *filp, struct kcd_process *p,
				void *data)
{
	struct kcd_ioctl_set_event_args *args = data;

	return kcd_set_event(p, args->event_id);
}

static int kcd_ioctl_reset_event(struct file *filp, struct kcd_process *p,
				void *data)
{
	struct kcd_ioctl_reset_event_args *args = data;

	return kcd_reset_event(p, args->event_id);
}

static int kcd_ioctl_wait_events(struct file *filp, struct kcd_process *p,
				void *data)
{
	struct kcd_ioctl_wait_events_args *args = data;

	return kcd_wait_on_events(p, args->num_events,
			(void __user *)args->events_ptr,
			(args->wait_for_all != 0),
			&args->timeout, &args->wait_result);
}

static int kcd_ioctl_acquire_vm(struct file *filep, struct kcd_process *p,
				void *data)
{
	struct kcd_ioctl_acquire_vm_args *args = data;
	struct kcd_process_device *pdd;
	struct file *drm_file;
	int ret;

	drm_file = fget(args->drm_fd);
	if (!drm_file)
		return -EINVAL;

	mutex_lock(&p->mutex);
	pdd = kcd_process_device_data_by_id(p, args->gpu_id);
	if (!pdd) {
		ret = -EINVAL;
		goto err_pdd;
	}

	if (pdd->drm_file) {
		ret = pdd->drm_file == drm_file ? 0 : -EBUSY;
		goto err_drm_file;
	}

	ret = kcd_process_device_init_vm(pdd, drm_file);
	if (ret)
		goto err_unlock;

	/* On success, the PDD keeps the drm_file reference */
	mutex_unlock(&p->mutex);

	return 0;

err_unlock:
err_pdd:
err_drm_file:
	mutex_unlock(&p->mutex);
	fput(drm_file);
	return ret;
}

bool kcd_dev_is_large_bar(struct kcd_node *dev)
{
	if (debug_largebar) {
		pr_debug("Simulate large-bar allocation on non large-bar machine\n");
		return true;
	}

	if (dev->local_mem_info.local_mem_size_private == 0 &&
	    dev->local_mem_info.local_mem_size_public > 0)
		return true;

	return false;
}

static int kcd_ioctl_get_available_memory(struct file *filep,
					  struct kcd_process *p, void *data)
{
	struct kcd_ioctl_get_available_memory_args *args = data;
	struct kcd_process_device *pdd = kcd_lock_pdd_by_id(p, args->gpu_id);

	if (!pdd)
		return -EINVAL;
	args->available = loonggpu_lgkcd_get_available_memory(pdd->dev->adev);
	kcd_unlock_pdd(pdd);
	return 0;
}

static int kcd_ioctl_alloc_memory_of_gpu(struct file *filep,
					struct kcd_process *p, void *data)
{
	struct kcd_ioctl_alloc_memory_of_gpu_args *args = data;
	struct kcd_process_device *pdd;
	void *mem;
	struct kcd_node *dev;
	int idr_handle;
	long err;
	uint64_t offset = args->mmap_offset;
	uint32_t flags = args->flags;
	uint64_t cpuva = 0;
	unsigned int mem_type = 0;

	if (args->size == 0)
		return -EINVAL;

#if IS_ENABLED(CONFIG_VDD_LOONGSON_SVM)
	/* Flush pending deferred work to avoid racing with deferred actions
	 * from previous memory map changes (e.g. munmap).
	 */
	svm_range_list_lock_and_flush_work(&p->svms, current->mm);
	mutex_lock(&p->svms.lock);
	mmap_write_unlock(current->mm);
	if (interval_tree_iter_first(&p->svms.objects,
				     args->va_addr >> PAGE_SHIFT,
				     (args->va_addr + args->size - 1) >> PAGE_SHIFT)) {
		pr_err("Address: 0x%llx already allocated by SVM\n",
			args->va_addr);
		mutex_unlock(&p->svms.lock);
		return -EADDRINUSE;
	}

	/* When register user buffer check if it has been registered by svm by
	 * buffer cpu virtual address.
	 */
	if ((flags & KCD_IOC_ALLOC_MEM_FLAGS_USERPTR) &&
	    interval_tree_iter_first(&p->svms.objects,
				     args->mmap_offset >> PAGE_SHIFT,
				     (args->mmap_offset  + args->size - 1) >> PAGE_SHIFT)) {
		pr_err("User Buffer Address: 0x%llx already allocated by SVM\n",
			args->mmap_offset);
		mutex_unlock(&p->svms.lock);
		return -EADDRINUSE;
	}

	mutex_unlock(&p->svms.lock);
#endif
	mutex_lock(&p->mutex);
	pdd = kcd_process_device_data_by_id(p, args->gpu_id);
	if (!pdd) {
		err = -EINVAL;
		goto err_pdd;
	}

	dev = pdd->dev;

	if ((flags & KCD_IOC_ALLOC_MEM_FLAGS_PUBLIC) &&
		(flags & KCD_IOC_ALLOC_MEM_FLAGS_VRAM) &&
		!kcd_dev_is_large_bar(dev)) {
		pr_err("Alloc host visible vram on small bar is not allowed\n");
		err = -EINVAL;
		goto err_large_bar;
	}

	if (flags & KCD_IOC_ALLOC_MEM_FLAGS_USERPTR) {
		if (offset & (PAGE_SIZE - 1)) {
			pr_debug("Unaligned userptr address:%llx\n",
					offset);
			err = -EINVAL;
			goto err_unlock;
		}
		cpuva = offset;
	}

	pdd = kcd_bind_process_to_device(dev, p);
	if (IS_ERR(pdd)) {
		err = PTR_ERR(pdd);
		goto err_unlock;
	}

	if (flags & KCD_IOC_ALLOC_MEM_FLAGS_DOORBELL) {
		if (args->size != kcd_doorbell_process_slice(dev->kcd)) {
			err = -EINVAL;
			goto err_unlock;
		}
		if (!dev->kcd->has_doorbells) {
			err = -EINVAL;
			goto err_unlock;
		}
		offset = kcd_get_process_doorbells(pdd);
		if (!offset) {
			err = -ENOMEM;
			goto err_unlock;
		}

		offset &= dev->adev->doorbell.size - 1;
		offset |= dev->adev->gmc.vram_start;
	}

	err = loonggpu_lgkcd_gpuvm_alloc_memory_of_gpu(
		dev->adev, args->va_addr, args->size,
		pdd->drm_priv, (struct kgd_mem **) &mem, &offset,
		flags);

	if (err)
		goto err_unlock;

	mem_type = flags & (KCD_IOC_ALLOC_MEM_FLAGS_VRAM |
			    KCD_IOC_ALLOC_MEM_FLAGS_GTT |
			    KCD_IOC_ALLOC_MEM_FLAGS_USERPTR |
			    KCD_IOC_ALLOC_MEM_FLAGS_DOORBELL);
	idr_handle = kcd_process_device_create_obj_handle(pdd, mem,
			args->va_addr, args->size, cpuva, mem_type, -1);
	if (idr_handle < 0) {
		err = -EFAULT;
		goto err_free;
	}

	/* Update the VRAM usage count */
	if (flags & KCD_IOC_ALLOC_MEM_FLAGS_VRAM) {
		uint64_t size = args->size;
		WRITE_ONCE(pdd->vram_usage, pdd->vram_usage + PAGE_ALIGN(size));
	}

	mutex_unlock(&p->mutex);

	args->handle = MAKE_HANDLE(args->gpu_id, idr_handle);
	args->mmap_offset = offset;


	return 0;

err_free:
	loonggpu_lgkcd_gpuvm_free_memory_of_gpu(dev->adev, (struct kgd_mem *)mem,
					       pdd->drm_priv, NULL);
err_unlock:
err_pdd:
err_large_bar:
	mutex_unlock(&p->mutex);
	return err;
}

static int kcd_ioctl_free_memory_of_gpu(struct file *filep,
					struct kcd_process *p, void *data)
{
	struct kcd_ioctl_free_memory_of_gpu_args *args = data;
	struct kcd_process_device *pdd;
	struct kcd_bo *buf_obj;
	int ret;
	uint64_t size = 0;

	mutex_lock(&p->mutex);
	/*
	 * Safeguard to prevent user space from freeing signal BO.
	 * It will be freed at process termination.
	 */
	if (p->signal_handle && (p->signal_handle == args->handle)) {
		pr_err("Free signal BO is not allowed\n");
		ret = -EPERM;
		goto err_unlock;
	}

	pdd = kcd_process_device_data_by_id(p, GET_GPU_ID(args->handle));
	if (!pdd) {
		pr_err("Process device data doesn't exist\n");
		ret = -EINVAL;
		goto err_pdd;
	}

	buf_obj = kcd_process_device_find_bo(pdd,
					GET_IDR_HANDLE(args->handle));
	if (!buf_obj) {
		ret = -EINVAL;
		goto err_pdd;
	}

	ret = loonggpu_lgkcd_gpuvm_free_memory_of_gpu(pdd->dev->adev,
				buf_obj->mem, pdd->drm_priv, &size);

	/* If freeing the buffer failed, leave the handle in place for
	 * clean-up during process tear-down.
	 */
	if (!ret)
		kcd_process_device_remove_obj_handle(
			pdd, GET_IDR_HANDLE(args->handle));

	WRITE_ONCE(pdd->vram_usage, pdd->vram_usage - size);

err_unlock:
err_pdd:
	mutex_unlock(&p->mutex);
	return ret;
}

static int kcd_ioctl_map_memory_to_gpu(struct file *filep,
					struct kcd_process *p, void *data)
{
	struct kcd_ioctl_map_memory_to_gpu_args *args = data;
	struct kcd_process_device *pdd, *peer_pdd;
	void *mem;
	struct kcd_node *dev;
	long err = 0;
	int i;
	uint32_t *devices_arr = NULL;

	if (!args->n_devices) {
		pr_debug("Device IDs array empty\n");
		return -EINVAL;
	}
	if (args->n_success > args->n_devices) {
		pr_debug("n_success exceeds n_devices\n");
		return -EINVAL;
	}

	devices_arr = kmalloc_array(args->n_devices, sizeof(*devices_arr),
				    GFP_KERNEL);
	if (!devices_arr)
		return -ENOMEM;

	err = copy_from_user(devices_arr,
			     (void __user *)args->device_ids_array_ptr,
			     args->n_devices * sizeof(*devices_arr));
	if (err != 0) {
		err = -EFAULT;
		goto copy_from_user_failed;
	}

	mutex_lock(&p->mutex);
	pdd = kcd_process_device_data_by_id(p, GET_GPU_ID(args->handle));
	if (!pdd) {
		err = -EINVAL;
		goto get_process_device_data_failed;
	}
	dev = pdd->dev;

	pdd = kcd_bind_process_to_device(dev, p);
	if (IS_ERR(pdd)) {
		err = PTR_ERR(pdd);
		goto bind_process_to_device_failed;
	}

	mem = kcd_process_device_translate_handle(pdd,
						GET_IDR_HANDLE(args->handle));
	if (!mem) {
		err = -ENOMEM;
		goto get_mem_obj_from_handle_failed;
	}

	for (i = args->n_success; i < args->n_devices; i++) {
		peer_pdd = kcd_process_device_data_by_id(p, devices_arr[i]);
		if (!peer_pdd) {
			pr_debug("Getting device by id failed for 0x%x\n",
				 devices_arr[i]);
			err = -EINVAL;
			goto get_mem_obj_from_handle_failed;
		}

		peer_pdd = kcd_bind_process_to_device(peer_pdd->dev, p);
		if (IS_ERR(peer_pdd)) {
			err = PTR_ERR(peer_pdd);
			goto get_mem_obj_from_handle_failed;
		}

		err = loonggpu_lgkcd_gpuvm_map_memory_to_gpu(
			peer_pdd->dev->adev, (struct kgd_mem *)mem,
			peer_pdd->drm_priv);
		if (err) {
			struct pci_dev *pdev = peer_pdd->dev->adev->pdev;

			dev_err(dev->adev->dev,
			       "Failed to map peer:%04x:%02x:%02x.%d mem_domain:%d\n",
			       pci_domain_nr(pdev->bus),
			       pdev->bus->number,
			       PCI_SLOT(pdev->devfn),
			       PCI_FUNC(pdev->devfn),
			       ((struct kgd_mem *)mem)->domain);
			goto map_memory_to_gpu_failed;
		}
		args->n_success = i+1;
	}

	err = loonggpu_lgkcd_gpuvm_sync_memory(dev->adev, (struct kgd_mem *) mem, true);
	if (err) {
		pr_debug("Sync memory failed, wait interrupted by user signal\n");
		goto sync_memory_failed;
	}

	mutex_unlock(&p->mutex);

	/* Flush TLBs after waiting for the page table updates to complete */
	for (i = 0; i < args->n_devices; i++) {
		peer_pdd = kcd_process_device_data_by_id(p, devices_arr[i]);
		if (WARN_ON_ONCE(!peer_pdd))
			continue;
		kcd_flush_tlb(peer_pdd, TLB_FLUSH_LEGACY);
	}
	kfree(devices_arr);

	return err;

get_process_device_data_failed:
bind_process_to_device_failed:
get_mem_obj_from_handle_failed:
map_memory_to_gpu_failed:
sync_memory_failed:
	mutex_unlock(&p->mutex);
copy_from_user_failed:
	kfree(devices_arr);

	return err;
}

static int kcd_ioctl_unmap_memory_from_gpu(struct file *filep,
					struct kcd_process *p, void *data)
{
	struct kcd_ioctl_unmap_memory_from_gpu_args *args = data;
	struct kcd_process_device *pdd, *peer_pdd;
	void *mem;
	long err = 0;
	uint32_t *devices_arr = NULL, i;
	bool flush_tlb;

	if (!args->n_devices) {
		pr_debug("Device IDs array empty\n");
		return -EINVAL;
	}
	if (args->n_success > args->n_devices) {
		pr_debug("n_success exceeds n_devices\n");
		return -EINVAL;
	}

	devices_arr = kmalloc_array(args->n_devices, sizeof(*devices_arr),
				    GFP_KERNEL);
	if (!devices_arr)
		return -ENOMEM;

	err = copy_from_user(devices_arr,
			     (void __user *)args->device_ids_array_ptr,
			     args->n_devices * sizeof(*devices_arr));
	if (err != 0) {
		err = -EFAULT;
		goto copy_from_user_failed;
	}

	mutex_lock(&p->mutex);
	pdd = kcd_process_device_data_by_id(p, GET_GPU_ID(args->handle));
	if (!pdd) {
		err = -EINVAL;
		goto bind_process_to_device_failed;
	}

	mem = kcd_process_device_translate_handle(pdd,
						GET_IDR_HANDLE(args->handle));
	if (!mem) {
		err = -ENOMEM;
		goto get_mem_obj_from_handle_failed;
	}

	for (i = args->n_success; i < args->n_devices; i++) {
		peer_pdd = kcd_process_device_data_by_id(p, devices_arr[i]);
		if (!peer_pdd) {
			err = -EINVAL;
			goto get_mem_obj_from_handle_failed;
		}
		err = loonggpu_lgkcd_gpuvm_unmap_memory_from_gpu(
			peer_pdd->dev->adev, (struct kgd_mem *)mem, peer_pdd->drm_priv);
		if (err) {
			pr_err("Failed to unmap from gpu %d/%d\n",
			       i, args->n_devices);
			goto unmap_memory_from_gpu_failed;
		}
		args->n_success = i+1;
	}

	flush_tlb = kcd_flush_tlb_after_unmap(pdd->dev->kcd);
	if (flush_tlb) {
		err = loonggpu_lgkcd_gpuvm_sync_memory(pdd->dev->adev,
				(struct kgd_mem *) mem, true);
		if (err) {
			pr_debug("Sync memory failed, wait interrupted by user signal\n");
			goto sync_memory_failed;
		}
	}
	mutex_unlock(&p->mutex);

	if (flush_tlb) {
		/* Flush TLBs after waiting for the page table updates to complete */
		for (i = 0; i < args->n_devices; i++) {
			peer_pdd = kcd_process_device_data_by_id(p, devices_arr[i]);
			if (WARN_ON_ONCE(!peer_pdd))
				continue;
			kcd_flush_tlb(peer_pdd, TLB_FLUSH_HEAVYWEIGHT);
		}
	}
	kfree(devices_arr);

	return 0;

bind_process_to_device_failed:
get_mem_obj_from_handle_failed:
unmap_memory_from_gpu_failed:
sync_memory_failed:
	mutex_unlock(&p->mutex);
copy_from_user_failed:
	kfree(devices_arr);
	return err;
}

static int kcd_ioctl_get_dmabuf_info(struct file *filep,
		struct kcd_process *p, void *data)
{
	struct kcd_ioctl_get_dmabuf_info_args *args = data;
	struct kcd_node *dev = NULL;
	struct loonggpu_device *dmabuf_adev;
	void *metadata_buffer = NULL;
	uint32_t flags;
	unsigned int i;
	int r;

	/* Find a KCD GPU device that supports the get_dmabuf_info query */
	for (i = 0; kcd_topology_enum_kcd_devices(i, &dev) == 0; i++)
		if (dev)
			break;
	if (!dev)
		return -EINVAL;

	if (args->metadata_ptr) {
		metadata_buffer = kzalloc(args->metadata_size, GFP_KERNEL);
		if (!metadata_buffer)
			return -ENOMEM;
	}

	/* Get dmabuf info from KGD */
	r = loonggpu_lgkcd_get_dmabuf_info(dev->adev, args->dmabuf_fd,
					  &dmabuf_adev, &args->size,
					  metadata_buffer, args->metadata_size,
					  &args->metadata_size, &flags);
	if (r)
		goto exit;

	args->gpu_id = dmabuf_adev->kcd.dev->nodes[0]->id;
	args->flags = flags;

	/* Copy metadata buffer to user mode */
	if (metadata_buffer) {
		r = copy_to_user((void __user *)args->metadata_ptr,
				 metadata_buffer, args->metadata_size);
		if (r != 0)
			r = -EFAULT;
	}

exit:
	kfree(metadata_buffer);

	return r;
}

static int kcd_ioctl_import_dmabuf(struct file *filep,
				   struct kcd_process *p, void *data)
{
	struct kcd_ioctl_import_dmabuf_args *args = data;
	struct kcd_process_device *pdd;
	int r;

	mutex_lock(&p->mutex);
	pdd = kcd_process_device_data_by_id(p, args->gpu_id);
	mutex_unlock(&p->mutex);
	if (!pdd)
		r = -EINVAL;

	r = kcd_ipc_import_dmabuf(pdd->dev, p, args->gpu_id, args->dmabuf_fd,
				  args->va_addr, &args->handle, NULL);
	if (r)
		pr_err("Failed to import dmabuf\n");

	return r;
}

static int kcd_ioctl_export_dmabuf(struct file *filep,
				   struct kcd_process *p, void *data)
{
	struct kcd_ioctl_export_dmabuf_args *args = data;
	struct kcd_process_device *pdd;
	struct dma_buf *dmabuf;
	struct kcd_node *dev;
	void *mem;
	int ret = 0;

	dev = kcd_device_by_id(GET_GPU_ID(args->handle));
	if (!dev)
		return -EINVAL;

	mutex_lock(&p->mutex);

	pdd = kcd_get_process_device_data(dev, p);
	if (!pdd) {
		ret = -EINVAL;
		goto err_unlock;
	}

	mem = kcd_process_device_translate_handle(pdd,
						GET_IDR_HANDLE(args->handle));
	if (!mem) {
		ret = -EINVAL;
		goto err_unlock;
	}

	ret = loonggpu_lgkcd_gpuvm_export_dmabuf(mem, &dmabuf);
	mutex_unlock(&p->mutex);
	if (ret)
		goto err_out;

	ret = dma_buf_fd(dmabuf, args->flags);
	if (ret < 0) {
		dma_buf_put(dmabuf);
		goto err_out;
	}
	/* dma_buf_fd assigns the reference count to the fd, no need to
	 * put the reference here.
	 */
	args->dmabuf_fd = ret;

	return 0;

err_unlock:
	mutex_unlock(&p->mutex);
err_out:
	return ret;
}

static int kcd_ioctl_ipc_export_handle(struct file *filep,
				       struct kcd_process *p,
				       void *data)
{
	struct kcd_ioctl_ipc_export_handle_args *args = data;
	struct kcd_process_device *pdd;
	int r;

	mutex_lock(&p->mutex);
	pdd = kcd_process_device_data_by_id(p, args->gpu_id);
	mutex_unlock(&p->mutex);
	if (!pdd)
		return -EINVAL;

	r = kcd_ipc_export_as_handle(pdd->dev, p, args->handle, args->share_handle,
				     args->flags);
	if (r)
		pr_err("Failed to export IPC handle\n");

	return r;
}

static int kcd_ioctl_ipc_import_handle(struct file *filep,
				       struct kcd_process *p,
				       void *data)
{
	struct kcd_ioctl_ipc_import_handle_args *args = data;
	struct kcd_process_device *pdd;
	int r;

	mutex_lock(&p->mutex);
	pdd = kcd_process_device_data_by_id(p, args->gpu_id);
	mutex_unlock(&p->mutex);
	if (!pdd)
		return -EINVAL;

	r = kcd_ipc_import_handle(pdd->dev, p, args->gpu_id, args->share_handle,
				  args->va_addr, &args->handle,
				  &args->mmap_offset, &args->flags, false);
	if (r)
		pr_err("Failed to import IPC handle\n");

	return r;
}

/* Handle requests for watching SMI events */
static int kcd_ioctl_smi_events(struct file *filep,
				struct kcd_process *p, void *data)
{
	struct kcd_ioctl_smi_events_args *args = data;
	struct kcd_process_device *pdd;

	mutex_lock(&p->mutex);

	pdd = kcd_process_device_data_by_id(p, args->gpuid);
	mutex_unlock(&p->mutex);
	if (!pdd)
		return -EINVAL;

	return kcd_smi_event_open(pdd->dev, &args->anon_fd);
}

#if IS_ENABLED(CONFIG_VDD_LOONGSON_SVM)

static int kcd_ioctl_svm(struct file *filep, struct kcd_process *p, void *data)
{
	struct kcd_ioctl_svm_args *args = data;
	int r = 0;

	pr_debug("start 0x%llx size 0x%llx op 0x%x nattr 0x%x\n",
		 args->start_addr, args->size, args->op, args->nattr);

	if ((args->start_addr & ~PAGE_MASK) || (args->size & ~PAGE_MASK))
		return -EINVAL;
	if (!args->start_addr || !args->size)
		return -EINVAL;

	r = svm_ioctl(p, args->op, args->start_addr, args->size, args->nattr,
		      args->attrs);

	return r;
}
#else
static int kcd_ioctl_svm(struct file *filep, struct kcd_process *p, void *data)
{
	return -EPERM;
}
#endif

static int runtime_enable(struct kcd_process *p, uint64_t r_debug,
			bool enable_ttmp_setup)
{
	int i = 0, ret = 0;

	if (p->is_runtime_retry)
		goto retry;

	if (p->runtime_info.runtime_state != DEBUG_RUNTIME_STATE_DISABLED)
		return -EBUSY;

	for (i = 0; i < p->n_pdds; i++) {
		struct kcd_process_device *pdd = p->pdds[i];

		if (pdd->qpd.queue_count)
			return -EEXIST;
	}

	p->runtime_info.runtime_state = DEBUG_RUNTIME_STATE_ENABLED;
	p->runtime_info.r_debug = r_debug;
	p->runtime_info.ttmp_setup = enable_ttmp_setup;

retry:
	if (p->debug_trap_enabled) {
		if (!p->is_runtime_retry) {
			kcd_dbg_trap_activate(p);
			kcd_dbg_ev_raise(KCD_EC_MASK(EC_PROCESS_RUNTIME),
					p, NULL, 0, false, NULL, 0);
		}

		mutex_unlock(&p->mutex);
		ret = down_interruptible(&p->runtime_enable_sema);
		mutex_lock(&p->mutex);

		p->is_runtime_retry = !!ret;
	}

	return ret;
}

static int runtime_disable(struct kcd_process *p)
{
	int ret;
	bool was_enabled = p->runtime_info.runtime_state == DEBUG_RUNTIME_STATE_ENABLED;

	p->runtime_info.runtime_state = DEBUG_RUNTIME_STATE_DISABLED;
	p->runtime_info.r_debug = 0;

	if (p->debug_trap_enabled) {
		if (was_enabled)
			kcd_dbg_trap_deactivate(p, false, 0);

		if (!p->is_runtime_retry)
			kcd_dbg_ev_raise(KCD_EC_MASK(EC_PROCESS_RUNTIME),
					p, NULL, 0, false, NULL, 0);

		mutex_unlock(&p->mutex);
		ret = down_interruptible(&p->runtime_enable_sema);
		mutex_lock(&p->mutex);

		p->is_runtime_retry = !!ret;
		if (ret)
			return ret;
	}

	p->runtime_info.ttmp_setup = false;

	return 0;
}

static int kcd_ioctl_runtime_enable(struct file *filep, struct kcd_process *p, void *data)
{
	struct kcd_ioctl_runtime_enable_args *args = data;
	int r;

	mutex_lock(&p->mutex);

	if (args->mode_mask & KCD_RUNTIME_ENABLE_MODE_ENABLE_MASK)
		r = runtime_enable(p, args->r_debug,
				!!(args->mode_mask & KCD_RUNTIME_ENABLE_MODE_TTMP_SAVE_MASK));
	else
		r = runtime_disable(p);

	mutex_unlock(&p->mutex);

	return r;
}

static int kcd_ioctl_set_debug_trap(struct file *filep, struct kcd_process *p, void *data)
{
	struct kcd_ioctl_dbg_trap_args *args = data;
	struct task_struct *thread = NULL;
	struct mm_struct *mm = NULL;
	struct pid *pid = NULL;
	struct kcd_process *target = NULL;
	int r = 0;

	pid = find_get_pid(args->pid);
	if (!pid) {
		pr_debug("Cannot find pid info for %i\n", args->pid);
		r = -ESRCH;
		goto out;
	}

	thread = get_pid_task(pid, PIDTYPE_PID);
	if (!thread) {
		r = -ESRCH;
		goto out;
	}

	mm = get_task_mm(thread);
	if (!mm) {
		r = -ESRCH;
		goto out;
	}

	if (args->op == KCD_IOC_DBG_TRAP_ENABLE) {
		bool create_process;

		rcu_read_lock();
		create_process = thread && thread != current && ptrace_parent(thread) == current;
		rcu_read_unlock();

		target = create_process ? kcd_create_process(thread) :
					kcd_lookup_process_by_pid(pid);
	} else {
		target = kcd_lookup_process_by_pid(pid);
	}

	if (IS_ERR_OR_NULL(target)) {
		pr_debug("Cannot find process PID %i to debug\n", args->pid);
		r = target ? PTR_ERR(target) : -ESRCH;
		goto out;
	}

	/* Check if target is still PTRACED. */
	rcu_read_lock();
	if (target != p && args->op != KCD_IOC_DBG_TRAP_DISABLE
				&& ptrace_parent(target->lead_thread) != current) {
		pr_err("PID %i is not PTRACED and cannot be debugged\n", args->pid);
		r = -EPERM;
	}
	rcu_read_unlock();

	if (r)
		goto out;

	mutex_lock(&target->mutex);

	if (args->op != KCD_IOC_DBG_TRAP_ENABLE && !target->debug_trap_enabled) {
		pr_err("PID %i not debug enabled for op %i\n", args->pid, args->op);
		r = -EINVAL;
		goto unlock_out;
	}

	if (target->runtime_info.runtime_state != DEBUG_RUNTIME_STATE_ENABLED &&
			(args->op == KCD_IOC_DBG_TRAP_SUSPEND_QUEUES ||
			 args->op == KCD_IOC_DBG_TRAP_RESUME_QUEUES ||
			 args->op == KCD_IOC_DBG_TRAP_SET_FLAGS)) {
		r = -EPERM;
		goto unlock_out;
	}

	switch (args->op) {
	case KCD_IOC_DBG_TRAP_ENABLE:
		if (target != p)
			target->debugger_process = p;

		r = kcd_dbg_trap_enable(target,
					args->enable.dbg_fd,
					(void __user *)args->enable.rinfo_ptr,
					&args->enable.rinfo_size);
		if (!r)
			target->exception_enable_mask = args->enable.exception_mask;

		break;
	case KCD_IOC_DBG_TRAP_DISABLE:
		r = kcd_dbg_trap_disable(target);
		break;
	case KCD_IOC_DBG_TRAP_SEND_RUNTIME_EVENT:
		r = kcd_dbg_send_exception_to_runtime(target,
				args->send_runtime_event.gpu_id,
				args->send_runtime_event.queue_id,
				args->send_runtime_event.exception_mask);
		break;
	case KCD_IOC_DBG_TRAP_SET_EXCEPTIONS_ENABLED:
		kcd_dbg_set_enabled_debug_exception_mask(target,
				args->set_exceptions_enabled.exception_mask);
		break;
	case KCD_IOC_DBG_TRAP_SUSPEND_QUEUES:
		r = suspend_queues(target,
				args->suspend_queues.num_queues,
				args->suspend_queues.exception_mask,
				(uint32_t *)args->suspend_queues.queue_array_ptr);

		break;
	case KCD_IOC_DBG_TRAP_RESUME_QUEUES:
		r = resume_queues(target, args->resume_queues.num_queues,
				(uint32_t *)args->resume_queues.queue_array_ptr);
		break;
	case KCD_IOC_DBG_TRAP_SET_FLAGS:
		r = kcd_dbg_trap_set_flags(target, &args->set_flags.flags);
		break;
	case KCD_IOC_DBG_TRAP_QUERY_DEBUG_EVENT:
		r = kcd_dbg_ev_query_debug_event(target,
				&args->query_debug_event.queue_id,
				&args->query_debug_event.gpu_id,
				args->query_debug_event.exception_mask,
				&args->query_debug_event.exception_mask);
		break;
	case KCD_IOC_DBG_TRAP_QUERY_EXCEPTION_INFO:
		r = kcd_dbg_trap_query_exception_info(target,
				args->query_exception_info.source_id,
				args->query_exception_info.exception_code,
				args->query_exception_info.clear_exception,
				(void __user *)args->query_exception_info.info_ptr,
				&args->query_exception_info.info_size);
		break;
	case KCD_IOC_DBG_TRAP_GET_QUEUE_SNAPSHOT:
		r = pqm_get_queue_snapshot(&target->pqm,
				args->queue_snapshot.exception_mask,
				(void __user *)args->queue_snapshot.snapshot_buf_ptr,
				&args->queue_snapshot.num_queues,
				&args->queue_snapshot.entry_size);
		break;
	case KCD_IOC_DBG_TRAP_GET_DEVICE_SNAPSHOT:
		r = kcd_dbg_trap_device_snapshot(target,
				args->device_snapshot.exception_mask,
				(void __user *)args->device_snapshot.snapshot_buf_ptr,
				&args->device_snapshot.num_devices,
				&args->device_snapshot.entry_size);
		break;
	default:
		pr_err("Invalid option: %i\n", args->op);
		r = -EINVAL;
	}

unlock_out:
	mutex_unlock(&target->mutex);

out:
	if (thread)
		put_task_struct(thread);

	if (mm)
		mmput(mm);

	if (pid)
		put_pid(pid);

	if (target)
		kcd_unref_process(target);

	return r;
}

static int kcd_ioctl_submit_queue(struct file *filep, struct kcd_process *p, void *data)
{
	struct kcd_ioctl_submit_queue_args *args = data;
	struct queue *q;
	struct kcd_process_device *pdd;
	struct device_queue_manager *dqm;
	struct kcd_node *dev;
	struct process_queue_manager *pqm = &p->pqm;
	int ret = 0;

	dqm = NULL;

	mutex_lock(&p->mutex);
	q = pqm_get_user_queue(pqm, args->queue_id);
	if (!q) {
		pr_err("Queue id does not match any known queue\n");
		ret = -EINVAL;
		goto err_unlock;
	}

	dev = q->device;
	if (WARN_ON(!dev)) {
		ret = -ENODEV;
		goto err_unlock;
	}

	pdd = kcd_get_process_device_data(dev, pqm->process);
	if (!pdd) {
		pr_err("Process device data doesn't exist\n");
		ret = -1;
		goto err_unlock;
	}

	dqm = q->device->dqm;
	if (dqm)
		ret = dqm->ops.submit_queue(dqm, &pdd->qpd, q);

	mutex_unlock(&p->mutex);
	return ret;

err_unlock:
	mutex_unlock(&p->mutex);
	return ret;
}

#define LGKCD_IOCTL_DEF(ioctl, _func, _flags) \
	[_IOC_NR(ioctl)] = {.cmd = ioctl, .func = _func, .flags = _flags, \
			    .cmd_drv = 0, .name = #ioctl}

/** Ioctl table */
static const struct lgkcd_ioctl_desc lgkcd_ioctls[] = {
	LGKCD_IOCTL_DEF(LGKCD_IOC_GET_VERSION,
			kcd_ioctl_get_version, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_CREATE_QUEUE,
			kcd_ioctl_create_queue, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_DESTROY_QUEUE,
			kcd_ioctl_destroy_queue, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_GET_CLOCK_COUNTERS,
			kcd_ioctl_get_clock_counters, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_GET_PROCESS_APERTURES,
			kcd_ioctl_get_process_apertures, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_UPDATE_QUEUE,
			kcd_ioctl_update_queue, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_CREATE_EVENT,
			kcd_ioctl_create_event, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_DESTROY_EVENT,
			kcd_ioctl_destroy_event, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_SET_EVENT,
			kcd_ioctl_set_event, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_RESET_EVENT,
			kcd_ioctl_reset_event, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_WAIT_EVENTS,
			kcd_ioctl_wait_events, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_SET_TRAP_HANDLER,
			kcd_ioctl_set_trap_handler, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_GET_PROCESS_APERTURES_NEW,
			kcd_ioctl_get_process_apertures_new, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_ACQUIRE_VM,
			kcd_ioctl_acquire_vm, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_ALLOC_MEMORY_OF_GPU,
			kcd_ioctl_alloc_memory_of_gpu, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_FREE_MEMORY_OF_GPU,
			kcd_ioctl_free_memory_of_gpu, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_MAP_MEMORY_TO_GPU,
			kcd_ioctl_map_memory_to_gpu, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_UNMAP_MEMORY_FROM_GPU,
			kcd_ioctl_unmap_memory_from_gpu, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_GET_DMABUF_INFO,
				kcd_ioctl_get_dmabuf_info, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_IMPORT_DMABUF,
				kcd_ioctl_import_dmabuf, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_SMI_EVENTS,
			kcd_ioctl_smi_events, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_SVM, kcd_ioctl_svm, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_AVAILABLE_MEMORY,
			kcd_ioctl_get_available_memory, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_EXPORT_DMABUF,
				kcd_ioctl_export_dmabuf, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_RUNTIME_ENABLE,
			kcd_ioctl_runtime_enable, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_DBG_TRAP,
			kcd_ioctl_set_debug_trap, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_SUBMIT_QUEUE,
			kcd_ioctl_submit_queue, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_IPC_IMPORT_HANDLE,
				kcd_ioctl_ipc_import_handle, 0),

	LGKCD_IOCTL_DEF(LGKCD_IOC_IPC_EXPORT_HANDLE,
				kcd_ioctl_ipc_export_handle, 0),
};

#define LGKCD_CORE_IOCTL_COUNT	ARRAY_SIZE(lgkcd_ioctls)

static long kcd_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct kcd_process *process;
	lgkcd_ioctl_t *func;
	const struct lgkcd_ioctl_desc *ioctl = NULL;
	unsigned int nr = _IOC_NR(cmd);
	char stack_kdata[128];
	char *kdata = NULL;
	unsigned int usize, asize;
	int retcode = -EINVAL;

	if (nr >= LGKCD_CORE_IOCTL_COUNT)
		goto err_i1;

	if (((nr >= LGKCD_COMMAND_START) && (nr < LGKCD_COMMAND_END))  ||
	    ((nr >= LGKCD_COMMAND_START_2) && (nr < LGKCD_COMMAND_END_2))) {
		u32 lgkcd_size;

		ioctl = &lgkcd_ioctls[nr];

		lgkcd_size = _IOC_SIZE(ioctl->cmd);
		usize = asize = _IOC_SIZE(cmd);
		if (lgkcd_size > asize)
			asize = lgkcd_size;

		cmd = ioctl->cmd;
	} else
		goto err_i1;

	dev_dbg(kcd_device, "ioctl cmd 0x%x (#0x%x), arg 0x%lx\n", cmd, nr, arg);

	/* Get the process struct from the filep. Only the process
	 * that opened /dev/kcd can use the file descriptor. Child
	 * processes need to create their own KCD device context.
	 */
	process = filep->private_data;

	if (process->lead_thread != current->group_leader) {
		dev_dbg(kcd_device, "Using KCD FD in wrong process\n");
		retcode = -EBADF;
		goto err_i1;
	}

	/* Do not trust userspace, use our own definition */
	func = ioctl->func;

	if (unlikely(!func)) {
		dev_dbg(kcd_device, "no function\n");
		retcode = -EINVAL;
		goto err_i1;
	}

	if (cmd & (IOC_IN | IOC_OUT)) {
		if (asize <= sizeof(stack_kdata)) {
			kdata = stack_kdata;
		} else {
			kdata = kmalloc(asize, GFP_KERNEL);
			if (!kdata) {
				retcode = -ENOMEM;
				goto err_i1;
			}
		}
		if (asize > usize)
			memset(kdata + usize, 0, asize - usize);
	}

	if (cmd & IOC_IN) {
		if (copy_from_user(kdata, (void __user *)arg, usize) != 0) {
			retcode = -EFAULT;
			goto err_i1;
		}
	} else if (cmd & IOC_OUT) {
		memset(kdata, 0, usize);
	}

	retcode = func(filep, process, kdata);

	if (cmd & IOC_OUT)
		if (copy_to_user((void __user *)arg, kdata, usize) != 0)
			retcode = -EFAULT;

err_i1:
	if (!ioctl)
		dev_dbg(kcd_device, "invalid ioctl: pid=%d, cmd=0x%02x, nr=0x%02x\n",
			  task_pid_nr(current), cmd, nr);

	if (kdata != stack_kdata)
		kfree(kdata);

	if (retcode)
		dev_dbg(kcd_device, "ioctl cmd (#0x%x), arg 0x%lx, ret = %d\n",
				nr, arg, retcode);

	return retcode;
}

static int kcd_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct kcd_process *process;
	struct kcd_node *dev = NULL;
	unsigned long mmap_offset;
	unsigned int gpu_id;

	process = kcd_get_process(current);
	if (IS_ERR(process))
		return PTR_ERR(process);

	mmap_offset = vma->vm_pgoff << PAGE_SHIFT;
	gpu_id = KCD_MMAP_GET_GPU_ID(mmap_offset);
	if (gpu_id)
		dev = kcd_device_by_id(gpu_id);

	switch (mmap_offset & KCD_MMAP_TYPE_MASK) {
	case KCD_MMAP_TYPE_DOORBELL:
		if (!dev || !dev->kcd->has_doorbells)
			return -ENODEV;
		return kcd_doorbell_mmap(dev, process, vma);
	case KCD_MMAP_TYPE_EVENTS:
		return kcd_event_mmap(process, vma);
	}

	return -EFAULT;
}
