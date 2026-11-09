#include <linux/kernel.h>
#include "loonggpu.h"
#include "loonggpu_common.h"
#include "loonggpu_cp.h"
#include "loonggpu_pipe.h"

static void epipe_set_ring_funcs(struct loonggpu_device *adev);
static void epipe_set_irq_funcs(struct loonggpu_device *adev);

static uint32_t epipe_cb_wptr_offset = 0;
static uint32_t epipe_cb_rptr_offset = 0;

static int epipe_gpu_early_init(struct loonggpu_device *adev)
{
	return 0;
}

static int epipe_sw_init(void *handle)
{
	int r;
	struct loonggpu_ring *ring;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	/* FENCE Event */
	r = loonggpu_irq_add_id(adev, LOONGGPU_IH_CLIENTID_LEGACY, LOONGGPU_LG210_SRCID_CP_END_OF_EPIPE, &adev->epipe.eop_irq);
	if (r)
		return r;

	/* set up the epipe ring */
	ring = &adev->epipe.ring;
	ring->ring_obj = NULL;
	sprintf(ring->name, "epipe");

	r = loonggpu_ring_init(adev, ring, 256, &adev->epipe.eop_irq,
				LOONGGPU_CP_IRQ_EPIPE_EOP);
	if (r)
		return r;

	r = epipe_gpu_early_init(adev);
	if (r)
		return r;

	return 0;
}

static int epipe_sw_fini(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	loonggpu_ring_fini(&adev->epipe.ring);

	return 0;
}

static int epipe_cp_epipe_resume(struct loonggpu_device *adev)
{
	struct loonggpu_ring *ring;

        epipe_cb_wptr_offset = LOONGGPU_LG2XX_EPIPE_CB_WPTR_OFFSET;
        epipe_cb_rptr_offset = LOONGGPU_LG2XX_EPIPE_CB_RPTR_OFFSET;

	/* Set ring buffer size */
	ring = &adev->epipe.ring;

	/* start the ring */
	loonggpu_ring_clear_ring(ring);

	/* Initialize the ring buffer's read and write pointers */
	ring->wptr = 0;
	WREG32(epipe_cb_wptr_offset, lower_32_bits(ring->wptr));

	/* set the RPTR */
	WREG32(epipe_cb_rptr_offset, 0);

	mdelay(1);

	loonggpu_cmd_exec(adev, LG2XX_ICMD32i(LG2XX_ICMD32_MOP_EPIPE,
			LG2XX_ICMD32_SOP_EPIPE_BBQ, 0),
			lower_32_bits(ring->gpu_addr), upper_32_bits(ring->gpu_addr));
        loonggpu_cmd_exec(adev, LG2XX_ICMD32i(LG2XX_ICMD32_MOP_EPIPE,
			LG2XX_ICMD32_SOP_EPIPE_BQSZ, 0), 
                        ring->ring_size / 4, 0);

	ring->ready = true;
	return 0;
}

static int epipe_cp_resume(struct loonggpu_device *adev)
{
	int r;

	r = epipe_cp_epipe_resume(adev);
	if (r)
		return r;

	return 0;
}

static int epipe_hw_init(void *handle)
{
	int r;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	r = epipe_cp_resume(adev);

	return r;
}

static int epipe_hw_fini(void *handle)
{
	return 0;
}

static int epipe_suspend(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	return epipe_hw_fini(adev);
}

static int epipe_resume(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	return epipe_hw_init(adev);
}

static bool epipe_is_idle(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	return (RREG32(LOONGGPU_STATUS) == GSCMD_STS_DONE);
}

static int epipe_wait_for_idle(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	if (loonggpu_cp_wait_done(adev) == true)
			return 0;

	return -ETIMEDOUT;
}

static int epipe_early_init(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	epipe_set_ring_funcs(adev);
	epipe_set_irq_funcs(adev);
	return 0;
}

static int epipe_late_init(void *handle)
{
	return 0;
}

static u64 epipe_ring_get_rptr(struct loonggpu_ring *ring)
{
	struct loonggpu_device *adev = ring->adev;

	return RREG32(epipe_cb_rptr_offset);
}

static u64 epipe_ring_get_wptr(struct loonggpu_ring *ring)
{
	struct loonggpu_device *adev = ring->adev;

	return RREG32(epipe_cb_wptr_offset);
}

static void epipe_ring_set_wptr(struct loonggpu_ring *ring)
{
	struct loonggpu_device *adev = ring->adev;

	WREG32(epipe_cb_wptr_offset, lower_32_bits(ring->wptr));
}

static int epipe_set_eop_interrupt_state(struct loonggpu_device *adev,
					    struct loonggpu_irq_src *src,
					    unsigned type,
					    enum loonggpu_interrupt_state state)
{
	return 0;
}

static int epipe_eop_irq(struct loonggpu_device *adev,
			    struct loonggpu_irq_src *source,
			    struct loonggpu_iv_entry *entry)
{
	u8 me_id;

	DRM_DEBUG("IH: EPIPE FENCE\n");
	me_id = (entry->ring_id & 0x0c) >> 2;

	switch (me_id) {
	case 0:
		loonggpu_fence_process(&adev->epipe.ring);
		break;
	default:
		DRM_ERROR("loonggpu epipe number fail 0x%x",me_id);
		break;
	}
	return 0;
}

static const struct loonggpu_ip_funcs epipe_ip_funcs = {
	.name = "epipe",
	.early_init = epipe_early_init,
	.late_init = epipe_late_init,
	.sw_init = epipe_sw_init,
	.sw_fini = epipe_sw_fini,
	.hw_init = epipe_hw_init,
	.hw_fini = epipe_hw_fini,
	.suspend = epipe_suspend,
	.resume = epipe_resume,
	.is_idle = epipe_is_idle,
	.wait_for_idle = epipe_wait_for_idle,
};

static struct loonggpu_ring_funcs epipe_ring_funcs = {
	.type = LOONGGPU_RING_TYPE_EPIPE,
	.align_mask = 0xf,
	.nop = LG2XX_SCMD32(LG2XX_SCMD32_OP_NOP, 0),
	.support_64bit_ptrs = false,
	.get_rptr = epipe_ring_get_rptr,
	.get_wptr = epipe_ring_get_wptr,
	.set_wptr = epipe_ring_set_wptr,
	.emit_frame_size = LOONGGPU_PIPE_EMIT_FRAME_SIZE,
	.emit_ib_size =	LOONGGPU_PIPE_EMIT_IB_SIZE, /* epipe_ring_emit_ib */
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

static void epipe_set_ring_funcs(struct loonggpu_device *adev)
{
        epipe_ring_funcs.nop = LG2XX_SCMD32(LG2XX_SCMD32_OP_NOP, 0);
	adev->epipe.ring.funcs = &epipe_ring_funcs;
}

static const struct loonggpu_irq_src_funcs epipe_eop_irq_funcs = {
	.set = epipe_set_eop_interrupt_state,
	.process = epipe_eop_irq,
};

static void epipe_set_irq_funcs(struct loonggpu_device *adev)
{
	adev->epipe.eop_irq.num_types = LOONGGPU_CP_IRQ_LAST;
	adev->epipe.eop_irq.funcs = &epipe_eop_irq_funcs;
}

const struct loonggpu_ip_block_version epipe_ip_block = {
	.type = LOONGGPU_IP_BLOCK_TYPE_EPIPE,
	.major = 8,
	.minor = 0,
	.rev = 0,
	.funcs = &epipe_ip_funcs,
};
