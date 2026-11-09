/* SPDX-License-Identifier: GPL-2.0 OR MIT */
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

#ifndef KCD_MQD_MANAGER_H_
#define KCD_MQD_MANAGER_H_

#include "kcd_priv.h"

#define KCD_MAX_NUM_SE 8
#define KCD_MAX_NUM_SH_PER_SE 2

/**
 * struct mqd_manager
 *
 * @init_mqd: Allocates the mqd buffer on local gpu memory and initialize it.
 *
 * @load_mqd: Loads the mqd to a concrete hqd slot. Used only for no cp
 * scheduling mode.
 *
 * @update_mqd: Handles a update call for the MQD
 *
 * @destroy_mqd: Destroys the HQD slot and by that preempt the relevant queue.
 * Used only for no cp scheduling.
 *
 * @free_mqd: Releases the mqd buffer from local gpu memory.
 *
 * @mqd_mutex: Mqd manager mutex.
 *
 * @dev: The kcd device structure coupled with this module.
 *
 * MQD stands for Memory Queue Descriptor which represents the current queue
 * state in the memory and initiate the HQD (Hardware Queue Descriptor) state.
 * This structure is actually a base class for the different types of MQDs
 * structures for the variant ASICs that should be supported in the future.
 * This base class is also contains all the MQD specific operations.
 * Another important thing to mention is that each queue has a MQD that keeps
 * his state (or context) after each preemption or reassignment.
 * Basically there are a instances of the mqd manager class per MQD type per
 * ASIC. Currently the kcd driver supports only Kaveri so there are instances
 * per KCD_MQD_TYPE for each device.
 *
 */
extern int pipe_priority_map[];
struct mqd_manager {
	struct kcd_mem_obj*	(*allocate_mqd)(struct kcd_node *kcd,
		struct queue_properties *q);

	void	(*init_mqd)(struct mqd_manager *mm, void **mqd,
			struct kcd_mem_obj *mqd_mem_obj, uint64_t *gart_addr,
			struct queue_properties *q,
			uint32_t pasid);

	int	(*load_mqd)(struct mqd_manager *mm, void *mqd,
				uint32_t pipe_id, uint32_t queue_id,
				struct queue_properties *p,
				struct mm_struct *mms);

	void	(*update_mqd)(struct mqd_manager *mm, void *mqd,
				struct queue_properties *q);

	int	(*destroy_mqd)(struct mqd_manager *mm, void *mqd,
				enum kcd_preempt_type type,
				unsigned int timeout, uint32_t pipe_id,
				uint32_t queue_id);

	void	(*free_mqd)(struct mqd_manager *mm, void *mqd,
				struct kcd_mem_obj *mqd_mem_obj);

	void	(*restore_mqd)(struct mqd_manager *mm, void **mqd,
				struct kcd_mem_obj *mqd_mem_obj, uint64_t *gart_addr,
				struct queue_properties *p,
				const void *mqd_src);

#if defined(CONFIG_DEBUG_FS)
	int	(*debugfs_show_mqd)(struct seq_file *m, void *data);
#endif
	uint32_t (*read_doorbell_id)(void *mqd);
	uint64_t (*mqd_stride)(struct mqd_manager *mm,
				struct queue_properties *p);

	struct mutex	mqd_mutex;
	struct kcd_node	*dev;
	uint32_t mqd_size;
};

struct kcd_mem_obj *allocate_hiq_mqd(struct kcd_node *dev,
				struct queue_properties *q);

struct kcd_mem_obj *allocate_sdma_mqd(struct kcd_node *dev,
					struct queue_properties *q);
void free_mqd_hiq_sdma(struct mqd_manager *mm, void *mqd,
				struct kcd_mem_obj *mqd_mem_obj);

int kcd_hiq_load_mqd_kiq(struct mqd_manager *mm, void *mqd,
		uint32_t pipe_id, uint32_t queue_id,
		struct queue_properties *p, struct mm_struct *mms);

int kcd_hiq_destroy_mqd_kiq(struct mqd_manager *mm, void *mqd,
			enum kcd_preempt_type type, unsigned int timeout,
			uint32_t pipe_id, uint32_t queue_id);

void kcd_free_mqd_cp(struct mqd_manager *mm, void *mqd,
		struct kcd_mem_obj *mqd_mem_obj);

int kcd_load_mqd_sdma(struct mqd_manager *mm, void *mqd,
		uint32_t pipe_id, uint32_t queue_id,
		struct queue_properties *p, struct mm_struct *mms);

int kcd_destroy_mqd_sdma(struct mqd_manager *mm, void *mqd,
		enum kcd_preempt_type type, unsigned int timeout,
		uint32_t pipe_id, uint32_t queue_id);

void kcd_get_hiq_xcc_mqd(struct kcd_node *dev,
		struct kcd_mem_obj *mqd_mem_obj, uint32_t virtual_xcc_id);

uint64_t kcd_hiq_mqd_stride(struct kcd_node *dev);
uint64_t kcd_mqd_stride(struct mqd_manager *mm,
			struct queue_properties *q);
#endif /* KCD_MQD_MANAGER_H_ */
