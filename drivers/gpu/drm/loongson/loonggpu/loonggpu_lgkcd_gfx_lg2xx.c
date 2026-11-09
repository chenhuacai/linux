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

#include "loonggpu.h"
#include "loonggpu_lgkcd.h"
#include "../lgkcd/kcd_mqd_manager_lg2xx.h"

#define CPIPE_KERNEL (0x0 << 15)

static int kgd_hiq_mqd_load(struct loonggpu_device *adev, void *mqd,
			    uint32_t pipe_id, uint32_t queue_id)
{
	struct lg2xx_mqd *m = mqd;

	loonggpu_cmd_exec(adev,
		       LG2XX_ICMD32i(LG2XX_ICMD32_MOP_CPIPE,
			     LG2XX_ICMD32_SOP_CPIPE_BCQ, 0) |
			       CPIPE_KERNEL,
		       m->base_addr_lo, m->base_addr_hi);
	loonggpu_cmd_exec(adev,
		       LG2XX_ICMD32i(LG2XX_ICMD32_MOP_CPIPE,
			     LG2XX_ICMD32_SOP_CPIPE_CQSZ, 0),
		       m->length, 0);
	if (adev->family_type == CHIP_LG210) {
		loonggpu_cmd_exec(adev,
			LG2XX_ICMD32i(LG2XX_ICMD32_MOP_CPIPE,
				LG2XX_ICMD32_SOP_CPIPE_BCDB, 0),
			m->doorbell_offset, 0);
	}

	WREG32(LOONGGPU_LG2XX_CPIPE_CB_WPTR_OFFSET, 0);
	WREG32(LOONGGPU_LG2XX_CPIPE_CB_RPTR_OFFSET, 0);
	DRM_INFO("load kq base 0x%x%08x size = 0x%x\r\n", m->base_addr_hi,
		 m->base_addr_lo, m->length);

	return 0;
}

static int kgd_hiq_mqd_destroy(struct loonggpu_device *adev, void *mqd,
			       uint32_t pipe_id, uint32_t queue_id)
{
	struct lg2xx_mqd *m = mqd;

	loonggpu_cmd_exec(adev,
		       LG2XX_ICMD32i(LG2XX_ICMD32_MOP_CPIPE,
			     LG2XX_ICMD32_SOP_CPIPE_UBCQ, 0) |
			       CPIPE_KERNEL,
		       m->base_addr_lo, m->base_addr_hi);

	if (adev->family_type == CHIP_LG210) {
		loonggpu_cmd_exec(adev,
			LG2XX_ICMD32i(LG2XX_ICMD32_MOP_CPIPE,
				LG2XX_ICMD32_SOP_CPIPE_UBCDB, 0),
			0, 0);
	}

	WREG32(LOONGGPU_LG2XX_CPIPE_CB_WPTR_OFFSET, 0);
	WREG32(LOONGGPU_LG2XX_CPIPE_CB_RPTR_OFFSET, 0);

	return 0;
}

static uint32_t kgd_hiq_get_wptr(struct loonggpu_device *adev)
{
	return RREG32(LOONGGPU_LG2XX_CPIPE_CB_WPTR_OFFSET);
}

static uint32_t kgd_hiq_get_rptr(struct loonggpu_device *adev)
{
	return RREG32(LOONGGPU_LG2XX_CPIPE_CB_RPTR_OFFSET);
}

static void kgd_hiq_set_wptr(struct loonggpu_device *adev, uint32_t wptr_offset)
{
	WREG32(LOONGGPU_LG2XX_CPIPE_CB_WPTR_OFFSET, wptr_offset);
}

static void kgd_hiq_set_atomic_ops(struct loonggpu_device *adev, bool enable)
{
	if (adev->family_type >= CHIP_LG210) {
		loonggpu_cmd_exec(adev,
			LG2XX_ICMD32i(LG2XX_ICMD32_MOP_CPIPE,
			LG2XX_ICMD32_SOP_CPIPE_ATOMIC, 0) |
			CPIPE_KERNEL,
			enable, 0);

		DRM_INFO("set atomic_ops to %s \r\n",enable ? "enable" : "disable");
	}
}

const struct kcd2kgd_calls gfx_lg2xx_kcd2kgd = {
	.hiq_mqd_load = kgd_hiq_mqd_load,
	.hiq_mqd_destroy = kgd_hiq_mqd_destroy,
	.hiq_mqd_get_wptr = kgd_hiq_get_wptr,
	.hiq_mqd_get_rptr = kgd_hiq_get_rptr,
	.hiq_mqd_set_wptr = kgd_hiq_set_wptr,
	.hiq_mqd_set_atomic_ops = kgd_hiq_set_atomic_ops,
};
