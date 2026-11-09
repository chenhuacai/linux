#include <linux/kernel.h>
#include <linux/delay.h>
#include "loonggpu.h"
#include "loonggpu_common.h"
#include "loonggpu_cp.h"
#include "loonggpu_irq.h"
#include "loonggpu_pipe.h"
#include "loonggpu_dc_vbios.h"
#include "loonggpu_dc_resource.h"

#define GFX8_NUM_GFX_RINGS     1

MODULE_FIRMWARE("loonggpu/lg100_cp.bin");
MODULE_FIRMWARE("loonggpu/lg200_cp.bin");
MODULE_FIRMWARE("loonggpu/lg210_cp.bin");
MODULE_FIRMWARE("loonggpu/lg211_cp.bin");

static void gfx_set_ring_funcs(struct loonggpu_device *adev);
static void gfx_set_irq_funcs(struct loonggpu_device *adev);

static int gfx_ring_test_ring(struct loonggpu_ring *ring)
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
	else if (adev->family_type == CHIP_LG200
		|| adev->family_type == CHIP_LG210
		|| adev->family_type == CHIP_LG211)
		loonggpu_ring_write(ring, LG2XX_SCMD32(LG2XX_SCMD32_OP_WB32, 0));
	else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);

	loonggpu_ring_write(ring, lower_32_bits(gpu_addr));
	loonggpu_ring_write(ring, upper_32_bits(gpu_addr));
	loonggpu_ring_write(ring, 0xDEADBEEF);
	loonggpu_ring_commit(ring);

	for (i = 0; i < adev->usec_timeout; i++) {
		tmp = le32_to_cpu(adev->wb.wb[index]);
		if (tmp == 0xDEADBEEF)
			break;
		udelay(1);
	}

	if (i < adev->usec_timeout) {
		DRM_INFO("ring test on %d succeeded in %d usecs\n", ring->idx, i);
	} else {
		DRM_ERROR("loonggpu: ring %d test failed (0x%08X)\n",
			  ring->idx, tmp);
		r = -EINVAL;
	}
	loonggpu_device_wb_free(adev, index);

	return r;
}

static int gfx_ring_test_ib(struct loonggpu_ring *ring, long timeout)
{
	struct loonggpu_device *adev = ring->adev;
	struct loonggpu_ib ib;
	struct dma_fence *f = NULL;

	unsigned int index;
	uint64_t gpu_addr;
	uint32_t tmp;
	long r;

	r = loonggpu_device_wb_get(adev, &index);
	if (r) {
		dev_err(adev->dev, "(%ld) failed to allocate wb slot\n", r);
		return r;
	}

	gpu_addr = adev->wb.gpu_addr + (index * 4);
	adev->wb.wb[index] = cpu_to_le32(0xCAFEDEAD);
	memset(&ib, 0, sizeof(ib));
	r = loonggpu_ib_get(adev, NULL, 16, &ib);
	if (r) {
		DRM_ERROR("loonggpu: failed to get ib (%ld).\n", r);
		goto err1;
	}

	if (adev->family_type == CHIP_LG100) 
		ib.ptr[0] = GSPKT(GSPKT_WRITE, 3) | WRITE_DST_SEL(1) | WRITE_WAIT;
	else if (adev->family_type == CHIP_LG200
		|| adev->family_type == CHIP_LG210
		|| adev->family_type == CHIP_LG211)
		ib.ptr[0] = LG2XX_SCMD32(LG2XX_SCMD32_OP_WB32, 0);
	else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);

	ib.ptr[1] = lower_32_bits(gpu_addr);
	ib.ptr[2] = upper_32_bits(gpu_addr);
	ib.ptr[3] = 0xDEADBEEF;
	ib.length_dw = 4;

	r = loonggpu_ib_schedule(ring, 1, &ib, NULL, &f);
	if (r)
		goto err2;

	r = dma_fence_wait_timeout(f, false, timeout);
	if (r == 0) {
		DRM_ERROR("loonggpu: IB test timed out.\n");
		r = -ETIMEDOUT;
		goto err2;
	} else if (r < 0) {
		DRM_ERROR("loonggpu: fence wait failed (%ld).\n", r);
		goto err2;
	}

	tmp = adev->wb.wb[index];
	if (tmp == 0xDEADBEEF) {
		DRM_DEBUG("ib test on ring %d succeeded\n", ring->idx);
		r = 0;
	} else {
		DRM_ERROR("ib test on ring %d failed\n", ring->idx);
		r = -EINVAL;
	}

err2:
	loonggpu_ib_free(adev, &ib, NULL);
	dma_fence_put(f);
err1:
	loonggpu_device_wb_free(adev, index);
	return r;
}

