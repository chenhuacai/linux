#include <linux/firmware.h>
#include <drm/drm_cache.h>
#include "loonggpu.h"
#include "loonggpu_mmu.h"
#include "loonggpu_irq.h"
#include "loonggpu_helper.h"
#include "loonggpu_doorbell.h"

#define LOONGGPU_NUM_OF_VMIDS			4

static void mmu_set_gmc_funcs(struct loonggpu_device *adev);
static void mmu_set_irq_funcs(struct loonggpu_device *adev);

/**
 * mmu_vram_gtt_location  gmc locations ram info set
 *
 * @adev pionter of struct loonggpu_device
 * @mc  pionter of loonggpu_gmc
 */
static void mmu_vram_gtt_location(struct loonggpu_device *adev,
				       struct loonggpu_gmc *mc)
{
	u64 base = 0;

	/*TODO Use generic register to get vram base address*/

	/*refrence from LS7A2000 page data*/
	if (adev->chip == dev_7a2000 || adev->chip == dev_9a1000) {
		if (loonggpu_using_ram)
			base = adev->gmc.aper_base;
		else
			base = 0x1000000000000;
	}
	else if (adev->chip == dev_2k2000 || adev->chip == dev_2k3000)
		base = adev->gmc.aper_base;

	loonggpu_device_vram_location(adev, &adev->gmc, base);
	loonggpu_device_gart_location(adev, mc);
}

/**
 * mmu_mc_init - initialize the memory controller driver params
 *
 * @adev: loonggpu_device pointer
 *
 * Look up the amount of vram, vram width, and decide how to place
 * vram and gart within the GPU's physical address space ().
 * Returns 0 for success.
 */
static int mmu_mc_init(struct loonggpu_device *adev)
{
	adev->gmc.mc_mask = 0xffffffffffULL; /* 40 bit MC */

	/* size in MB on gpu ram*/
	adev->gmc.is_virtual_apu = !!loonggpu_virtual_apu;

	/*TODO 	pci_resource_start(adev->pdev, 0)
	 * 		pci_resource_len(adev->pdev, 0)
	 * */

	if (loonggpu_using_ram) {
		adev->gmc.aper_base = (unsigned long)
			loonggpu_get_vram_info(adev->dev,
				(unsigned long *)&adev->gmc.aper_size);
		if (!adev->gmc.aper_base) {
			DRM_INFO("get vram info from bios error\n");
			return -ENOMEM;
		}
		DRM_INFO("loonggpu using ram replace vram\n");
	} else {
		/* If valid system memory is passed in as the video memory,
		 * then the default is to use memory.
		 */
		adev->gmc.aper_base = (unsigned long)
			loonggpu_get_vram_info(adev->dev,
				(unsigned long *)&adev->gmc.aper_size);

		if (adev->gmc.aper_base && adev->family_type != CHIP_LG210) {
			loonggpu_using_ram = 1;
			DRM_INFO("loonggpu auto using ram replace vram\n");
		} else {
			if (adev->family_type == CHIP_LG100 ||
			    adev->family_type == CHIP_LG200 ||
			    adev->family_type == CHIP_LG211) {
				adev->gmc.aper_base =
					pci_resource_start(adev->pdev, 2);
				adev->gmc.aper_size =
					pci_resource_len(adev->pdev, 2);
			} else if (adev->family_type == CHIP_LG210) {
				adev->gmc.aper_base =
					pci_resource_start(adev->pdev, 0);
				adev->gmc.aper_size =
					pci_resource_len(adev->pdev, 0);
			}
		}
	}

	adev->gmc.mc_vram_size = adev->gmc.aper_size;
	adev->gmc.real_vram_size = adev->gmc.aper_size;
	DRM_INFO("aper_base %#llx SIZE %#llx bytes \n", adev->gmc.aper_base, adev->gmc.aper_size);
	/* In case the PCI BAR is larger than the actual amount of vram */
	adev->gmc.visible_vram_size = adev->gmc.aper_size;
	if (adev->gmc.visible_vram_size > adev->gmc.real_vram_size)
		adev->gmc.visible_vram_size = adev->gmc.real_vram_size;

	/* set the gart size */
	if (loonggpu_gart_size == -1) {
		adev->gmc.gart_size = 256ULL << 20;
	/* base = 0x1000000000000; */
	} else {
		adev->gmc.gart_size = (u64)loonggpu_gart_size << 20;
	}

	adev->gmc.vm_fault_info = kmalloc(sizeof(struct loonggpu_vm_fault_info), GFP_KERNEL);

	if (!adev->gmc.vm_fault_info)
		return -ENOMEM;

	return 0;
}

