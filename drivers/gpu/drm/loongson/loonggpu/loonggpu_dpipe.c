#include <linux/kernel.h>
#include "loonggpu.h"
#include "loonggpu_common.h"
#include "loonggpu_cp.h"
#include "loonggpu_pipe.h"

static void dpipe_set_ring_funcs(struct loonggpu_device *adev);
static void dpipe_set_irq_funcs(struct loonggpu_device *adev);

static uint32_t dpipe_cb_wptr_offset = 0;
static uint32_t dpipe_cb_rptr_offset = 0;

static int dpipe_sw_init(void *handle)
{
	int r;
	struct loonggpu_ring *ring;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	/* FENCE Event */
	r = loonggpu_irq_add_id(adev, LOONGGPU_IH_CLIENTID_LEGACY, LOONGGPU_LG210_SRCID_CP_END_OF_DPIPE, &adev->dpipe.eop_irq);
	if (r)
		return r;

	/* set up the dpipe ring */
	ring = &adev->dpipe.ring;
	ring->ring_obj = NULL;
	sprintf(ring->name, "dpipe");

	r = loonggpu_ring_init(adev, ring, 256, &adev->dpipe.eop_irq,
				LOONGGPU_CP_IRQ_DPIPE_EOP);
	if (r)
		return r;

	return 0;
}

static int dpipe_sw_fini(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	loonggpu_ring_fini(&adev->dpipe.ring);
	return 0;
}

static int dpipe_cp_dpipe_resume(struct loonggpu_device *adev)
{
	struct loonggpu_ring *ring;

        dpipe_cb_wptr_offset = LOONGGPU_LG2XX_DPIPE_CB_WPTR_OFFSET;
        dpipe_cb_rptr_offset = LOONGGPU_LG2XX_DPIPE_CB_RPTR_OFFSET;

	/* Set ring buffer size */
	ring = &adev->dpipe.ring;

	/* start the ring */
	loonggpu_ring_clear_ring(ring);

	/* Initialize the ring buffer's read and write pointers */
	ring->wptr = 0;
	WREG32(dpipe_cb_wptr_offset, lower_32_bits(ring->wptr));

	/* set the RPTR */
	WREG32(dpipe_cb_rptr_offset, 0);

	mdelay(1);

	loonggpu_cmd_exec(adev, LG2XX_ICMD32i(LG2XX_ICMD32_MOP_DPIPE,
			LG2XX_ICMD32_SOP_DPIPE_BBQ, 0),
			lower_32_bits(ring->gpu_addr), upper_32_bits(ring->gpu_addr));
        loonggpu_cmd_exec(adev, LG2XX_ICMD32i(LG2XX_ICMD32_MOP_DPIPE,
			LG2XX_ICMD32_SOP_DPIPE_BQSZ, 0),
                        ring->ring_size / 4, 0);

	ring->ready = true;
	return 0;
}

static int dpipe_cp_resume(struct loonggpu_device *adev)
{
	int r;

	r = dpipe_cp_dpipe_resume(adev);
	if (r)
		return r;

	return 0;
}

static int dpipe_hw_init(void *handle)
{
	int r;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	r = dpipe_cp_resume(adev);

	return r;
}

static int dpipe_hw_fini(void *handle)
{
	return 0;
}

static int dpipe_suspend(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	return dpipe_hw_fini(adev);
}

static int dpipe_resume(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	return dpipe_hw_init(adev);
}

static bool dpipe_is_idle(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	return (RREG32(LOONGGPU_STATUS) == GSCMD_STS_DONE);
}

static int dpipe_wait_for_idle(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	if (loonggpu_cp_wait_done(adev) == true)
			return 0;

	return -ETIMEDOUT;
}

static int dpipe_early_init(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	dpipe_set_ring_funcs(adev);
	dpipe_set_irq_funcs(adev);

	return 0;
}

static int dpipe_late_init(void *handle)
{
	return 0;
}

static u64 dpipe_ring_get_rptr(struct loonggpu_ring *ring)
{
	struct loonggpu_device *adev = ring->adev;

	return RREG32(dpipe_cb_rptr_offset);
}