static int gfx_ring_test_tl_ib(struct loonggpu_ring *ring, long timeout)
{
       struct loonggpu_device *adev = ring->adev;
       struct loonggpu_ib ib0, ib1;
       struct dma_fence *f = NULL;
       unsigned vmid = 0;
       unsigned int index;
       uint64_t gpu_addr;
       uint32_t tmp;
       unsigned i;
       long r;

       r = loonggpu_device_wb_get(adev, &index);
       if (r) {
               dev_err(adev->dev, "(%ld) failed to allocate wb slot\n", r);
               return r;
       }

       gpu_addr = adev->wb.gpu_addr + (index * 4);
       adev->wb.wb[index] = cpu_to_le32(0xCAFEDEAD);
       memset(&ib0, 0, sizeof(ib0));
       r = loonggpu_ib_get(adev, NULL, 16, &ib0);
       if (r) {
               DRM_ERROR("loonggpu: failed to get ib0 (%ld).\n", r);
               goto err1;
       }

	if (adev->family_type == CHIP_LG100) {
		ib0.ptr[0] = GSPKT(GSPKT_WRITE, 3) |
			      WRITE_DST_SEL(1) | WRITE_WAIT;
	} else if (adev->family_type == CHIP_LG200
			|| adev->family_type == CHIP_LG210
			|| adev->family_type == CHIP_LG211) {
		ib0.ptr[0] = LG2XX_SCMD32(LG2XX_SCMD32_OP_WB32, 0);
	} else
		DRM_ERROR("%s Illegal Family type %d\n",
			  __func__, adev->family_type);

       ib0.ptr[1] = lower_32_bits(gpu_addr);
       ib0.ptr[2] = upper_32_bits(gpu_addr);
       ib0.ptr[3] = 0xDEADBEEF;
       ib0.length_dw = 4;

       memset(&ib1, 0, sizeof(ib1));
       r = loonggpu_ib_get(adev, NULL, 64, &ib1);
       if (r) {
               DRM_ERROR("loonggpu: failed to get ib1 (%ld).\n", r);
               goto err1;
       }

	if (ring->adev->family_type == CHIP_LG100) {
		ib1.ptr[0] = GSPKT(GSPKT_NOP, 0);
		ib1.ptr[1] = GSPKT(GSPKT_INDIRECT, 3);
	} else if (ring->adev->family_type == CHIP_LG200
			|| ring->adev->family_type == CHIP_LG210
			|| ring->adev->family_type == CHIP_LG211) {
		ib1.ptr[0] = LG2XX_SCMD32(LG2XX_SCMD32_OP_VMID, 0);
		ib1.ptr[1] = LG2XX_SCMD32(LG2XX_SCMD32_OP_IB, 0);
	} else
		DRM_ERROR("%s Illegal Family type %d\n",
			  __func__, ring->adev->family_type);

       ib1.ptr[2] = lower_32_bits(ib0.gpu_addr);
       ib1.ptr[3] = upper_32_bits(ib0.gpu_addr);
       ib1.ptr[4] = ib0.length_dw | (vmid << 24);
       ib1.ptr[5] = LG2XX_SCMD32(LG2XX_SCMD32_OP_VMID, 0);
       for (i = 6; i < 64; i++)
	       ib1.ptr[i] = LG2XX_SCMD32(LG2XX_SCMD32_OP_NOP, 0);
       ib1.length_dw = 64;

       r = loonggpu_ib_schedule(ring, 1, &ib1, NULL, &f);
       if (r)
	       goto err2;

       r = dma_fence_wait_timeout(f, false, timeout);
       if (r == 0) {
	       DRM_ERROR("loonggpu: two level IB test timed out.\n");
	       r = -ETIMEDOUT;
	       goto err2;
       }
       else if (r < 0) {
	       DRM_ERROR("loonggpu: fence wait failed (%ld).\n", r);
	       goto err2;
       }

       tmp = adev->wb.wb[index];
       if (tmp == 0xDEADBEEF)
	       DRM_INFO("two level ib test on ring %d succeeded\n", ring->idx);
       else {
	       DRM_ERROR("two level ib test on ring %d failed\n", ring->idx);
	       r = -EINVAL;
       }

       loonggpu_ib_free(adev, &ib1, NULL);
err2:
	loonggpu_ib_free(adev, &ib0, NULL);
	dma_fence_put(f);
err1:
	loonggpu_device_wb_free(adev, index);

	return r;
}