/*
 * GART
 * VMID 0 is the physical GPU addresses as used by the kernel.
 * VMIDs 1-15 are used for userspace clients and are handled
 * by the loonggpu vm/vdd code.
 */

/**
 * mmu_flush_gpu_tlb - gart tlb flush callback
 *
 * @adev: loonggpu_device pointer
 * @vmid: vm instance to flush
 *
 * Flush the TLB for the requested page table ().
 */
static void mmu_flush_gpu_tlb(struct loonggpu_device *adev,
					uint32_t vmid)
{
	if (adev->family_type == CHIP_LG100) 
		loonggpu_cmd_exec(adev, GSCMD(GSCMD_MMU, MMU_FLUSH), LOONGGPU_MMU_FLUSH_PKT(vmid, LOONGGPU_MMU_FLUSH_VMID), 0);
	else if (adev->family_type == CHIP_LG200 ||
		 adev->family_type == CHIP_LG210 ||
		 adev->family_type == CHIP_LG211)
		loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_MMU, LG2XX_ICMD32_SOP_MMU_FTLB), 
			LOONGGPU_MMU_FLUSH_PKT(vmid, LOONGGPU_MMU_FLUSH_VMID), 0);
	else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);
}


static void mmu_flush_gpu_retry(struct loonggpu_device *adev,
					uint32_t vmid, u32 port)
{
	if (adev->family_type == CHIP_LG100) 
		;
	else if (adev->family_type == CHIP_LG200 ||
		 adev->family_type == CHIP_LG210 ||
		 adev->family_type == CHIP_LG211)
		loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_MMU, LG2XX_ICMD32_SOP_MMU_RETRY),
				port << 16 | vmid << 8,
				0);
	else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);
}

static uint64_t mmu_emit_flush_gpu_tlb(struct loonggpu_ring *ring,
					    unsigned vmid, uint64_t pd_addr)
{
	uint32_t reg;
	reg = LOONGGPU_MMU_VMID_OF_PGD(vmid);

	loonggpu_ring_emit_wreg(ring, reg, lower_32_bits(pd_addr));
	loonggpu_ring_emit_wreg(ring, reg + 4, upper_32_bits(pd_addr));

	loonggpu_ring_emit_wreg(ring, LOONGGPU_MMU_FLUSH_CTRL_OFFSET, LOONGGPU_MMU_FLUSH_PKT(vmid, LOONGGPU_MMU_FLUSH_VMID));

	return pd_addr;
}

/**
 * mmu_set_pte_pde - update the page tables using MMIO
 *
 * @adev: loonggpu_device pointer
 * @cpu_pt_addr: cpu address of the page table
 * @gpu_page_idx: entry in the page table to update
 * @addr: dst addr to write into pte/pde
 * @flags: access flags
 *
 * Update the page tables using the CPU.
 */
static int mmu_set_pte_pde(struct loonggpu_device *adev, void *cpu_pt_addr,
				uint32_t gpu_page_idx, uint64_t addr,
				uint64_t flags)
{
	void __iomem *ptr = (void *)cpu_pt_addr;
	uint64_t value;

	/*
	 * PTE format:
	 * 63:40 reserved
	 * 39:12 physical page base address
	 * 7:4 zinf
	 * 3 writeable
	 * 2 exception
	 * 1 hugepage
	 * 0 present
	 *
	 */
	value = addr & 0xFFFFFFFFFFFFF000ULL;
	value |= flags;

	writeq(value, ptr + (gpu_page_idx * LOONGGPU_MMU_PTE_SIZE));

	return 0;
}

static uint64_t mmu_get_vm_pte_flags(struct loonggpu_device *adev,
					  uint32_t flags)
{
	uint64_t pte_flag = 0;

	if (flags & LOONGGPU_VM_PAGE_READABLE)
		pte_flag |= LOONGGPU_PTE_PRESENT;
	if (flags & LOONGGPU_VM_PAGE_WRITEABLE)
		pte_flag |= LOONGGPU_PTE_WRITEABLE;

	return pte_flag;
}

static void mmu_get_vm_pde(struct loonggpu_device *adev, int level,
				uint64_t *addr, uint64_t *flags)
{
}

