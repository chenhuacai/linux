// SPDX-License-Identifier: MIT
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

#include "loonggpu_lgkcd.h"

#include "loonggpu.h"
#include "loonggpu_gfx.h"
#include "loonggpu_shared.h"
#include <linux/module.h>
#include <linux/dma-buf.h>
#include "kcd_ioctl.h"


/* Total memory size in system memory and all GPU VRAM. Used to
 * estimate worst case amount of memory to reserve for page tables
 */
uint64_t loonggpu_lgkcd_total_mem_size;

static bool kcd_initialized;

int loonggpu_lgkcd_init(void)
{
	struct sysinfo si;
	int ret;

	si_meminfo(&si);
	loonggpu_lgkcd_total_mem_size = si.freeram - si.freehigh;
	loonggpu_lgkcd_total_mem_size *= si.mem_unit;

	ret = kgd2kcd_init();
	kcd_initialized = !ret;

	return ret;
}

void loonggpu_lgkcd_fini(void)
{
	if (kcd_initialized) {
		kgd2kcd_exit();
		kcd_initialized = false;
	}
}

void loonggpu_lgkcd_device_probe(struct loonggpu_device *adev)
{
	if (!kcd_initialized)
		return;

	adev->kcd.dev = kgd2kcd_probe(adev);
}

static void loonggpu_lgkcd_reset_work(struct work_struct *work)
{
	struct loonggpu_device *adev = container_of(work, struct loonggpu_device,
						  kcd.reset_work);

	loonggpu_device_gpu_recover(adev, NULL, false);
}

/**
 * loonggpu_doorbell_get_kcd_info - Report doorbell configuration required to
 *                                setup lgkcd
 *
 * @adev: loonggpu_device pointer
 * @aperture_base: output returning doorbell aperture base physical address
 * @aperture_size: output returning doorbell aperture size in bytes
 * @start_offset: output returning # of doorbell bytes reserved for loonggpu.
 *
 * loonggpu and lgkcd share the doorbell aperture. loonggpu sets it up,
 * takes doorbells required for its own rings and reports the setup to lgkcd.
 * loonggpu reserved doorbells are at the start of the doorbell aperture.
 */
static void loonggpu_doorbell_get_kcd_info(struct loonggpu_device *adev,
					 phys_addr_t *aperture_base,
					 size_t *aperture_size,
					 size_t *start_offset)
{
	if (adev->doorbell.size > adev->doorbell.num_kernel_doorbells *
						sizeof(u32)) {
		*aperture_base = adev->doorbell.base;
		*aperture_size = adev->doorbell.size;
		*start_offset = adev->doorbell.num_kernel_doorbells * sizeof(u32);
	} else {
		*aperture_base = 0;
		*aperture_size = 0;
		*start_offset = 0;
	}
}

void loonggpu_lgkcd_device_init(struct loonggpu_device *adev)
{
	int i;
	int last_valid_bit;

	loonggpu_lgkcd_gpuvm_init_mem_limits();

	if (adev->kcd.dev) {
		struct kgd2kcd_shared_resources gpu_resources = {
			.compute_vmid_bitmap =
				((1 << LOONGGPU_NUM_VMID) - 1) -
				((1 << adev->vm_manager.first_kcd_vmid) - 1),
			.num_pipe_per_mec = adev->gfx.mec.num_pipe_per_mec,
			.num_queue_per_pipe = adev->gfx.mec.num_queue_per_pipe,
			.gpuvm_size = min(adev->vm_manager.max_pfn
					  << LOONGGPU_GPU_PAGE_SHIFT,
					  LOONGGPU_VA_HOLE_START),
			.drm_render_minor = adev->ddev->render->index,
			.sdma_doorbell_idx = adev->doorbell_index.sdma_engine,
		};

		/* this is going to have a few of the MSBs set that we need to
		 * clear
		 */
		bitmap_complement(gpu_resources.cp_queue_bitmap,
				  adev->gfx.mec.queue_bitmap,
				  KGD_MAX_QUEUES);

		/* According to linux/bitmap.h we shouldn't use bitmap_clear if
		 * nbits is not compile time constant
		 */
		last_valid_bit = 1 /* only first MEC can have compute queues */
				* adev->gfx.mec.num_pipe_per_mec
				* adev->gfx.mec.num_queue_per_pipe;
		for (i = last_valid_bit; i < KGD_MAX_QUEUES; ++i)
			clear_bit(i, gpu_resources.cp_queue_bitmap);

		loonggpu_doorbell_get_kcd_info(adev,
				&gpu_resources.doorbell_physical_address,
				&gpu_resources.doorbell_aperture_size,
				&gpu_resources.doorbell_start_offset);

		gpu_resources.non_cp_doorbells_start = 0x00;
		gpu_resources.non_cp_doorbells_end = 0x1f;

		adev->kcd.init_complete = kgd2kcd_device_init(adev->kcd.dev,
							&gpu_resources);

		loonggpu_lgkcd_total_mem_size += adev->gmc.real_vram_size;

		INIT_WORK(&adev->kcd.reset_work, loonggpu_lgkcd_reset_work);
	}
}

