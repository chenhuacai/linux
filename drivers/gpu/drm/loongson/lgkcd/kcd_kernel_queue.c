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

#include <linux/delay.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include "kcd_kernel_queue.h"
#include "kcd_priv.h"
#include "kcd_device_queue_manager.h"

static uint32_t kcd_kq_get_wptr(struct kernel_queue *kq)
{
	return kq->mqd_mgr->dev->kcd2kgd->hiq_mqd_get_wptr(kq->dev->adev);
}

static void kcd_kq_set_wptr(struct kernel_queue *kq, uint32_t wptr_offset)
{
	kq->mqd_mgr->dev->kcd2kgd->hiq_mqd_set_wptr(kq->dev->adev, wptr_offset);
}

static uint32_t kcd_kq_get_rptr(struct kernel_queue *kq)
{
	return kq->mqd_mgr->dev->kcd2kgd->hiq_mqd_get_rptr(kq->dev->adev);
}

static void kcd_kq_set_atomic_ops(struct kernel_queue *kq, bool enable)
{
	return kq->mqd_mgr->dev->kcd2kgd->hiq_mqd_set_atomic_ops(kq->dev->adev, enable);
}

/* Initialize a kernel queue, including allocations of GART memory
 * needed for the queue.
 */
static bool kq_initialize(struct kernel_queue *kq, struct kcd_node *dev,
		enum kcd_queue_type type, unsigned int queue_size)
{
	struct queue_properties prop;
	int retval;

	if (WARN_ON(type != KCD_QUEUE_TYPE_HIQ))
		return false;

	pr_debug("Initializing queue type %d size %d\n", KCD_QUEUE_TYPE_HIQ,
			queue_size);

	memset(&prop, 0, sizeof(prop));

	kq->dev = dev;
	kq->nop_packet = LG2XX_SCMD32_OP_NOP;
	switch (type) {
	case KCD_QUEUE_TYPE_HIQ:
		kq->mqd_mgr = dev->dqm->mqd_mgrs[KCD_MQD_TYPE_HIQ];
		break;
	default:
		pr_err("Invalid queue type %d\n", type);
		return false;
	}

	if (!kq->mqd_mgr)
		return false;

	if (dev->kcd->has_doorbells) {
		prop.doorbell_ptr = 
			kcd_get_kernel_doorbell(dev->kcd, &prop.doorbell_off);

		if (!prop.doorbell_ptr) {
			pr_err("Failed to initialize doorbell");
			goto err_get_kernel_doorbell;
		}
	}

	retval = kcd_gtt_sa_allocate(dev, queue_size, &kq->pq);
	if (retval != 0) {
		pr_err("Failed to init pq queues size %d\n", queue_size);
		goto err_pq_allocate_vidmem;
	}

	kq->pq_kernel_addr = kq->pq->cpu_ptr;
	kq->pq_gpu_addr = kq->pq->gpu_addr;

	memset(kq->pq_kernel_addr, 0, queue_size);

	prop.queue_size = queue_size;
	prop.is_interop = false;
	prop.priority = 1;
	prop.queue_percent = 100;
	prop.type = type;
	prop.vmid = 0;
	prop.queue_address = kq->pq_gpu_addr;

	if (init_queue(&kq->queue, &prop) != 0)
		goto err_init_queue;

	kq->queue->device = dev;
	kq->queue->process = NULL;

	kq->queue->mqd_mem_obj = kq->mqd_mgr->allocate_mqd(kq->mqd_mgr->dev,
					&kq->queue->properties);
	if (!kq->queue->mqd_mem_obj)
		goto err_allocate_mqd;
	kq->mqd_mgr->init_mqd(kq->mqd_mgr, &kq->queue->mqd,
					kq->queue->mqd_mem_obj,
					&kq->queue->gart_mqd_addr,
					&kq->queue->properties,
					0);
	/* assign HIQ to HQD */
	if (type == KCD_QUEUE_TYPE_HIQ) {
		pr_debug("Assigning hiq to hqd\n");
		kq->queue->pipe = KCD_CIK_HIQ_PIPE;
		kq->queue->queue = KCD_CIK_HIQ_QUEUE;
		kq->mqd_mgr->load_mqd(kq->mqd_mgr, kq->queue->mqd,
				kq->queue->pipe, kq->queue->queue,
				&kq->queue->properties, NULL);
	}

	kcd_kq_set_atomic_ops(kq, dev->kcd->pci_atomic_requested);
	print_queue(kq->queue);

	return true;
err_allocate_mqd:
	uninit_queue(kq->queue);
err_init_queue:
	kcd_gtt_sa_free(dev, kq->pq);
err_pq_allocate_vidmem:
	kcd_release_kernel_doorbell(dev->kcd, prop.doorbell_ptr);
err_get_kernel_doorbell:
	return false;

}

/* Uninitialize a kernel queue and free all its memory usages. */
static void kq_uninitialize(struct kernel_queue *kq, bool hanging)
{
	if (kq->queue->properties.type == KCD_QUEUE_TYPE_HIQ && !hanging)
		kq->mqd_mgr->destroy_mqd(kq->mqd_mgr,
					kq->queue->mqd,
					KCD_PREEMPT_TYPE_WAVEFRONT_RESET,
					KCD_UNMAP_LATENCY_MS,
					kq->queue->pipe,
					kq->queue->queue);

	kq->mqd_mgr->free_mqd(kq->mqd_mgr, kq->queue->mqd,
				kq->queue->mqd_mem_obj);

	kcd_gtt_sa_free(kq->dev, kq->pq);
	if (kq->dev->kcd->has_doorbells)
		kcd_release_kernel_doorbell(kq->dev->kcd,
						kq->queue->properties.doorbell_ptr);
	uninit_queue(kq->queue);
}

