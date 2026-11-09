/*
 * Copyright 2014 Advanced Micro Devices, Inc.
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

/*
 * This file defines the private interface between the
 * LOONGSON kernel graphics drivers and the LOONGSON KCD.
 */

#ifndef KGD_KCD_INTERFACE_H_INCLUDED
#define KGD_KCD_INTERFACE_H_INCLUDED

#include <linux/types.h>
#include <linux/bitmap.h>
#include <linux/dma-fence.h>
#include "loonggpu_irq.h"
#include "loonggpu_gfx.h"

struct pci_dev;
struct loonggpu_device;

struct kcd_dev;
struct kgd_mem;

enum kcd_preempt_type {
	KCD_PREEMPT_TYPE_WAVEFRONT_DRAIN = 0,
	KCD_PREEMPT_TYPE_WAVEFRONT_RESET,
	KCD_PREEMPT_TYPE_WAVEFRONT_SAVE
};

struct kcd_vm_fault_info {
	uint64_t	page_addr;
	uint32_t	vmid;
	uint32_t	mc_id;
	uint32_t	status;
	bool		prot_valid;
	bool		prot_read;
	bool		prot_write;
	bool		prot_exec;
};

struct kcd_cu_info {
	uint32_t num_shader_engines;
	uint32_t num_shader_arrays_per_engine;
	uint32_t num_cu_per_sh;
	uint32_t cu_active_number;
	uint32_t cu_ao_mask;
	uint32_t simd_per_cu;
	uint32_t max_waves_per_simd;
	uint32_t wave_front_size;
	uint32_t max_scratch_slots_per_cu;
	uint32_t lds_size;
	uint32_t cu_bitmap[4][4];
};

/* For getting GPU local memory information from KGD */
struct kcd_local_mem_info {
	uint64_t local_mem_size_private;
	uint64_t local_mem_size_public;
	uint32_t vram_width;
	uint32_t mem_clk_max;
};

enum kgd_memory_pool {
	KGD_POOL_SYSTEM_CACHEABLE = 1,
	KGD_POOL_SYSTEM_WRITECOMBINE = 2,
	KGD_POOL_FRAMEBUFFER = 3,
};

/**
 * enum kcd_sched_policy
 *
 * @KCD_SCHED_POLICY_HWS: H/W scheduling policy known as command processor (cp)
 * scheduling. In this scheduling mode we're using the firmware code to
 * schedule the user mode queues and kernel queues such as HIQ.
 * the HIQ queue is used as a special queue that dispatches the configuration
 * to the cp and the user mode queues list that are currently running.
 * in this scheduling mode user mode queues over subscription feature is
 * enabled.
 *
 * @KCD_SCHED_POLICY_HWS_NO_OVERSUBSCRIPTION: The same as above but the over
 * subscription feature disabled.
 *
 */
enum kcd_sched_policy {
	KCD_SCHED_POLICY_HWS = 0,
	KCD_SCHED_POLICY_HWS_NO_OVERSUBSCRIPTION
};

#define KGD_MAX_QUEUES 128
struct kgd2kcd_shared_resources {
	/* Bit n == 1 means VMID n is available for KCD. */
	unsigned int compute_vmid_bitmap;

	/* number of pipes per mec */
	uint32_t num_pipe_per_mec;

	/* number of queues per pipe */
	uint32_t num_queue_per_pipe;

	/* Bit n == 1 means Queue n is available for KCD */
	DECLARE_BITMAP(cp_queue_bitmap, KGD_MAX_QUEUES);

	/* SDMA doorbell assignments (SOC15 and later chips only). Only
	 * specific doorbells are routed to each SDMA engine. Others
	 * are routed to IH and VCN. They are not usable by the CP.
	 */
	uint32_t *sdma_doorbell_idx;

	/* From SOC15 onward, the doorbell index range not usable for CP
	 * queues.
	 */
	uint32_t non_cp_doorbells_start;
	uint32_t non_cp_doorbells_end;

	/* Base address of doorbell aperture. */
	phys_addr_t doorbell_physical_address;

	/* Size in bytes of doorbell aperture. */
	size_t doorbell_aperture_size;

	/* Number of bytes at start of aperture reserved for KGD. */
	size_t doorbell_start_offset;

	/* GPUVM address space size in bytes */
	uint64_t gpuvm_size;

	/* Minor device number of the render node */
	int drm_render_minor;
};

#define KCD_MAX_NUM_OF_QUEUES_PER_DEVICE_DEFAULT 4096

/**
 * struct kcd2kgd_calls
 */
struct kcd2kgd_calls {
	/* Register access functions */

	int (*hiq_mqd_load)(struct loonggpu_device *adev, void *mqd,
			    uint32_t pipe_id, uint32_t queue_id);
	int (*hiq_mqd_destroy)(struct loonggpu_device *adev, void *mqd,
				uint32_t pipe_id, uint32_t queue_id);
	uint32_t (*hiq_mqd_get_wptr)(struct loonggpu_device *adev);
	uint32_t (*hiq_mqd_get_rptr)(struct loonggpu_device *adev);
	void (*hiq_mqd_set_wptr)(struct loonggpu_device *adev, uint32_t wptr_offset);
	void (*hiq_mqd_set_atomic_ops)(struct loonggpu_device *adev, bool enable);
};

#endif	/* KGD_KCD_INTERFACE_H_INCLUDED */