static int gfx_gpu_get_cu_info(struct loonggpu_device *adev)
{
	int i, j;
	struct loonggpu_cu_info *acu_info = &adev->gfx.cu_info;

	memset(acu_info, 0, sizeof(*acu_info));

	acu_info->simd_per_cu = 4;
	acu_info->max_waves_per_simd = 10;
	acu_info->wave_front_size = 32;
	acu_info->max_scratch_slots_per_cu = 40;
	acu_info->lds_size = 128;

	if (adev->family_type == CHIP_LG100)
		acu_info->number = 1;
	else if (adev->family_type == CHIP_LG200 ||
		 adev->family_type == CHIP_LG211)
		acu_info->number = 2;
	else if (adev->family_type == CHIP_LG210)
		acu_info->number = 8;
	else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);

	for (i = 0; i < adev->gfx.config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx.config.max_sh_per_se; j++) {
			acu_info->bitmap[i][j] = 0x1;
			acu_info->ao_cu_bitmap[i][j] = 0x1;
		}
	}
	return 0;
}

static uint32_t gfx_compute_clock_common(struct loonggpu_device *adev,
					 uint32_t reg, uint32_t ch)
{
	uint64_t tmp, pll_div_ref, pll_loopc, pll_div_out;
	uint32_t clk;
	void __iomem *io_base = adev->io_base;

	if (io_base == NULL)
		return 0;

	if (adev->family_type != CHIP_LG200 &&
	    adev->family_type != CHIP_LG211)
		return 0;

	tmp = readq(io_base + reg);
	mb();

	pll_div_ref = (tmp >> 32) & 0x7f;
	pll_loopc = (tmp >> 21) & 0x1ff;
	pll_div_out = (tmp >> (7 * ch)) & 0x7f;

	clk = 100 * pll_loopc / pll_div_ref / pll_div_out;

	return clk * 100;
}

static uint32_t gfx_vram_clock_9a1000(struct loonggpu_device *adev)
{
	uint64_t tmp, pll_div_ref, pll_loopc, pll_div_out, pll_div_sel;
	uint32_t clk;
	void __iomem *io_base = NULL;

	io_base = adev->rmmio_sc + 0x8008;
	if (io_base == NULL)
		return 0;

	tmp = readq(io_base);
	/* Synchronize consecutive MMIO reads to guarantee ordering of
	 * ref/div_out register pair as required by 9a1000 PLL spec.
	 */
	mb();

	pll_loopc = (tmp >> 20) & 0x3ff;
	tmp = readq(io_base + 0x4);

	/* Same as above. */
	mb();

	pll_div_ref = (tmp >> 0) & 0xf;
	pll_div_out = (tmp >> 4) & 0x3;
	pll_div_out = (pll_div_out <= 3) ? (1U << pll_div_out) : 1U;
	pll_div_sel = ((tmp >> 26) & 0x1) ? 3 : 1;

	clk = 27 * pll_loopc / pll_div_ref / pll_div_out / pll_div_sel;

	return clk * 100;
}

static uint32_t gfx_compute_clock_9a1000(struct loonggpu_device *adev)
{
	uint64_t tmp, pll_div_ref, pll_loopc, pll_div_out;
	uint32_t clk;
	void __iomem *io_base = NULL;

	io_base = adev->rmmio_sc + 0x8000;
	if (io_base == NULL)
		return 0;

	tmp = readq(io_base);
	mb();

	pll_div_ref = 1;
	pll_loopc = (tmp >> 24) & 0xff;
	pll_div_out = (tmp >> 0) & 0x7f;

	clk = 27 * pll_loopc / pll_div_ref / pll_div_out;

	return clk * 100;
}

static uint32_t gfx_get_mem_clock(struct loonggpu_device *adev)
{
	/* return all clocks in 100KHz */
	if (adev->family_type == CHIP_LG210)
		return gfx_vram_clock_9a1000(adev) * 2;
	return gfx_compute_clock_common(adev, 0x490, 1) * 2;
}

static uint32_t gfx_get_engine_clock(struct loonggpu_device *adev)
{
	/* return all clocks in 100KHz */
	if (adev->family_type == CHIP_LG210)
		return gfx_compute_clock_9a1000(adev);
	return gfx_compute_clock_common(adev, 0x4a0, 0);
}

