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

#include "kcd_priv.h"
#include "kcd_events.h"
#include "loonggpu_lgkcd.h"
#include "kcd_smi_events.h"
#include "kcd_debug.h"

struct lg2xx_ih_ring_entry {
	union {
		struct {
			uint32_t source_id : 8;
			uint32_t pasid : 16;
			uint32_t reversed : 4;
			uint32_t vmid : 4;
		} bitfields0;
		uint32_t data0;
	};

	uint32_t data1; /* reserved */
	uint32_t data2; /* event id */
	uint32_t data3;
};

static bool lg2xx_event_interrupt_isr(struct kcd_node *dev,
					const uint32_t *ih_ring_entry,
					uint32_t *patched_ihre,
					bool *patched_flag)
{
	const struct lg2xx_ih_ring_entry *ihre =
			(const struct lg2xx_ih_ring_entry *)ih_ring_entry;

	return ihre->bitfields0.source_id == LOONGGPU_LG200_SRCID_CPIPE ||
	       ihre->bitfields0.source_id == LOONGGPU_LG200_SRCID_SQ_INTERRUPTMSG ||
	       ihre->bitfields0.source_id == KCD_IRQ_FENCE_SOURCEID ||
	       (!dev->kcd->noretry && ihre->bitfields0.source_id == LOONGGPU_LG200_SRCID_MMU_PAGE_FAULT);
}

static void lg2xx_event_interrupt_wq(struct kcd_node *dev,
					const uint32_t *ih_ring_entry)
{
	const struct lg2xx_ih_ring_entry *ihre =
			(const struct lg2xx_ih_ring_entry *)ih_ring_entry;
	uint32_t context_id = ihre->data2 & 0xfffffff;
	uint32_t pasid = ihre->bitfields0.pasid;
	uint32_t vmid = ihre->bitfields0.vmid;
	uint32_t queue_id, trap_mask;

	if (ihre->bitfields0.source_id == LOONGGPU_LG200_SRCID_CPIPE)
		kcd_signal_event_interrupt(pasid, context_id, 28);
	else if (ihre->bitfields0.source_id == LOONGGPU_LG200_SRCID_MMU_PAGE_FAULT) {
		loonggpu_lgkcd_gpuvm_fault(dev->adev, pasid, vmid, 0,
			(uint64_t)(ihre->data3 & 0xffff) << 32 | ihre->data2,
			ihre->data3 >> 28);
	} else if (ihre->bitfields0.source_id == LOONGGPU_LG200_SRCID_SQ_INTERRUPTMSG) {
		queue_id = ihre->data2 & 0xffff;
		trap_mask = ihre->data3;
		kcd_set_dbg_ev_from_interrupt(dev, pasid, queue_id, trap_mask, NULL, 0);
	} else if (KCD_IRQ_IS_FENCE(KCD_IRQ_FENCE_CLIENTID, ihre->bitfields0.source_id))
		kcd_process_close_interrupt_drain(ihre->data3);
}

const struct kcd_event_interrupt_class event_interrupt_class_lg2xx = {
	.interrupt_isr = lg2xx_event_interrupt_isr,
	.interrupt_wq = lg2xx_event_interrupt_wq,
};
