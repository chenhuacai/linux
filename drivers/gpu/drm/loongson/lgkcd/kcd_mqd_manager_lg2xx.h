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

#ifndef KCD_MQD_MANAGER_LG2XX_H_
#define KCD_MQD_MANAGER_LG2XX_H_

struct lg2xx_mqd {
	uint32_t base_addr_lo;
	uint32_t base_addr_hi;
	uint32_t rd_ptr_lo;
	uint32_t rd_ptr_hi;
	uint32_t wr_ptr_lo;
	uint32_t wr_ptr_hi;
	uint32_t length;
	uint32_t pasid;
	uint32_t queue_id;
	uint32_t queue_type;
	uint32_t doorbell_offset;
	uint32_t cwsr_lo;
	uint32_t cwsr_hi;
	uint32_t cwsr_size;
};

#endif /* KCD_MQD_MANAGER_LG2XX_H_ */