static int gfx_gpu_early_init(struct loonggpu_device *adev)
{
	struct gpu_resource *gpu_res;
	u32 gb_addr_config;
	u32 tmp;

	adev->gfx.config.max_shader_engines = 2;
	adev->gfx.config.max_tile_pipes = 4;
	adev->gfx.config.max_cu_per_sh = 2;
	adev->gfx.config.max_sh_per_se = 1;
	adev->gfx.config.max_backends_per_se = 2;
	adev->gfx.config.max_texture_channel_caches = 4;
	adev->gfx.config.max_gprs = 256;
	adev->gfx.config.max_gs_threads = 32;
	adev->gfx.config.max_hw_contexts = 8;

	adev->gfx.config.sc_prim_fifo_size_frontend = 0x20;
	adev->gfx.config.sc_prim_fifo_size_backend = 0x100;
	adev->gfx.config.sc_hiz_tile_fifo_size = 0x30;
	adev->gfx.config.sc_earlyz_tile_fifo_size = 0x130;

	adev->gfx.config.mc_arb_ramcfg = 0;

	adev->gfx.config.num_tile_pipes = adev->gfx.config.max_tile_pipes;
	adev->gfx.config.mem_max_burst_length_bytes = 256;

	tmp = 0;
	adev->gfx.config.mem_row_size_in_kb = (4 * (1 << (8 + tmp))) / 1024;
	if (adev->gfx.config.mem_row_size_in_kb > 4)
		adev->gfx.config.mem_row_size_in_kb = 4;

	adev->gfx.config.shader_engine_tile_size = 32;
	adev->gfx.config.num_gpus = 1;
	adev->gfx.config.multi_gpu_tile_size = 64;

	/* fix up row size */
	switch (adev->gfx.config.mem_row_size_in_kb) {
	case 1:
	default:
		gb_addr_config = 0;
		break;
	case 2:
		gb_addr_config = 0;
		break;
	case 4:
		gb_addr_config = 0;
		break;
	}
	adev->gfx.config.gb_addr_config = gb_addr_config;

	gfx_gpu_get_cu_info(adev);

	adev->gfx.mec.num_mec = 1;
	adev->gfx.mec.num_pipe_per_mec = 1;
	adev->gfx.mec.num_queue_per_pipe = 8;

	adev->clock.default_sclk = gfx_get_engine_clock(adev);
	adev->clock.default_mclk = gfx_get_mem_clock(adev);
	DRM_INFO("GPU CLK:%dMhz VRAM CLK:%dMhz\n",
		 adev->clock.default_sclk / 100,
		 adev->clock.default_mclk / 100);

	/* vbios clocks are in MHz, convert to the driver's 10 KHz units */
	gpu_res = dc_get_vbios_resource(adev->dc->vbios, 0,
					LOONGGPU_RESOURCE_GPU);
	if (gpu_res) {
		adev->clock.default_sclk = gpu_res->shaders_freq * 100;
		adev->clock.default_mclk = gpu_res->freq * 100;
		DRM_INFO("VBIOS info GPU CLK:%dMhz VRAM CLK:%dMhz\n",
			adev->clock.default_sclk / 100,
			adev->clock.default_mclk / 100);
	}

	return 0;
}

static int gfx_sw_init(void *handle)
{
	int i, r;
	struct loonggpu_ring *ring;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	unsigned src_id_cp_end_of_pipe = 0;

	switch (adev->family_type)
	{
	case CHIP_LG100:
		src_id_cp_end_of_pipe = LOONGGPU_SRCID_CP_END_OF_PIPE;
		break;
	case CHIP_LG200:
	case CHIP_LG210:
	case CHIP_LG211:
		src_id_cp_end_of_pipe = LOONGGPU_LG200_SRCID_CP_END_OF_GPIPE;
		break;
	default:
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);
		break;
	}

	/* EOP Event */
	r = loonggpu_irq_add_id(adev, LOONGGPU_IH_CLIENTID_LEGACY, src_id_cp_end_of_pipe, &adev->gfx.eop_irq);
	if (r)
		return r;

	adev->gfx.gfx_current_status = LOONGGPU_GFX_NORMAL_MODE;

	/* set up the gfx ring */
	for (i = 0; i < adev->gfx.num_gfx_rings; i++) {
		ring = &adev->gfx.gfx_ring[i];
		ring->ring_obj = NULL;
		sprintf(ring->name, "gfx");

		r = loonggpu_ring_init(adev, ring, GFX_RING_BUF_DWS, &adev->gfx.eop_irq,
				     LOONGGPU_CP_IRQ_GFX_EOP);
		if (r)
			return r;
	}

	adev->gfx.ce_ram_size = 0x8000;

	r = gfx_gpu_early_init(adev);
	if (r)
		return r;

	return 0;
}

static int gfx_sw_fini(void *handle)
{
	int i;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	for (i = 0; i < adev->gfx.num_gfx_rings; i++)
		loonggpu_ring_fini(&adev->gfx.gfx_ring[i]);

	return 0;
}

static void gfx_parse_ind_reg_list(int *register_list_format,
				int ind_offset,
				int list_size,
				int *unique_indices,
				int *indices_count,
				int max_indices,
				int *ind_start_offsets,
				int *offset_count,
				int max_offset)
{
	int indices;
	bool new_entry = true;

	for (; ind_offset < list_size; ind_offset++) {

		if (new_entry) {
			new_entry = false;
			ind_start_offsets[*offset_count] = ind_offset;
			*offset_count = *offset_count + 1;
			BUG_ON(*offset_count >= max_offset);
		}

		if (register_list_format[ind_offset] == 0xFFFFFFFF) {
			new_entry = true;
			continue;
		}

		ind_offset += 2;

		/* look for the matching indice */
		for (indices = 0;
			indices < *indices_count;
			indices++) {
			if (unique_indices[indices] ==
				register_list_format[ind_offset])
				break;
		}

		if (indices >= *indices_count) {
			unique_indices[*indices_count] =
				register_list_format[ind_offset];
			indices = *indices_count;
			*indices_count = *indices_count + 1;
			BUG_ON(*indices_count >= max_indices);
		}

		register_list_format[ind_offset] = indices;
	}
}

