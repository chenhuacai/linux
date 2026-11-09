#include <linux/kernel.h>
#include "loonggpu.h"
#include "loonggpu_common.h"
#include "loonggpu_cp.h"
#include "loonggpu_pipe.h"

#define LOONGGPU_PERF_BUF_SIZE                      (8 * 1024)

void loonggpu_pipe_ring_emit_ib(struct loonggpu_ring *ring,
				struct loonggpu_ib *ib,
				unsigned vmid, bool ctx_switch)
{
        u32 header, control = 0;

        loonggpu_ring_write(ring, LG2XX_SCMD32(LG2XX_SCMD32_OP_VMID, vmid));
        header = LG2XX_SCMD32(LG2XX_SCMD32_OP_IB, 0);

        control |= ib->length_dw | (vmid << 24);

        loonggpu_ring_write(ring, header);
        loonggpu_ring_write(ring, lower_32_bits(ib->gpu_addr));
        loonggpu_ring_write(ring, upper_32_bits(ib->gpu_addr));
        loonggpu_ring_write(ring, control);
}

void loonggpu_pipe_ring_emit_fence(struct loonggpu_ring *ring, u64 addr,
				   u64 seq, unsigned flags)
{
        bool write64bit = flags & LOONGGPU_FENCE_FLAG_64BIT;
        bool int_sel = flags & LOONGGPU_FENCE_FLAG_INT;

        loonggpu_ring_write(ring, LG2XX_SCMD32(LG2XX_SCMD32_OP_VMID, 0));
        loonggpu_ring_write(ring, LG2XX_SCMD32(write64bit ? LG2XX_SCMD32_OP_WB64 : LG2XX_SCMD32_OP_WB32, 0));
        loonggpu_ring_write(ring, lower_32_bits(addr));
        loonggpu_ring_write(ring, upper_32_bits(addr));
        loonggpu_ring_write(ring, lower_32_bits(seq));

        if (write64bit)
                loonggpu_ring_write(ring, upper_32_bits(seq));

        if (int_sel)
                loonggpu_ring_write(ring, GSPKT(LG2XX_SCMD32_OP_INTR, 0));
}

void loonggpu_pipe_ring_emit_pipeline_sync(struct loonggpu_ring *ring)
{
        uint32_t seq = ring->fence_drv.sync_seq;
        uint64_t addr = ring->fence_drv.gpu_addr;

        loonggpu_ring_write(ring, LG2XX_SCMD32(LG2XX_SCMD32_OP_VMID, 0));
        loonggpu_ring_write(ring, LG2XX_SCMD32(LG2XX_SCMD32_OP_POLL, 0) |
                                      POLL_CONDITION(3) | /* equal */
                                      POLL_REG_MEM(1));   /* reg/mem */

        loonggpu_ring_write(ring, lower_32_bits(addr));
        loonggpu_ring_write(ring, upper_32_bits(addr));
        loonggpu_ring_write(ring, seq);                           /* reference */
        loonggpu_ring_write(ring, 0xffffffff);                    /* mask */
        loonggpu_ring_write(ring, POLL_TIMES_INTERVAL(0xfff, 1)); /* retry count, poll interval */
}

void loonggpu_pipe_ring_emit_vm_flush(struct loonggpu_ring *ring,
				      unsigned vmid, uint64_t pd_addr)
{
        loonggpu_gmc_emit_flush_gpu_tlb(ring, vmid, pd_addr);
}

void loonggpu_pipe_ring_emit_wreg(struct loonggpu_ring *ring, uint32_t reg,
				  uint32_t val)
{
        loonggpu_ring_write(ring, LG2XX_SCMD32(LG2XX_SCMD32_OP_WREG, 0));
        loonggpu_ring_write(ring, reg);
        loonggpu_ring_write(ring, val);
}