void loonggpu_lgkcd_device_fini_sw(struct loonggpu_device *adev)
{
	if (adev->kcd.dev) {
		kgd2kcd_device_exit(adev->kcd.dev);
		adev->kcd.dev = NULL;
		loonggpu_lgkcd_total_mem_size -= adev->gmc.real_vram_size;
	}
}

void loonggpu_lgkcd_interrupt(struct loonggpu_device *adev,
		const void *ih_ring_entry)
{
	if (adev->kcd.dev)
		kgd2kcd_interrupt(adev->kcd.dev, ih_ring_entry);
}

void loonggpu_lgkcd_suspend(struct loonggpu_device *adev, bool run_pm)
{
	if (adev->kcd.dev)
		kgd2kcd_suspend(adev->kcd.dev, run_pm);
}

int loonggpu_lgkcd_resume(struct loonggpu_device *adev, bool run_pm)
{
	int r = 0;

	if (adev->kcd.dev)
		r = kgd2kcd_resume(adev->kcd.dev, run_pm);

	return r;
}

int loonggpu_lgkcd_pre_reset(struct loonggpu_device *adev)
{
	int r = 0;

	if (adev->kcd.dev)
		r = kgd2kcd_pre_reset(adev->kcd.dev);

	return r;
}

int loonggpu_lgkcd_post_reset(struct loonggpu_device *adev)
{
	int r = 0;

	if (adev->kcd.dev)
		r = kgd2kcd_post_reset(adev->kcd.dev);

	return r;
}

void loonggpu_lgkcd_gpu_reset(struct loonggpu_device *adev)
{
	loonggpu_device_gpu_recover(adev, NULL, false);
}

int loonggpu_lgkcd_alloc_gtt_mem(struct loonggpu_device *adev, size_t size,
				void **mem_obj, uint64_t *gpu_addr,
				void **cpu_ptr, bool cp_mqd_gfx9)
{
	struct loonggpu_bo *bo = NULL;
	struct loonggpu_bo_param bp;
	int r;
	void *cpu_ptr_tmp = NULL;

	memset(&bp, 0, sizeof(bp));
	bp.size = size;
	bp.byte_align = PAGE_SIZE;
	bp.domain = LOONGGPU_GEM_DOMAIN_GTT;
	bp.flags = LOONGGPU_GEM_CREATE_CPU_GTT_USWC;
	bp.type = ttm_bo_type_kernel;
	bp.resv = NULL;

	r = loonggpu_bo_create(adev, &bp, &bo);
	if (r) {
		dev_err(adev->dev,
			"failed to allocate BO for lgkcd (%d)\n", r);
		return r;
	}

	/* map the buffer */
	r = loonggpu_bo_reserve(bo, true);
	if (r) {
		dev_err(adev->dev, "(%d) failed to reserve bo for lgkcd\n", r);
		goto allocate_mem_reserve_bo_failed;
	}

	r = loonggpu_bo_pin(bo, LOONGGPU_GEM_DOMAIN_GTT);
	if (r) {
		dev_err(adev->dev, "(%d) failed to pin bo for lgkcd\n", r);
		goto allocate_mem_pin_bo_failed;
	}

	r = loonggpu_ttm_alloc_gart(&bo->tbo);
	if (r) {
		dev_err(adev->dev, "%p bind failed\n", bo);
		goto allocate_mem_kmap_bo_failed;
	}

	r = loonggpu_bo_kmap(bo, &cpu_ptr_tmp);
	if (r) {
		dev_err(adev->dev,
			"(%d) failed to map bo to kernel for lgkcd\n", r);
		goto allocate_mem_kmap_bo_failed;
	}

	*mem_obj = bo;
	*gpu_addr = loonggpu_bo_gpu_offset(bo);
	*cpu_ptr = cpu_ptr_tmp;

	lg_gbo_to_gem_obj(bo).funcs = &loonggpu_gem_object_funcs;

	loonggpu_bo_unreserve(bo);

	return 0;

allocate_mem_kmap_bo_failed:
	loonggpu_bo_unpin(bo);
allocate_mem_pin_bo_failed:
	loonggpu_bo_unreserve(bo);
allocate_mem_reserve_bo_failed:
	loonggpu_bo_unref(&bo);

	return r;
}