static int gfx_cp_gfx_resume(struct loonggpu_device *adev)
{
	struct loonggpu_ring *ring;
	int r = 0;

	switch (adev->family_type)
	{
	case CHIP_LG100:
		loonggpu_cmd_exec(adev, GSCMD(GSCMD_PIPE, GSCMD_PIPE_FLUSH), 1, ~1);
		adev->gfx_cb_wptr_offset = LOONGGPU_GFX_CB_WPTR_OFFSET;
		adev->gfx_cb_rptr_offset = LOONGGPU_GFX_CB_RPTR_OFFSET;
		break;
	case CHIP_LG200:
	case CHIP_LG210:
	case CHIP_LG211:
		loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_SYNC, 
			LG2XX_ICMD32_SOP_SYNC_CSYNC), 1, ~1);
		adev->gfx_cb_wptr_offset = LOONGGPU_LG2XX_GPIPE_CB_WPTR_OFFSET;
		adev->gfx_cb_rptr_offset = LOONGGPU_LG2XX_GPIPE_CB_RPTR_OFFSET;
		break;
	default:
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);
		break;
	}

	/* Set ring buffer size */
	ring = &adev->gfx.gfx_ring[0];

	/* clear the ring */
	loonggpu_ring_clear_ring(ring);

	/* Initialize the ring buffer's read and write pointers */
	ring->wptr = 0;
	WREG32(adev->gfx_cb_wptr_offset, lower_32_bits(ring->wptr));

	/* set the RPTR */
	WREG32(adev->gfx_cb_rptr_offset, 0);

	mdelay(1);

	if (adev->family_type == CHIP_LG100) {
		WREG32(LOONGGPU_GFX_CB_BASE_LO_OFFSET, ring->gpu_addr);
		WREG32(LOONGGPU_GFX_CB_BASE_HI_OFFSET, upper_32_bits(ring->gpu_addr));
	} else if (adev->family_type == CHIP_LG200
		|| adev->family_type == CHIP_LG210
		|| adev->family_type == CHIP_LG211) {
		loonggpu_cmd_exec(adev,
			LG2XX_ICMD32i(LG2XX_ICMD32_MOP_GPIPE,
				LG2XX_ICMD32_SOP_GPIPE_BGQ, 0),
			lower_32_bits(ring->gpu_addr), upper_32_bits(ring->gpu_addr));
		loonggpu_cmd_exec(adev,
			LG2XX_ICMD32i(LG2XX_ICMD32_MOP_GPIPE,
				LG2XX_ICMD32_SOP_GPIPE_GQSZ, 0),
			ring->ring_size / 4, 0);
	} else {
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);
	}

	ring->ready = true;

	return r;
}

static int gfx_cp_resume(struct loonggpu_device *adev)
{
	int r;

	r = gfx_cp_gfx_resume(adev);
	if (r)
		return r;

	return 0;
}

static int gfx_hw_init(void *handle)
{
	int r;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	r = gfx_cp_resume(adev);

	return r;
}

static int gfx_hw_fini(void *handle)
{
	return 0;
}

static int gfx_suspend(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;
	adev->gfx.in_suspend = true;
	return gfx_hw_fini(adev);
}

static int gfx_resume(void *handle)
{
	int r;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	r = gfx_hw_init(adev);
	adev->gfx.in_suspend = false;
	return r;
}

static bool gfx_is_idle(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	return (RREG32(LOONGGPU_STATUS) == GSCMD_STS_DONE);
}

static int gfx_wait_for_idle(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	if (loonggpu_cp_wait_done(adev) == true)
			return 0;

	return -ETIMEDOUT;
}

/**
 * gfx_get_gpu_clock_counter - return GPU clock counter snapshot
 *
 * @adev: loonggpu_device pointer
 *
 * Fetches a GPU clock counter snapshot.
 * Returns the 64 bit clock counter snapshot.
 */
