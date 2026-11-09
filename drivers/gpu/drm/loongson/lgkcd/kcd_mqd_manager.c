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

#include "kcd_mqd_manager.h"
#include "loonggpu_lgkcd.h"
#include "kcd_device_queue_manager.h"

/* Mapping queue priority to pipe priority, indexed by queue priority */
int pipe_priority_map[] = {
	KCD_PIPE_PRIORITY_CS_LOW,
	KCD_PIPE_PRIORITY_CS_LOW,
	KCD_PIPE_PRIORITY_CS_LOW,
	KCD_PIPE_PRIORITY_CS_LOW,
	KCD_PIPE_PRIORITY_CS_LOW,
	KCD_PIPE_PRIORITY_CS_LOW,
	KCD_PIPE_PRIORITY_CS_LOW,
	KCD_PIPE_PRIORITY_CS_MEDIUM,
	KCD_PIPE_PRIORITY_CS_MEDIUM,
	KCD_PIPE_PRIORITY_CS_MEDIUM,
	KCD_PIPE_PRIORITY_CS_MEDIUM,
	KCD_PIPE_PRIORITY_CS_HIGH,
	KCD_PIPE_PRIORITY_CS_HIGH,
	KCD_PIPE_PRIORITY_CS_HIGH,
	KCD_PIPE_PRIORITY_CS_HIGH,
	KCD_PIPE_PRIORITY_CS_HIGH
};

struct kcd_mem_obj *allocate_hiq_mqd(struct kcd_node *dev, struct queue_properties *q)
{
	struct kcd_mem_obj *mqd_mem_obj;

	mqd_mem_obj = kzalloc(sizeof(struct kcd_mem_obj), GFP_KERNEL);
	if (!mqd_mem_obj)
		return NULL;

	mqd_mem_obj->gtt_mem = dev->dqm->hiq_sdma_mqd.gtt_mem;
	mqd_mem_obj->gpu_addr = dev->dqm->hiq_sdma_mqd.gpu_addr;
	mqd_mem_obj->cpu_ptr = dev->dqm->hiq_sdma_mqd.cpu_ptr;

	return mqd_mem_obj;
}

struct kcd_mem_obj *allocate_sdma_mqd(struct kcd_node *dev,
					struct queue_properties *q)
{
	struct kcd_mem_obj *mqd_mem_obj;
	uint64_t offset;

	mqd_mem_obj = kzalloc(sizeof(struct kcd_mem_obj), GFP_KERNEL);
	if (!mqd_mem_obj)
		return NULL;

	offset = (q->sdma_engine_id *
		dev->kcd->device_info.num_sdma_queues_per_engine +
		q->sdma_queue_id) *
		dev->dqm->mqd_mgrs[KCD_MQD_TYPE_SDMA]->mqd_size;

	offset += dev->dqm->mqd_mgrs[KCD_MQD_TYPE_HIQ]->mqd_size;

	mqd_mem_obj->gtt_mem = (void *)((uint64_t)dev->dqm->hiq_sdma_mqd.gtt_mem
				+ offset);
	mqd_mem_obj->gpu_addr = dev->dqm->hiq_sdma_mqd.gpu_addr + offset;
	mqd_mem_obj->cpu_ptr = (uint32_t *)((uint64_t)
				dev->dqm->hiq_sdma_mqd.cpu_ptr + offset);

	return mqd_mem_obj;
}

void free_mqd_hiq_sdma(struct mqd_manager *mm, void *mqd,
			struct kcd_mem_obj *mqd_mem_obj)
{
	WARN_ON(!mqd_mem_obj->gtt_mem);
	kfree(mqd_mem_obj);
}

int kcd_hiq_load_mqd_kiq(struct mqd_manager *mm, void *mqd,
		     uint32_t pipe_id, uint32_t queue_id,
		     struct queue_properties *p, struct mm_struct *mms)
{
	return mm->dev->kcd2kgd->hiq_mqd_load(mm->dev->adev, mqd, pipe_id,
					      queue_id);
}

int kcd_hiq_destroy_mqd_kiq(struct mqd_manager *mm, void *mqd,
			enum kcd_preempt_type type, unsigned int timeout,
			uint32_t pipe_id, uint32_t queue_id)
{
	return mm->dev->kcd2kgd->hiq_mqd_destroy(mm->dev->adev, mqd, pipe_id,
					      queue_id);
}

void kcd_free_mqd_cp(struct mqd_manager *mm, void *mqd,
	      struct kcd_mem_obj *mqd_mem_obj)
{
	if (mqd_mem_obj->gtt_mem) {
		loonggpu_lgkcd_free_gtt_mem(mm->dev->adev, mqd_mem_obj->gtt_mem);
		kfree(mqd_mem_obj);
	} else {
		kcd_gtt_sa_free(mm->dev, mqd_mem_obj);
	}
}

uint64_t kcd_hiq_mqd_stride(struct kcd_node *dev)
{
	return dev->dqm->mqd_mgrs[KCD_MQD_TYPE_HIQ]->mqd_size;
}

void kcd_get_hiq_xcc_mqd(struct kcd_node *dev, struct kcd_mem_obj *mqd_mem_obj,
		     uint32_t virtual_xcc_id)
{
	uint64_t offset;

	offset = kcd_hiq_mqd_stride(dev) * virtual_xcc_id;

	mqd_mem_obj->gtt_mem = (virtual_xcc_id == 0) ?
			dev->dqm->hiq_sdma_mqd.gtt_mem : NULL;
	mqd_mem_obj->gpu_addr = dev->dqm->hiq_sdma_mqd.gpu_addr + offset;
	mqd_mem_obj->cpu_ptr = (uint32_t *)((uintptr_t)
				dev->dqm->hiq_sdma_mqd.cpu_ptr + offset);
}

uint64_t kcd_mqd_stride(struct mqd_manager *mm,
			struct queue_properties *q)
{
	return mm->mqd_size;
}