static u64 dpipe_ring_get_wptr(struct loonggpu_ring *ring)
{
	struct loonggpu_device *adev = ring->adev;

	return RREG32(dpipe_cb_wptr_offset);
}

static void dpipe_ring_set_wptr(struct loonggpu_ring *ring)
{
	struct loonggpu_device *adev = ring->adev;

	WREG32(dpipe_cb_wptr_offset, lower_32_bits(ring->wptr));
}

static int dpipe_set_eop_interrupt_state(struct loonggpu_device *adev,
					    struct loonggpu_irq_src *src,
					    unsigned type,
					    enum loonggpu_interrupt_state state)
{
	return 0;
}

static int dpipe_eop_irq(struct loonggpu_device *adev,
			    struct loonggpu_irq_src *source,
			    struct loonggpu_iv_entry *entry)
{
	u8 me_id;

	DRM_DEBUG("IH: DPIPE FENCE\n");
	me_id = (entry->ring_id & 0x0c) >> 2;

	switch (me_id) {
	case 0:
		loonggpu_fence_process(&adev->dpipe.ring);
		break;
	default:
		DRM_ERROR("loonggpu dpipe number fail 0x%x",me_id);
		break;
	}
	return 0;
}

static const struct loonggpu_ip_funcs dpipe_ip_funcs = {
	.name = "dpipe",
	.early_init = dpipe_early_init,
	.late_init = dpipe_late_init,
	.sw_init = dpipe_sw_init,
	.sw_fini = dpipe_sw_fini,
	.hw_init = dpipe_hw_init,
	.hw_fini = dpipe_hw_fini,
	.suspend = dpipe_suspend,
	.resume = dpipe_resume,
	.is_idle = dpipe_is_idle,
	.wait_for_idle = dpipe_wait_for_idle,
};

static struct loonggpu_ring_funcs dpipe_ring_funcs = {
	.type = LOONGGPU_RING_TYPE_DPIPE,
	.align_mask = 0xf,
	.nop = LG2XX_SCMD32(LG2XX_SCMD32_OP_NOP, 0),
	.support_64bit_ptrs = false,
	.get_rptr = dpipe_ring_get_rptr,
	.get_wptr = dpipe_ring_get_wptr,
	.set_wptr = dpipe_ring_set_wptr,
	.emit_frame_size = LOONGGPU_PIPE_EMIT_FRAME_SIZE,
	.emit_ib_size =	LOONGGPU_PIPE_EMIT_IB_SIZE,
	.emit_ib = loonggpu_pipe_ring_emit_ib,
	.emit_fence = loonggpu_pipe_ring_emit_fence,
	.emit_pipeline_sync = loonggpu_pipe_ring_emit_pipeline_sync,
	.emit_vm_flush = loonggpu_pipe_ring_emit_vm_flush,
	.test_ring = loonggpu_pipe_ring_test_ring,
	.test_cs = loonggpu_pipe_ring_test_cs,
	.test_ib = loonggpu_pipe_ring_test_ib,
	.insert_nop = loonggpu_ring_insert_nop,
	.pad_ib = loonggpu_ring_generic_pad_ib,
	.emit_wreg = loonggpu_pipe_ring_emit_wreg,
};

static void dpipe_set_ring_funcs(struct loonggpu_device *adev)
{
        dpipe_ring_funcs.nop = LG2XX_SCMD32(LG2XX_SCMD32_OP_NOP, 0);
	adev->dpipe.ring.funcs = &dpipe_ring_funcs;
}

static const struct loonggpu_irq_src_funcs dpipe_eop_irq_funcs = {
	.set = dpipe_set_eop_interrupt_state,
	.process = dpipe_eop_irq,
};

static void dpipe_set_irq_funcs(struct loonggpu_device *adev)
{
	adev->dpipe.eop_irq.num_types = LOONGGPU_CP_IRQ_LAST;
	adev->dpipe.eop_irq.funcs = &dpipe_eop_irq_funcs;
}

const struct loonggpu_ip_block_version dpipe_ip_block = {
	.type = LOONGGPU_IP_BLOCK_TYPE_DPIPE,
	.major = 8,
	.minor = 0,
	.rev = 0,
	.funcs = &dpipe_ip_funcs,
};