int kq_acquire_packet_buffer(struct kernel_queue *kq,
		size_t packet_size_in_dwords, unsigned int **buffer_ptr)
{
	size_t available_size;
	size_t queue_size_dwords;
	uint32_t wptr, rptr;
	unsigned int *queue_address;
	int timeout = 10 * 100; /* 10 seconds */

	/* When rptr == wptr, the buffer is empty.
	 * When rptr == wptr + 1, the buffer is full.
	 * It is always rptr that advances to the position of wptr, rather than
	 * the opposite. So we can only use up to queue_size_dwords - 1 dwords.
	 */
	rptr = kcd_kq_get_rptr(kq);
	wptr = kq->pending_wptr;
	queue_address = (unsigned int *)kq->pq_kernel_addr;
	queue_size_dwords = kq->queue->properties.queue_size / 4;

	pr_debug("rptr: %d\n", rptr);
	pr_debug("wptr: %d\n", wptr);
	pr_debug("queue_address 0x%p\n", queue_address);

	available_size = (rptr + queue_size_dwords - 1 - wptr) %
							queue_size_dwords;

	if (packet_size_in_dwords > available_size) {
		/*
		 * make sure calling functions know
		 * acquire_packet_buffer() failed
		 */
		goto err_no_space;
	}

	if (wptr + packet_size_in_dwords >= queue_size_dwords) {
		/* fill nops, roll back and start at position 0 */
		while (wptr > 0) {
			queue_address[wptr] = kq->nop_packet;
			wptr = (wptr + 1) % queue_size_dwords;
		}

		/* make sure after rolling back to position 0, there is
		 * still enough space.
		 */
		if (packet_size_in_dwords >= rptr) {
			kq->pending_wptr = wptr;
			kq_submit_packet(kq);
			while (1) {
				rptr = kcd_kq_get_rptr(kq);
				if (packet_size_in_dwords < rptr || rptr == wptr)
					break;

				msleep(10);
				if (timeout-- <= 0) {
					pr_err("timeout to acquire packet buffer\n");
					goto err_no_space;
				}
			}
		}
	}

	*buffer_ptr = &queue_address[wptr];
	kq->pending_wptr = wptr + packet_size_in_dwords;

	return 0;

err_no_space:
	*buffer_ptr = NULL;
	return -ENOMEM;
}

void kq_submit_packet(struct kernel_queue *kq)
{
#ifdef DEBUG
	int i;
	int ks = kq->queue->properties.queue_size / 4;
	pr_info("pkg_start %d %d\n", kcd_kq_get_wptr(kq), kq->pending_wptr);
	for (i = kcd_kq_get_wptr(kq);
	     ((i < kq->pending_wptr) || (i > kq->pending_wptr && i <= ks));
	     i++) {
		if (i >= ks)
			i = 0;
		pr_info("0x%08X %d", kq->pq_kernel_addr[i], i);
	}
	pr_info("pkg_end\n");
#endif
	kcd_kq_set_wptr(kq, kq->pending_wptr);
	if (kq->dev->kcd->has_doorbells)
		write_kernel_doorbell64(kq->queue->properties.doorbell_ptr,
					kq->pending_wptr);
}

void kq_rollback_packet(struct kernel_queue *kq)
{
	kq->pending_wptr = kcd_kq_get_wptr(kq);
}

struct kernel_queue *kernel_queue_init(struct kcd_node *dev,
					enum kcd_queue_type type)
{
	struct kernel_queue *kq;

	kq = kzalloc(sizeof(*kq), GFP_KERNEL);
	if (!kq)
		return NULL;

	if (kq_initialize(kq, dev, type, KCD_KERNEL_QUEUE_SIZE))
		return kq;

	pr_err("Failed to init kernel queue\n");

	kfree(kq);
	return NULL;
}

void kernel_queue_uninit(struct kernel_queue *kq, bool hanging)
{
	kq_uninitialize(kq, hanging);
	kfree(kq);
}

/* FIXME: Can this test be removed? */
static __attribute__((unused)) void test_kq(struct kcd_node *dev)
{
	struct kernel_queue *kq;
	uint32_t *buffer, i;
	int retval;

	pr_err("Starting kernel queue test\n");

	kq = kernel_queue_init(dev, KCD_QUEUE_TYPE_HIQ);
	if (unlikely(!kq)) {
		pr_err("  Failed to initialize HIQ\n");
		pr_err("Kernel queue test failed\n");
		return;
	}

	retval = kq_acquire_packet_buffer(kq, 5, &buffer);
	if (unlikely(retval != 0)) {
		pr_err("  Failed to acquire packet buffer\n");
		pr_err("Kernel queue test failed\n");
		return;
	}
	for (i = 0; i < 5; i++)
		buffer[i] = kq->nop_packet;
	kq_submit_packet(kq);

	pr_err("Ending kernel queue test\n");
}


