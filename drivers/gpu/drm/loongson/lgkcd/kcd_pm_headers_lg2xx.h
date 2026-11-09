/* SPDX-License-Identifier: GPL-2.0 OR MIT */
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

#ifndef PM_PACKETS_LG2XX_H
#define PM_PACKETS_LG2XX_H
#include "kcd_mqd_manager_lg2xx.h"

struct pm_map_process {
	uint32_t	cmd32;
	uint32_t	pasid;
	uint32_t	reserved;
	uint32_t pgd_base_addr_lo;
	uint32_t pgd_base_addr_hi;
	uint32_t tba_lo;
	uint32_t tba_hi;
	uint32_t tma_lo;
	uint32_t tma_hi;
	uint32_t dbg_flags;
};

struct pm_map_queues {
	uint32_t cmd32;
	struct lg2xx_mqd mqd;
};

struct pm_query_status {
	uint32_t	cmd32;
	uint32_t	addr_lo;
	uint32_t	addr_hi;
	uint32_t	data_lo;
	uint32_t	data_hi;
};

struct pm_unmap_queues {
	uint32_t	cmd32;
	uint32_t	filter_type;
	uint32_t	data;
};

struct pm_submit_queue {
	uint32_t	cmd32;
	uint32_t	pasid;
	uint32_t	queue_id;
};
#endif