/**
 * mmu_gart_enable - gart enable
 *
 * @adev: loonggpu_device pointer
 *
 * This sets up the TLBs, programs the page tables for VMID0,
 * sets up the hw for VMIDs 1-15 which are allocated on
 * demand, and sets up the global locations for the LDS, GDS,
 * and GPUVM for FSA64 clients ().
 * Returns 0 for success, errors for failure.
 */
static int mmu_gart_enable(struct loonggpu_device *adev)
{
	int r, i;

	if (adev->gart.robj == NULL) {
		dev_err(adev->dev, "No VRAM object for PCIE GART.\n");
		return -EINVAL;
	}
	r = loonggpu_gart_table_vram_pin(adev);
	if (r)
		return r;

	if (adev->family_type == CHIP_LG100) {
		loonggpu_cmd_exec(adev, GSCMD(GSCMD_MMU, MMU_SET_EXC), 3, 0);
		loonggpu_cmd_exec(adev, GSCMDi(GSCMD_MMU, MMU_SET_PGD, 0), \
				lower_32_bits(adev->gart.table_addr), upper_32_bits(adev->gart.table_addr));
		loonggpu_cmd_exec(adev, GSCMD(GSCMD_MMU, MMU_SET_SAFE), \
				lower_32_bits(adev->dummy_page_addr), upper_32_bits(adev->dummy_page_addr));
		loonggpu_cmd_exec(adev, GSCMDi(GSCMD_MMU, MMU_SET_DIR, 0),
				LOONGGPU_MMU_DIR_CTRL_256M_1LVL, 0);

		for (i = 1; i < LOONGGPU_NUM_OF_VMIDS; i++) {
			loonggpu_cmd_exec(adev, GSCMDi(GSCMD_MMU, MMU_SET_DIR, i),
					LOONGGPU_MMU_DIR_CTRL_1T_3LVL, 0);
		}

		loonggpu_cmd_exec(adev, GSCMD(GSCMD_MMU, MMU_ENABLE), MMU_ENABLE, ~1);
		loonggpu_cmd_exec(adev, GSCMD(GSCMD_MMU, MMU_FLUSH), LOONGGPU_MMU_FLUSH_PKT(0, LOONGGPU_MMU_FLUSH_ALL), 0);
	}
	else if (adev->family_type == CHIP_LG200 ||
		 adev->family_type == CHIP_LG210 ||
		 adev->family_type == CHIP_LG211) {
		loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_MMU, LG2XX_ICMD32_SOP_MMU_UEXP), 3, 0);
		loonggpu_cmd_exec(adev, LG2XX_ICMD32i(LG2XX_ICMD32_MOP_MMU, LG2XX_ICMD32_SOP_MMU_UDIR, 0), \
				lower_32_bits(adev->gart.table_addr), upper_32_bits(adev->gart.table_addr));
		loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_MMU, LG2XX_ICMD32_SOP_MMU_USAFE), \
				lower_32_bits(adev->dummy_page_addr), upper_32_bits(adev->dummy_page_addr));
		loonggpu_cmd_exec(adev, LG2XX_ICMD32i(LG2XX_ICMD32_MOP_MMU, LG2XX_ICMD32_SOP_MMU_UPG, 0),
				LOONGGPU_MMU_DIR_CTRL_512M_1LVL, 0);

		for (i = 1; i < LOONGGPU_NUM_OF_VMIDS + 4; i++) {
			loonggpu_cmd_exec(adev, LG2XX_ICMD32i(LG2XX_ICMD32_MOP_MMU, LG2XX_ICMD32_SOP_MMU_UPG, i),
					LOONGGPU_MMU_DIR_CTRL_1T_3LVL, 0);
		}

		loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_MMU, LG2XX_ICMD32_SOP_MMU_MMUEN), MMU_ENABLE | 
			      (loonggpu_noretry ? LOONGGPU_LG2XX_MMU_EN_FAULT : 0), ~1);
		loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_MMU, LG2XX_ICMD32_SOP_MMU_FTLB), 
				LOONGGPU_MMU_FLUSH_PKT(0, LOONGGPU_MMU_FLUSH_ALL), 0);
	}
	else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);

	DRM_INFO("PCIE GART of %uM enabled (table at 0x%016llX).\n",
		 (unsigned)(adev->gmc.gart_size >> 20),
		 (unsigned long long)adev->gart.table_addr);
	adev->gart.ready = true;
	adev->gmc.noretry = loonggpu_noretry;
	return 0;
}