static uint64_t gfx_get_gpu_clock_counter(struct loonggpu_device *adev)
{
	uint64_t clock = 0;

	mutex_lock(&adev->gfx.gpu_clock_mutex);

	if ((adev->family_type == CHIP_LG200)
		|| (adev->family_type == CHIP_LG210)
		|| adev->family_type == CHIP_LG211) {
		clock = RREG32(LOONGGPU_LG2XX_TIME_COUNT_LO);
		clock |= (uint64_t)RREG32(LOONGGPU_LG2XX_TIME_COUNT_HI) << 32;
	}
	else
		DRM_DEBUG("%s Not impelet\n", __func__);

	mutex_unlock(&adev->gfx.gpu_clock_mutex);
	return clock;
}

static const struct loonggpu_gfx_funcs gfx_gfx_funcs = {
	.get_gpu_clock_counter = &gfx_get_gpu_clock_counter,
	.read_wave_data = NULL,
	.read_wave_sgprs = NULL,
	.select_me_pipe_q = NULL
};

static int gfx_early_init(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	adev->gfx.num_gfx_rings = GFX8_NUM_GFX_RINGS;
	adev->gfx.funcs = &gfx_gfx_funcs;
	gfx_set_ring_funcs(adev);
	gfx_set_irq_funcs(adev);

	return 0;
}

static int gfx_late_init(void *handle)
{
	return 0;
}

static u64 gfx_ring_get_rptr(struct loonggpu_ring *ring)
{
	return ring->adev->wb.wb[ring->rptr_offs];
}

static u64 gfx_ring_get_wptr_gfx(struct loonggpu_ring *ring)
{
	struct loonggpu_device *adev = ring->adev;

	return RREG32(adev->gfx_cb_wptr_offset);
}

static void gfx_ring_set_wptr_gfx(struct loonggpu_ring *ring)
{
	struct loonggpu_device *adev = ring->adev;

	WREG32(adev->gfx_cb_wptr_offset, lower_32_bits(ring->wptr));
}

static void gfx_ring_emit_ib_gfx(struct loonggpu_ring *ring,
				      struct loonggpu_ib *ib,
				      unsigned vmid, bool ctx_switch)
{
	u32 header, control = 0;

	if (ring->adev->family_type == CHIP_LG100)
		header = GSPKT(GSPKT_INDIRECT, 3);
	else if (ring->adev->family_type == CHIP_LG200
		|| ring->adev->family_type == CHIP_LG210
		|| ring->adev->family_type == CHIP_LG211) {
		loonggpu_ring_write(ring, LG2XX_SCMD32(LG2XX_SCMD32_OP_VMID, vmid));
		header = LG2XX_SCMD32(LG2XX_SCMD32_OP_IB, 0);
	}
	else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, ring->adev->family_type);

	control |= ib->length_dw | (vmid << 24);

	loonggpu_ring_write(ring, header);
	loonggpu_ring_write(ring, lower_32_bits(ib->gpu_addr));
	loonggpu_ring_write(ring, upper_32_bits(ib->gpu_addr));
	loonggpu_ring_write(ring, control);
}

static void gfx_ring_emit_fence_gfx(struct loonggpu_ring *ring, u64 addr,
					 u64 seq, unsigned flags)
{
	bool write64bit = flags & LOONGGPU_FENCE_FLAG_64BIT;
	bool int_sel = flags & LOONGGPU_FENCE_FLAG_INT;

	if (ring->adev->family_type == CHIP_LG100) {
		loonggpu_ring_write(ring, GSPKT(GSPKT_FENCE, write64bit ? 4 : 3)
			| (write64bit ? 1 << 9 : 0) | (int_sel ? 1 << 8 : 0));
		loonggpu_ring_write(ring, lower_32_bits(addr));
		loonggpu_ring_write(ring, upper_32_bits(addr));
		loonggpu_ring_write(ring, lower_32_bits(seq));
		if (write64bit)
			loonggpu_ring_write(ring, upper_32_bits(seq));
	} else if (ring->adev->family_type == CHIP_LG200
		|| ring->adev->family_type == CHIP_LG210
		|| ring->adev->family_type == CHIP_LG211) {
		loonggpu_ring_write(ring, LG2XX_SCMD32(LG2XX_SCMD32_OP_VMID, 0));
		loonggpu_ring_write(ring, LG2XX_SCMD32(write64bit?LG2XX_SCMD32_OP_WB32:
				LG2XX_SCMD32_OP_WB32, 0));
		loonggpu_ring_write(ring, lower_32_bits(addr));
		loonggpu_ring_write(ring, upper_32_bits(addr));
		loonggpu_ring_write(ring, lower_32_bits(seq));
	
		if (write64bit)
			loonggpu_ring_write(ring, upper_32_bits(seq));
		else
			loonggpu_ring_write(ring, LG2XX_SCMD32_OP_DBAR);

		if (int_sel)
			loonggpu_ring_write(ring, GSPKT(LG2XX_SCMD32_OP_INTR, 0));

		if (ring->adev->pm.dpm_enabled && (ring->adev->pm.dpm.dvfs.capture_flags & LOONGGPU_DVFS_CAPTURE_EVNT)) {
			loonggpu_ring_write(ring, LG2XX_SCMD32(LG2XX_SCMD32_OP_ENVT, 0xfff));
			loonggpu_ring_write(ring,
				lower_32_bits(ring->adev->pm.dpm.dvfs.evnt_gpu_addr));
			loonggpu_ring_write(ring,
				upper_32_bits(ring->adev->pm.dpm.dvfs.evnt_gpu_addr));
		}
	}
	else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, ring->adev->family_type);

}