void loonggpu_lgkcd_free_gtt_mem(struct loonggpu_device *adev, void *mem_obj)
{
	struct loonggpu_bo *bo = (struct loonggpu_bo *) mem_obj;

	loonggpu_bo_reserve(bo, true);
	loonggpu_bo_kunmap(bo);
	loonggpu_bo_unpin(bo);
	loonggpu_bo_unreserve(bo);
	loonggpu_bo_unref(&(bo));
}

uint32_t loonggpu_lgkcd_get_fw_version(struct loonggpu_device *adev,
				      enum kgd_engine_type type)
{
	switch (type) {
	case KGD_ENGINE_MEC1:
		return adev->gfx.cp_fw_version;

	case KGD_ENGINE_SDMA1:
		return adev->xdma.instance[0].fw_version;

	default:
		return 0;
	}

	return 0;
}

void loonggpu_lgkcd_get_local_mem_info(struct loonggpu_device *adev,
				      struct kcd_local_mem_info *mem_info)
{
	memset(mem_info, 0, sizeof(*mem_info));

	mem_info->local_mem_size_public = adev->gmc.visible_vram_size;
	mem_info->local_mem_size_private = adev->gmc.real_vram_size -
					adev->gmc.visible_vram_size;

	mem_info->vram_width = adev->gmc.vram_width;

	pr_debug("Address base: %pap public 0x%llx private 0x%llx\n",
			&adev->gmc.aper_base,
			mem_info->local_mem_size_public,
			mem_info->local_mem_size_private);

	/* TODO */
	mem_info->mem_clk_max = adev->clock.default_mclk / 100;
}

uint64_t loonggpu_lgkcd_get_gpu_clock_counter(struct loonggpu_device *adev)
{
	if (adev->gfx.funcs->get_gpu_clock_counter)
		return adev->gfx.funcs->get_gpu_clock_counter(adev);
	return 0;
}

uint32_t loonggpu_lgkcd_get_max_engine_clock_in_mhz(struct loonggpu_device *adev)
{
	return adev->clock.default_sclk / 100;
}

void loonggpu_lgkcd_get_cu_info(struct loonggpu_device *adev, struct kcd_cu_info *cu_info)
{
	struct loonggpu_cu_info acu_info = adev->gfx.cu_info;

	memset(cu_info, 0, sizeof(*cu_info));

	cu_info->cu_active_number = acu_info.number;
	cu_info->cu_ao_mask = acu_info.ao_cu_mask;
	memcpy(&cu_info->cu_bitmap[0], &acu_info.bitmap[0],
	       sizeof(cu_info->cu_bitmap));

	cu_info->num_shader_engines = adev->gfx.config.max_shader_engines;
	cu_info->num_shader_arrays_per_engine = adev->gfx.config.max_sh_per_se;
	cu_info->num_cu_per_sh = adev->gfx.config.max_cu_per_sh;
	cu_info->simd_per_cu = acu_info.simd_per_cu;
	cu_info->max_waves_per_simd = acu_info.max_waves_per_simd;
	cu_info->wave_front_size = acu_info.wave_front_size;
	cu_info->max_scratch_slots_per_cu = acu_info.max_scratch_slots_per_cu;
	cu_info->lds_size = acu_info.lds_size;
}