static int mmu_gart_init(struct loonggpu_device *adev)
{
	int r;

	if (adev->gart.robj) {
		WARN(1, "LOONGGPU PCIE GART already initialized\n");
		return 0;
	}
	/* Initialize common gart structure */
	r = loonggpu_gart_init(adev);
	if (r)
		return r;
	adev->gart.table_size = adev->gart.num_gpu_pages * LOONGGPU_MMU_PTE_SIZE;
	adev->gart.gart_pte_flags = 0;
	return loonggpu_gart_table_vram_alloc(adev);
}

/**
 * mmu_gart_disable - gart disable
 *
 * @adev: loonggpu_device pointer
 *
 * This disables all VM page table ().
 */
static void mmu_gart_disable(struct loonggpu_device *adev)
{
	loonggpu_gart_table_vram_unpin(adev);
}

static int mmu_early_init(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	mmu_set_gmc_funcs(adev);
	mmu_set_irq_funcs(adev);

	spin_lock_init(&adev->gmc.invalidate_lock);

	return 0;
}

static int mmu_late_init(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	loonggpu_bo_late_init(adev);

	if (loonggpu_vm_fault_stop != LOONGGPU_VM_FAULT_STOP_ALWAYS)
		return loonggpu_irq_get(adev, &adev->gmc.vm_fault, 0);
	else
		return 0;
}

static inline int mmu_irq_set(struct loonggpu_device *adev)
{
	int r;
	unsigned src_id_mmu_page_fault = 0;
	unsigned src_id_mmu_port_fault = 0;

	switch (adev->family_type)
	{
	case CHIP_LG100:
		src_id_mmu_page_fault = LOONGGPU_SRCID_GFX_PAGE_INV_FAULT;
		src_id_mmu_port_fault = LOONGGPU_SRCID_GFX_MEM_PROT_FAULT;
		break;
	case CHIP_LG200:
	case CHIP_LG210:
	case CHIP_LG211:
		src_id_mmu_page_fault = LOONGGPU_LG200_SRCID_MMU_PAGE_FAULT;
		src_id_mmu_port_fault = LOONGGPU_LG200_SRCID_MMU_PORT_FAULT;
		break;
	default:
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);
		break;
	}
	r = loonggpu_irq_add_id(adev, LOONGGPU_IH_CLIENTID_LEGACY, src_id_mmu_page_fault, &adev->gmc.vm_fault);
	if (r)
		return r;

	r = loonggpu_irq_add_id(adev, LOONGGPU_IH_CLIENTID_LEGACY, src_id_mmu_port_fault, &adev->gmc.vm_fault);
	if (r)
		return r;

	return 0;
}

static inline void mmu_set_dma_consistent(struct loonggpu_device *adev)
{
	int r;
	int dma_bits;

	/* set DMA mask + need_dma32 flags.
	 * PCIE - can handle 40-bits.
	 * PCI - dma32 for legacy pci gart, 40 bits on newer asics
	 */
	adev->need_dma32 = false;
	if (adev->family_type == CHIP_LG210)
		dma_bits = 50;
	else
		dma_bits = 40;

	r = dma_set_mask(&adev->pdev->dev, DMA_BIT_MASK(dma_bits));
	if (r) {
		adev->need_dma32 = true;
		dma_bits = 32;
		pr_warn("loonggpu: No suitable DMA available\n");
	}
	r = dma_set_coherent_mask(&adev->pdev->dev, DMA_BIT_MASK(dma_bits));
	if (r) {
		dma_set_coherent_mask(&adev->pdev->dev, DMA_BIT_MASK(32));
		pr_warn("loonggpu: No coherent DMA available\n");
	}

	adev->gmc.dma_bits = dma_bits;

	if (adev->family_type != CHIP_LG210) {
		r = dma_set_mask(&adev->loongson_dc->dev, DMA_BIT_MASK(dma_bits));
		if (r)
			pr_warn("loonggpu dc: No suitable DMA available\n");

		r = dma_set_coherent_mask(&adev->loongson_dc->dev, DMA_BIT_MASK(dma_bits));
		if (r) {
			dma_set_coherent_mask(&adev->loongson_dc->dev, DMA_BIT_MASK(32));
			pr_warn("loonggpu dc: No coherent DMA available\n");
		}
	}
}