static void gfx_ring_emit_pipeline_sync(struct loonggpu_ring *ring)
{
	u32 seq = ring->fence_drv.sync_seq;
	u64 addr = ring->fence_drv.gpu_addr;

	/* wait for idle */
	if (ring->adev->family_type == CHIP_LG100) {
		loonggpu_ring_write(ring, GSPKT(GSPKT_POLL, 5) |
				POLL_CONDITION(3) | /* equal */
				POLL_REG_MEM(1)); /* reg/mem */
	} else if (ring->adev->family_type == CHIP_LG200
		|| ring->adev->family_type == CHIP_LG210
		|| ring->adev->family_type == CHIP_LG211) {
		loonggpu_ring_write(ring, LG2XX_SCMD32(LG2XX_SCMD32_OP_VMID, 0));
		loonggpu_ring_write(ring, LG2XX_SCMD32(LG2XX_SCMD32_OP_POLL, 0) |
				POLL_CONDITION(3) | /* equal */
				POLL_REG_MEM(1)); /* reg/mem */
	}
	else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, ring->adev->family_type);

	loonggpu_ring_write(ring, lower_32_bits(addr));
	loonggpu_ring_write(ring, upper_32_bits(addr));
	loonggpu_ring_write(ring, seq); /* reference */
	loonggpu_ring_write(ring, 0xffffffff); /* mask */
	loonggpu_ring_write(ring, POLL_TIMES_INTERVAL(0xfff, 1)); /* retry count, interval */
}

static void gfx_ring_emit_vm_flush(struct loonggpu_ring *ring,
					unsigned vmid, uint64_t pd_addr)
{
	loonggpu_gmc_emit_flush_gpu_tlb(ring, vmid, pd_addr);
}

static void gfx_ring_emit_wreg(struct loonggpu_ring *ring, uint32_t reg,
				  uint32_t val)
{
	if (ring->adev->family_type == CHIP_LG100) 
		loonggpu_ring_write(ring, GSPKT(GSPKT_WRITE, 2) | WRITE_DST_SEL(0) | WRITE_WAIT);
	else if (ring->adev->family_type == CHIP_LG200
		|| ring->adev->family_type == CHIP_LG210
		|| ring->adev->family_type == CHIP_LG211)
		loonggpu_ring_write(ring, LG2XX_SCMD32(LG2XX_SCMD32_OP_WREG, 0));
	else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, ring->adev->family_type);

	loonggpu_ring_write(ring, reg);
	loonggpu_ring_write(ring, val);
}

static void gfx_set_gfx_eop_interrupt_state(struct loonggpu_device *adev,
						 enum loonggpu_interrupt_state state)
{
}

static int gfx_set_eop_interrupt_state(struct loonggpu_device *adev,
					    struct loonggpu_irq_src *src,
					    unsigned type,
					    enum loonggpu_interrupt_state state)
{
	gfx_set_gfx_eop_interrupt_state(adev, state);

	return 0;
}

static int gfx_eop_irq(struct loonggpu_device *adev,
			    struct loonggpu_irq_src *source,
			    struct loonggpu_iv_entry *entry)
{
	u8 me_id, pipe_id, queue_id;

	DRM_DEBUG("IH: CP EOP\n");
	me_id = (entry->ring_id & 0x0c) >> 2;
	pipe_id = (entry->ring_id & 0x03) >> 0;
	queue_id = (entry->ring_id & 0x70) >> 4;

	switch (me_id) {
	case 0:
		loonggpu_fence_process(&adev->gfx.gfx_ring[0]);
		break;
	case 1:
	case 2:
		break;
	}
	return 0;
}

static const struct loonggpu_ip_funcs gfx_ip_funcs = {
	.name = "gfx",
	.early_init = gfx_early_init,
	.late_init = gfx_late_init,
	.sw_init = gfx_sw_init,
	.sw_fini = gfx_sw_fini,
	.hw_init = gfx_hw_init,
	.hw_fini = gfx_hw_fini,
	.suspend = gfx_suspend,
	.resume = gfx_resume,
	.is_idle = gfx_is_idle,
	.wait_for_idle = gfx_wait_for_idle,
};