int loonggpu_lgkcd_get_dmabuf_info(struct loonggpu_device *adev, int dma_buf_fd,
				  struct loonggpu_device **dmabuf_adev,
				  uint64_t *bo_size, void *metadata_buffer,
				  size_t buffer_size, uint32_t *metadata_size,
				  uint32_t *flags)
{
	struct dma_buf *dma_buf;
	struct drm_gem_object *obj;
	struct loonggpu_bo *bo;
	uint64_t metadata_flags;
	int r = -EINVAL;

	dma_buf = dma_buf_get(dma_buf_fd);
	if (IS_ERR(dma_buf))
		return PTR_ERR(dma_buf);

	if (dma_buf->ops != &loonggpu_dmabuf_ops)
		/* Can't handle non-graphics buffers */
		goto out_put;

	obj = dma_buf->priv;
	if (obj->dev->driver != adev_to_drm(adev)->driver)
		/* Can't handle buffers from different drivers */
		goto out_put;

	adev = drm_to_adev(obj->dev);
	bo = gem_to_loonggpu_bo(obj);
	if (!(bo->preferred_domains & (LOONGGPU_GEM_DOMAIN_VRAM |
				    LOONGGPU_GEM_DOMAIN_GTT)))
		/* Only VRAM and GTT BOs are supported */
		goto out_put;

	r = 0;
	if (dmabuf_adev)
		*dmabuf_adev = adev;
	if (bo_size)
		*bo_size = loonggpu_bo_size(bo);
	if (metadata_buffer)
		r = loonggpu_bo_get_metadata(bo, metadata_buffer, buffer_size,
					   metadata_size, &metadata_flags);
	if (flags) {
		*flags = (bo->preferred_domains & LOONGGPU_GEM_DOMAIN_VRAM) ?
				KCD_IOC_ALLOC_MEM_FLAGS_VRAM
				: KCD_IOC_ALLOC_MEM_FLAGS_GTT;

		if (bo->flags & LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED)
			*flags |= KCD_IOC_ALLOC_MEM_FLAGS_PUBLIC;
	}

out_put:
	dma_buf_put(dma_buf);
	return r;
}

int loonggpu_lgkcd_get_pcie_bandwidth_mbytes(struct loonggpu_device *adev, bool is_min)
{
	int num_lanes_factor = 0, gen_speed_mbits_factor = 0;

	num_lanes_factor = 16; /* PCIE x16 */
	if (adev->family_type == CHIP_LG210)
		num_lanes_factor = 8; /* PCIE x8 */

	gen_speed_mbits_factor = 8000; /* PCIE 3.0 */

	return (num_lanes_factor * gen_speed_mbits_factor)/BITS_PER_BYTE;
}

void loonggpu_lgkcd_set_compute_idle(struct loonggpu_device *adev, bool idle)
{
	/* TODO */
}

bool loonggpu_lgkcd_is_kcd_vmid(struct loonggpu_device *adev, u32 vmid)
{
	if (adev->kcd.dev)
		return vmid >= adev->vm_manager.first_kcd_vmid;

	return false;
}

int loonggpu_lgkcd_flush_gpu_tlb_pasid(struct loonggpu_device *adev,
				      uint16_t pasid,
				      enum TLB_FLUSH_TYPE flush_type)
{
	mb();
	/* TODO */
	loonggpu_gmc_flush_gpu_tlb(adev, 4);
	loonggpu_gmc_flush_gpu_tlb(adev, 5);
	loonggpu_gmc_flush_gpu_tlb(adev, 6);
	loonggpu_gmc_flush_gpu_tlb(adev, 7);
	return 0;
}

bool loonggpu_lgkcd_have_atomics_support(struct loonggpu_device *adev)
{
	int ret;

	if (adev->family_type < CHIP_LG210)
		return false;

	ret = pci_enable_atomic_ops_to_root(adev->pdev,
			PCI_EXP_DEVCAP2_ATOMIC_COMP32 |
			PCI_EXP_DEVCAP2_ATOMIC_COMP64);

	pr_info("lgkcd: %s atomic_ops\n",ret == 0 ? "support" : "not support");

	return !ret;
}

void loonggpu_lgkcd_debug_mem_fence(struct loonggpu_device *adev)
{
}

int loonggpu_lgkcd_send_close_event_drain_irq(struct loonggpu_device *adev,
					uint32_t *payload)
{
	int ret;

	/* Device or IH ring is not ready so bail. */
	ret = loonggpu_ih_wait_on_checkpoint_process_ts(adev, &adev->irq.ih);
	if (ret)
		return ret;

	/* Send payload to fence KCD interrupts */
	loonggpu_lgkcd_interrupt(adev, payload);

	return 0;
}

u64 loonggpu_lgkcd_real_memory_size(struct loonggpu_device *adev)
{
	return adev->gmc.real_vram_size;
}