static inline int mmu_vm_manager_init(struct loonggpu_device *adev)
{

	/* Adjust VM size here.
	 * Currently set to 4GB ((1 << 20) 4k pages).
	 * Max GPUVM size for cayman and SI is 40 bits.
	 */
	loonggpu_vm_adjust_size(adev, 64, 2, 40);

	/*
	 * number of VMs
	 * VMID 0 is reserved for System
	 * loonggpu graphics/compute will use VMIDs 1-7
	 */
	adev->vm_manager.id_mgr.num_ids = LOONGGPU_NUM_OF_VMIDS;
	adev->vm_manager.first_kcd_vmid = LOONGGPU_NUM_OF_VMIDS;
	loonggpu_vm_manager_init(adev);

	/* base offset of vram pages */
	if (loonggpu_using_ram)
		adev->vm_manager.vram_base_offset = adev->gmc.aper_base;
	else
		adev->vm_manager.vram_base_offset = 0x1000000000000;

	return 0;
}

static int mmu_sw_init(void *handle)
{
	int r;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	r = mmu_irq_set(adev);
	if (r)
		return r;

	mmu_set_dma_consistent(adev);

	adev->need_swiotlb = lg_drm_need_swiotlb(adev->gmc.dma_bits);

	r = mmu_mc_init(adev);
	if (r)
		return r;

	mmu_vram_gtt_location(adev, &adev->gmc);

	/* Memory manager via ttm*/
	r = loonggpu_bo_init(adev);
	if (r)
		return r;

	r = mmu_gart_init(adev);
	if (r)
		return r;

	r = mmu_vm_manager_init(adev);
	if (r)
		return r;

	return 0;
}

static int mmu_sw_fini(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	loonggpu_gem_force_release(adev);
	loonggpu_vm_manager_fini(adev);
	kfree(adev->gmc.vm_fault_info);
	loonggpu_gart_table_vram_free(adev);
	loonggpu_bo_fini(adev);
	loonggpu_gart_fini(adev);
	release_firmware(adev->gmc.fw);
	adev->gmc.fw = NULL;

	return 0;
}

static int mmu_hw_init(void *handle)
{
	int r;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	r = mmu_gart_enable(adev);
	if (r)
		return r;

	r = loonggpu_doorbell_enable(adev, true);
	if (r)
		return r;

	return r;
}

static int mmu_hw_fini(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	loonggpu_irq_put(adev, &adev->gmc.vm_fault, 0);
	loonggpu_doorbell_disable(adev);
	mmu_gart_disable(adev);

	return 0;
}

static int mmu_suspend(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	mmu_hw_fini(adev);

	return 0;
}

static int mmu_resume(void *handle)
{
	int r;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	r = mmu_hw_init(adev);
	if (r)
		return r;

	loonggpu_vmid_reset_all(adev);

	return 0;
}

static bool mmu_is_idle(void *handle)
{
	return true;
}

static bool mmu_check_soft_reset(void *handle)
{
	return false;
}

static int mmu_pre_soft_reset(void *handle)
{
	return 0;
}

static int mmu_soft_reset(void *handle)
{
	return 0;
}

static int mmu_post_soft_reset(void *handle)
{
	return 0;
}

static int mmu_vm_fault_interrupt_state(struct loonggpu_device *adev,
					     struct loonggpu_irq_src *src,
					     unsigned type,
					     enum loonggpu_interrupt_state state)
{
	uint32_t cmd = 0;
	switch (adev->family_type) {
	case CHIP_LG100:
		cmd = GSCMD(GSCMD_MMU, MMU_SET_EXC);
		break;
	case CHIP_LG200:
	case CHIP_LG210:
	case CHIP_LG211:
		cmd = LG2XX_ICMD32(LG2XX_ICMD32_MOP_MMU, LG2XX_ICMD32_SOP_MMU_UEXP);
		break;
	default:
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);
		break;
	}

	switch (state) {
	case LOONGGPU_IRQ_STATE_DISABLE:
		loonggpu_cmd_exec(adev, cmd, 0, ~1);
		break;
	case LOONGGPU_IRQ_STATE_ENABLE:
		loonggpu_cmd_exec(adev, cmd, 1, ~1);
		break;
	default:
		break;
	}

	return 0;
}

/**
 * mmu_process_interrupt
 * TODO
 * @adev
 * @source
 * @entry
 *
 * Return:

 */
