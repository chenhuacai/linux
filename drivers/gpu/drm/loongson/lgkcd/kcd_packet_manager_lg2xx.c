// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright 2016-2022 Advanced Micro Devices, Inc.
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

#include "kcd_kernel_queue.h"
#include "kcd_device_queue_manager.h"
#include "kcd_pm_headers_lg2xx.h"

static int pm_map_process(struct packet_manager *pm,
		uint32_t *buffer, struct qcm_process_device *qpd)
{
	struct pm_map_process *packet;
	uint64_t vm_page_table_base_addr = qpd->page_table_base;
	struct kcd_process_device *pdd =
			container_of(qpd, struct kcd_process_device, qpd);

	packet = (struct pm_map_process *)buffer;
	memset(buffer, 0, sizeof(struct pm_map_process));

	/* TODO : Build packet */
	packet->cmd32 = LG2XX_SCMD32(LG2XX_SCMD32_OP_MAP_PROCESS, 0);
	packet->pasid = pdd->process->pasid;
	packet->pgd_base_addr_lo =
			lower_32_bits(vm_page_table_base_addr);
	packet->pgd_base_addr_hi =
			upper_32_bits(vm_page_table_base_addr);

	if (pdd->dev->adev->family_type < CHIP_LG210)
		return 0;

	packet->tba_lo = lower_32_bits(qpd->tba_addr);
	packet->tba_hi = upper_32_bits(qpd->tba_addr);
	packet->tma_lo = lower_32_bits(qpd->tma_addr);
	packet->tma_hi = upper_32_bits(qpd->tma_addr);
	packet->dbg_flags = pdd->process->debugger_process ? 0x1 : 0;

	return 0;
}

static int pm_map_queues(struct packet_manager *pm, uint32_t *buffer,
		struct queue *q, bool is_static)
{
	struct pm_map_queues *packet;

	packet = (struct pm_map_queues *)buffer;

	packet->cmd32 = LG2XX_SCMD32(LG2XX_SCMD32_OP_MAP_QUEUE, 0);
	packet->mqd = *((struct lg2xx_mqd *)q->mqd);

	return 0;
}

static int pm_unmap_queues(struct packet_manager *pm, uint32_t *buffer,
			enum kcd_unmap_queues_filter filter,
			uint32_t filter_param, bool reset)
{
	struct pm_unmap_queues *packet;

	packet = (struct pm_unmap_queues *)buffer;
	memset(buffer, 0, sizeof(struct pm_unmap_queues));

	packet->cmd32 = LG2XX_SCMD32(LG2XX_SCMD32_OP_UNMAP_QUEUE, 0);
	packet->filter_type = filter;

	switch (filter) {
	case KCD_UNMAP_QUEUES_FILTER_BY_PASID:
		packet->data = filter_param;
		break;
	case KCD_UNMAP_QUEUES_FILTER_ALL_QUEUES:
		break;
	case KCD_UNMAP_QUEUES_FILTER_DYNAMIC_QUEUES:
		break;
	default:
		WARN(1, "filter %d", filter);
		return -EINVAL;
	}

	return 0;

}

static int pm_query_status(struct packet_manager *pm, uint32_t *buffer,
			uint64_t fence_address,	uint64_t fence_value)
{
	struct pm_query_status *packet;

	packet = (struct pm_query_status *)buffer;
	memset(buffer, 0, sizeof(struct pm_query_status));

	packet->cmd32 = LG2XX_SCMD32(LG2XX_SCMD32_OP_WB64, 0);
	packet->addr_hi = upper_32_bits((uint64_t)fence_address);
	packet->addr_lo = lower_32_bits((uint64_t)fence_address);
	packet->data_hi = upper_32_bits((uint64_t)fence_value);
	packet->data_lo = lower_32_bits((uint64_t)fence_value);

	return 0;
}

static int pm_submit_one_queue(struct packet_manager *pm, uint32_t *buffer,
			uint32_t pasid,	uint32_t queue_id)
{
	struct pm_submit_queue *packet;

	packet = (struct pm_submit_queue *)buffer;
	memset(buffer, 0, sizeof(struct pm_submit_queue));

	packet->cmd32 = LG2XX_SCMD32(LG2XX_SCMD32_OP_DOORBELL, 0);
	packet->pasid = pasid;
	packet->queue_id = queue_id;

	return 0;
}

const struct packet_manager_funcs kcd_lg2xx_pm_funcs = {
	.map_process		= pm_map_process,
	.map_queues		= pm_map_queues,
	.unmap_queues		= pm_unmap_queues,
	.query_status		= pm_query_status,
	.submit_queue		= pm_submit_one_queue,
	.map_process_size	= sizeof(struct pm_map_process),
	.map_queues_size	= sizeof(struct pm_map_queues),
	.unmap_queues_size	= sizeof(struct pm_unmap_queues),
	.query_status_size	= sizeof(struct pm_query_status),
	.submit_queue_size	= sizeof(struct pm_submit_queue)
};