static struct loonggpu_ring_funcs gfx_ring_funcs_gfx_lg100 = {
	.type = LOONGGPU_RING_TYPE_GFX,
	.align_mask = 0xf,
	.nop = GSPKT(GSPKT_NOP, 0),
	.support_64bit_ptrs = false,
	.get_rptr = gfx_ring_get_rptr,
	.get_wptr = gfx_ring_get_wptr_gfx,
	.set_wptr = gfx_ring_set_wptr_gfx,
	.emit_frame_size = /* maximum 215dw if count 16 IBs in */
		7 +  /* COND_EXEC */
		1 +  /* PIPELINE_SYNC */
		VI_FLUSH_GPU_TLB_NUM_WREG * 5 + 9 + /* VM_FLUSH */
		5 +  /* FENCE for VM_FLUSH */
		3 + /* CNTX_CTRL */
		5 + 5,/* FENCE x2 */
	.emit_ib_size =	4, /* gfx_ring_emit_ib_gfx */
	.emit_ib = gfx_ring_emit_ib_gfx,
	.emit_fence = gfx_ring_emit_fence_gfx,
	.emit_pipeline_sync = gfx_ring_emit_pipeline_sync,
	.emit_vm_flush = gfx_ring_emit_vm_flush,
	.test_ring = gfx_ring_test_ring,
	.test_cs = loonggpu_pipe_ring_test_cs,
	.test_ib = gfx_ring_test_ib,
	.test_tl_ib = gfx_ring_test_tl_ib,
	.insert_nop = loonggpu_ring_insert_nop,
	.pad_ib = loonggpu_ring_generic_pad_ib,
	.emit_wreg = gfx_ring_emit_wreg,
};

static struct loonggpu_ring_funcs gfx_ring_funcs_gfx_lg2xx = {
	.type = LOONGGPU_RING_TYPE_GFX,
	.align_mask = 0xf,
	.nop = LG2XX_SCMD32(LG2XX_SCMD32_OP_NOP, 0),
	.support_64bit_ptrs = false,
	.get_rptr = gfx_ring_get_rptr,
	.get_wptr = gfx_ring_get_wptr_gfx,
	.set_wptr = gfx_ring_set_wptr_gfx,
	.emit_frame_size =
			7 +  /* COND_EXEC */
			1 +  /* PIPELINE_SYNC */
			VI_FLUSH_GPU_TLB_NUM_WREG * 5 + 9 + /* VM_FLUSH */
			5 +  /* FENCE for VM_FLUSH */
			3 + /* CNTX_CTRL */
			10 + 10, /* FENCE x2 */
	.emit_ib_size =	5, /* gfx_ring_emit_ib_gfx */
	.emit_ib = gfx_ring_emit_ib_gfx,
	.emit_fence = gfx_ring_emit_fence_gfx,
	.emit_pipeline_sync = gfx_ring_emit_pipeline_sync,
	.emit_vm_flush = gfx_ring_emit_vm_flush,
	.test_ring = gfx_ring_test_ring,
	.test_cs = loonggpu_pipe_ring_test_cs,
	.test_ib = gfx_ring_test_ib,
	.test_tl_ib = gfx_ring_test_tl_ib,
	.insert_nop = loonggpu_ring_insert_nop,
	.pad_ib = loonggpu_ring_generic_pad_ib,
	.emit_wreg = gfx_ring_emit_wreg,
};

static void gfx_set_ring_funcs(struct loonggpu_device *adev)
{
	int i;
	struct loonggpu_ring_funcs *gfx_ring_funcs_gfx;

	switch (adev->family_type)
	{
	case CHIP_LG100:
		gfx_ring_funcs_gfx = &gfx_ring_funcs_gfx_lg100;
		break;
	case CHIP_LG200:
	case CHIP_LG210:
	case CHIP_LG211:
		gfx_ring_funcs_gfx = &gfx_ring_funcs_gfx_lg2xx;
		break;
	default:
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);
		break;
	}

	for (i = 0; i < adev->gfx.num_gfx_rings; i++)
		adev->gfx.gfx_ring[i].funcs = gfx_ring_funcs_gfx;
}

static const struct loonggpu_irq_src_funcs gfx_eop_irq_funcs = {
	.set = gfx_set_eop_interrupt_state,
	.process = gfx_eop_irq,
};

static void gfx_set_irq_funcs(struct loonggpu_device *adev)
{
	adev->gfx.eop_irq.num_types = LOONGGPU_CP_IRQ_LAST;
	adev->gfx.eop_irq.funcs = &gfx_eop_irq_funcs;
}

const struct loonggpu_ip_block_version gfx_ip_block = {
	.type = LOONGGPU_IP_BLOCK_TYPE_GFX,
	.major = 8,
	.minor = 0,
	.rev = 0,
	.funcs = &gfx_ip_funcs,
};