static int mmu_process_interrupt(struct loonggpu_device *adev,
				      struct loonggpu_irq_src *source,
				      struct loonggpu_iv_entry *entry)
{
	u32 addr_hi, addr_lo, status;

	addr_lo = entry->src_data[0];
	addr_hi = entry->src_data[1];
	status =  (entry->src_data[1] >> 16);

	if (printk_ratelimit()) {
		struct loonggpu_task_info task_info;

		if (!loonggpu_noretry && entry->vmid >= 4)
			return 0;
	
		loonggpu_vm_get_task_info(adev, entry->pasid, &task_info);

		dev_err(adev->dev, "GPU fault detected: %d  vmid %d pasid %d for process %s pid %d thread %s pid %d\n",
			entry->src_id, entry->vmid, entry->pasid, task_info.process_name,
			task_info.tgid, task_info.task_name, task_info.pid);
		dev_err(adev->dev, "  VM_CONTEXT1_PROTECTION_FAULT_ADDR   0x%08x%08x\n",
			addr_hi, addr_lo);
		dev_err(adev->dev, "  VM_CONTEXT1_PROTECTION_FAULT_STATUS 0x%04X\n",
			status);
	}

	return 0;
}

static void mmu_emit_pasid_mapping(struct loonggpu_ring *ring, unsigned vmid,
					unsigned pasid)
{
	/**
	 * let the firmware save the mapping
	 * relationship between vmid and pasid in DRAM
	 * By simulating a command stream.
	 * The true ways is Set regs instead of this way
	**/
	if (ring->adev->family_type == CHIP_LG100) 
		loonggpu_ring_write(ring, GSPKT(GSPKT_VM_BIND, 3));
	else if (ring->adev->family_type == CHIP_LG200 ||
		 ring->adev->family_type == CHIP_LG210 ||
		 ring->adev->family_type == CHIP_LG211)
		loonggpu_ring_write(ring, LG2XX_SCMD32(LG2XX_SCMD32_OP_VM_BIND, 0));
	else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, ring->adev->family_type);

	loonggpu_ring_write(ring, ring->funcs->type);
	loonggpu_ring_write(ring, vmid);
	loonggpu_ring_write(ring, pasid);
}

static const struct loonggpu_ip_funcs mmu_ip_funcs = {
	.name = "mmu",
	.early_init = mmu_early_init,
	.late_init = mmu_late_init,
	.sw_init = mmu_sw_init,
	.sw_fini = mmu_sw_fini,
	.hw_init = mmu_hw_init,
	.hw_fini = mmu_hw_fini,
	.suspend = mmu_suspend,
	.resume = mmu_resume,
	.is_idle = mmu_is_idle,
	.wait_for_idle = NULL,
	.check_soft_reset = mmu_check_soft_reset,
	.pre_soft_reset = mmu_pre_soft_reset,
	.soft_reset = mmu_soft_reset,
	.post_soft_reset = mmu_post_soft_reset,
};

static const struct loonggpu_gmc_funcs mmu_gmc_funcs = {
	.flush_gpu_tlb = mmu_flush_gpu_tlb,
	.emit_flush_gpu_tlb = mmu_emit_flush_gpu_tlb,
	.emit_pasid_mapping = mmu_emit_pasid_mapping,
	.set_pte_pde = mmu_set_pte_pde,
	.get_vm_pte_flags = mmu_get_vm_pte_flags,
	.get_vm_pde = mmu_get_vm_pde,
	.flush_gpu_retry = mmu_flush_gpu_retry
};

static const struct loonggpu_irq_src_funcs mmu_irq_funcs = {
	.set = mmu_vm_fault_interrupt_state,
	.process = mmu_process_interrupt,
};

static void mmu_set_gmc_funcs(struct loonggpu_device *adev)
{
	if (adev->gmc.gmc_funcs == NULL)
		adev->gmc.gmc_funcs = &mmu_gmc_funcs;
}

static void mmu_set_irq_funcs(struct loonggpu_device *adev)
{
	adev->gmc.vm_fault.num_types = 1;
	adev->gmc.vm_fault.funcs = &mmu_irq_funcs;
}
const struct loonggpu_ip_block_version mmu_ip_block = {
	.type = LOONGGPU_IP_BLOCK_TYPE_GMC,
	.major = 1,
	.minor = 0,
	.rev = 0,
	.funcs = &mmu_ip_funcs,
};