int loonggpu_pipe_ring_test_ring(struct loonggpu_ring *ring)
{
	struct loonggpu_device *adev = ring->adev;
	unsigned i;
	unsigned index;
	int r;
	u32 tmp;
	u64 gpu_addr;

	r = loonggpu_device_wb_get(adev, &index);
	if (r) {
		dev_err(adev->dev, "(%d) failed to allocate wb slot\n", r);
		return r;
	}

	gpu_addr = adev->wb.gpu_addr + (index * 4);
	tmp = 0xCAFEDEAD;
	adev->wb.wb[index] = cpu_to_le32(tmp);

	r = loonggpu_ring_alloc(ring, 4);
	if (r) {
		DRM_ERROR("loonggpu: dma failed to lock ring %d (%d).\n", ring->idx, r);
		loonggpu_device_wb_free(adev, index);
		return r;
	}

	if (adev->family_type == CHIP_LG100) 
		loonggpu_ring_write(ring, GSPKT(GSPKT_WRITE, 3) | WRITE_DST_SEL(1) | WRITE_WAIT);
	else if (adev->family_type == CHIP_LG200 ||
		 adev->family_type == CHIP_LG210 ||
		 adev->family_type == CHIP_LG211)
		loonggpu_ring_write(ring, LG2XX_SCMD32(LG2XX_SCMD32_OP_WB32, 0));
	else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);

	loonggpu_ring_write(ring, lower_32_bits(gpu_addr));
	loonggpu_ring_write(ring, upper_32_bits(gpu_addr));
	loonggpu_ring_write(ring, 0xDEADBEEF);
	loonggpu_ring_commit(ring);

	for (i = 0; i < 4 * adev->usec_timeout; i++) {
		tmp = le32_to_cpu(adev->wb.wb[index]);
		if (tmp == 0xDEADBEEF)
			break;
		udelay(1);
	}

	if (i < adev->usec_timeout) {
		DRM_INFO("ring %s test on %d succeeded in %d usecs\n", ring->name, ring->idx, i);
		r = 0;
	} else {
		DRM_ERROR("loonggpu: ring %s %d test failed (0x%08X)\n", ring->name,
			  ring->idx, tmp);
		r = -EINVAL;
	}
	loonggpu_device_wb_free(adev, index);

	return r;
}

static int loonggpu_pipe_ring_test_ibs(struct loonggpu_ring *ring, long timeout, unsigned r_size, unsigned cmds)
{
	struct loonggpu_device *adev = ring->adev;
	struct loonggpu_ib ib;
	struct dma_fence *f = NULL;
	unsigned index;
	u32 tmp = 0;
	u64 gpu_addr;
	long r;
	int i;

	r = loonggpu_device_wb_get(adev, &index);
	if (r) {
		dev_err(adev->dev, "(%ld) failed to allocate wb slot\n", r);
		return r;
	}

	gpu_addr = adev->wb.gpu_addr + (index * 4);
	tmp = 0xCAFEDEAD;
	adev->wb.wb[index] = cpu_to_le32(tmp);
	memset(&ib, 0, sizeof(ib));
	r = loonggpu_ib_get(adev, NULL, r_size, &ib);
	if (r) {
		DRM_ERROR("loonggpu: failed to get ib (%ld).\n", r);
		goto err0;
	}
	ib.length_dw = 0;

	for (i = 0; i < cmds; i++) {
		if (adev->family_type == CHIP_LG100)
			ib.ptr[ib.length_dw++] = GSPKT(GSPKT_WRITE, 3) | WRITE_DST_SEL(1) | WRITE_WAIT;
		else if (adev->family_type == CHIP_LG200 ||
			 adev->family_type == CHIP_LG210 ||
			 adev->family_type == CHIP_LG211)
			ib.ptr[ib.length_dw++] = LG2XX_SCMD32(LG2XX_SCMD32_OP_WB32, 0);
		else
			DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);

		ib.ptr[ib.length_dw++] = lower_32_bits(gpu_addr);
		ib.ptr[ib.length_dw++] = upper_32_bits(gpu_addr);
		ib.ptr[ib.length_dw++] = 0xDEADBEEF + i;
	}

	r = loonggpu_ib_schedule(ring, 1, &ib, NULL, &f);
	if (r)
		goto err1;

	r = dma_fence_wait_timeout(f, false, timeout);
	if (r == 0) {
		DRM_ERROR("loonggpu: %s IB test timed out\n", ring->name);
		r = -ETIMEDOUT;
		goto err1;
	} else if (r < 0) {
		DRM_ERROR("loonggpu: %s fence wait failed (%ld).\n", ring->name, r);
		goto err1;
	}
	tmp = le32_to_cpu(adev->wb.wb[index]);
	if (tmp == (0xDEADBEEF + i - 1)) {
		DRM_INFO("%s ib test on ring %d succeeded %d cmd %d ms \n",
			ring->name, ring->idx, i, jiffies_to_msecs(timeout - r));
		r = 0;
	} else {
		DRM_ERROR("loonggpu:%s ib test failed (0x%08X) cmd %d\n", ring->name, tmp, i);
		r = -EINVAL;
	}

err1:
	loonggpu_ib_free(adev, &ib, NULL);
	dma_fence_put(f);
err0:
	loonggpu_device_wb_free(adev, index);

	return r;
}

int loonggpu_pipe_ring_test_ib(struct loonggpu_ring *ring, long timeout)
{
	return loonggpu_pipe_ring_test_ibs(ring, timeout, 256, 1);
}

int loonggpu_pipe_ring_test_cs(struct loonggpu_ring *ring, long timeout)
{
	return loonggpu_pipe_ring_test_ibs(ring, timeout,
		LOONGGPU_PERF_BUF_SIZE, LOONGGPU_PERF_BUF_SIZE / 4 - 4);
}