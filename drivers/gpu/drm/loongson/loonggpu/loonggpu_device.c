#include <linux/efi.h>
#include <linux/console.h>
#include <linux/firmware.h>
#include <linux/kthread.h>
#include <linux/pci.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/vgaarb.h>
#include <linux/vga_switcheroo.h>
#include "loonggpu.h"
#include <asm/loongson.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_atomic_helper.h>
#include "loonggpu_drm.h"
#include "loonggpu_trace.h"
#include "loonggpu_cp.h"
#include "loonggpu_common.h"
#include "loonggpu_xdma.h"
#include "loonggpu_pm.h"
#include "loonggpu_helper.h"
#include "loonggpu_lgkcd.h"
#include "loonggpu_backlight.h"
#include "loonggpu_gtt_mgr_helper.h"
#include "loonggpu_dc_hdmi.h"
#include "loonggpu_dc_crtc.h"
#include "loonggpu_dc_vbios.h"

#define LOONGGPU_RESUME_MS		2000

const char *loonggpu_family_name[] = {
	"LoongDC",
	"LG100",
	"LG200",
	"LG210",
	"LG211",
};

/**
 * loonggpu_cmd_exec
 * XXX while block will taking kernel hang!
 * @adev
 * @cmd
 * @arg0
 * @arg1
 *
 * Return:

 */
uint64_t loonggpu_cmd_exec(struct loonggpu_device *adev, uint32_t cmd, uint32_t arg0, uint32_t arg1)
{
	uint64_t ret;

	if (loonggpu_cp_wait_done(adev) == false)
		return  ~0ULL;

	writel(GSCMD_STS_NULL, ((void __iomem *)adev->rmmio) + LOONGGPU_STATUS);

	writel(cmd, ((void __iomem *)adev->rmmio) + LOONGGPU_COMMAND);
	writel(arg0, ((void __iomem *)adev->rmmio) + LOONGGPU_ARGUMENT0);
	writel(arg1, ((void __iomem *)adev->rmmio) + LOONGGPU_ARGUMENT1);

	writel(1, ((void __iomem *)adev->rmmio) + LOONGGPU_EC_INT);

	if (loonggpu_cp_wait_done(adev) == false)
		return  ~0ULL;

	ret = readl(((void __iomem *)adev->rmmio) + LOONGGPU_RETURN0);
	ret |= (uint64_t)readl(((void __iomem *)adev->rmmio) + LOONGGPU_RETURN1)<<32;

	return ret;
}

/*
 * MMIO register access helper functions.
 */
/**
 * loonggpu_mm_rreg - read a memory mapped IO register
 *
 * @adev: loonggpu_device pointer
 * @reg: dword aligned register offset
 * @acc_flags: access flags which require special behavior
 *
 * Returns the 32 bit value from the offset specified.
 */
uint32_t loonggpu_mm_rreg(struct loonggpu_device *adev, uint32_t reg,
			uint32_t acc_flags)
{
	uint32_t ret;

	if (reg < adev->rmmio_size && !(acc_flags & LOONGGPU_REGS_IDX))
		ret = readl(((void __iomem *)adev->rmmio) + reg);
	else {
		unsigned long flags;

		spin_lock_irqsave(&adev->mmio_idx_lock, flags);
		ret = 0;
		DRM_DEBUG_DRIVER("%s Not implemented\n", __func__);
		spin_unlock_irqrestore(&adev->mmio_idx_lock, flags);
	}
	trace_loonggpu_mm_rreg(adev->pdev->device, reg, ret);
	return ret;
}

/*
 * MMIO register read with bytes helper functions
 * @offset:bytes offset from MMIO start
 *
*/

/**
 * loonggpu_mm_rreg8 - read a memory mapped IO register
 *
 * @adev: loonggpu_device pointer
 * @offset: byte aligned register offset
 *
 * Returns the 8 bit value from the offset specified.
 */
uint8_t loonggpu_mm_rreg8(struct loonggpu_device *adev, uint32_t offset)
{
	if (offset < adev->rmmio_size)
		return (readb(adev->rmmio + offset));
	BUG();
}

/*
 * MMIO register write with bytes helper functions
 * @offset:bytes offset from MMIO start
 * @value: the value want to be written to the register
 *
*/
/**
 * loonggpu_mm_wreg8 - read a memory mapped IO register
 *
 * @adev: loonggpu_device pointer
 * @offset: byte aligned register offset
 * @value: 8 bit value to write
 *
 * Writes the value specified to the offset specified.
 */
void loonggpu_mm_wreg8(struct loonggpu_device *adev, uint32_t offset, uint8_t value)
{
	if (offset < adev->rmmio_size)
		writeb(value, adev->rmmio + offset);
	else
		BUG();
}

/**
 * loonggpu_mm_wreg - write to a memory mapped IO register
 *
 * @adev: loonggpu_device pointer
 * @reg: dword aligned register offset
 * @v: 32 bit value to write to the register
 * @acc_flags: access flags which require special behavior
 *
 * Writes the value specified to the offset specified.
 */
void loonggpu_mm_wreg(struct loonggpu_device *adev, uint32_t reg, uint32_t v)
{
	trace_loonggpu_mm_wreg(adev->pdev->device, reg, v);

	if ((reg) < adev->rmmio_size) {
		writel(v, ((void __iomem *)adev->rmmio) + reg);
	} else
		BUG();
}

/**
 * loonggpu_device_vram_scratch_init - allocate the VRAM scratch page
 *
 * @adev: loonggpu device pointer
 *
 * Allocates a scratch page of VRAM for use by various things in the
 * driver.
 */
static int loonggpu_device_vram_scratch_init(struct loonggpu_device *adev)
{
	return loonggpu_bo_create_kernel(adev, LOONGGPU_GPU_PAGE_SIZE,
				       PAGE_SIZE, LOONGGPU_GEM_DOMAIN_VRAM,
				       &adev->vram_scratch.robj,
				       &adev->vram_scratch.gpu_addr,
				       (void **)&adev->vram_scratch.ptr);
}

/**
 * loonggpu_device_vram_scratch_fini - Free the VRAM scratch page
 *
 * @adev: loonggpu device pointer
 *
 * Frees the VRAM scratch page.
 */
static void loonggpu_device_vram_scratch_fini(struct loonggpu_device *adev)
{
	loonggpu_bo_free_kernel(&adev->vram_scratch.robj, NULL, NULL);
}

/**
 * loonggpu_device_program_register_sequence - program an array of registers.
 *
 * @adev: loonggpu_device pointer
 * @registers: pointer to the register array
 * @array_size: size of the register array
 *
 * Programs an array or registers with and and or masks.
 * This is a helper for setting golden registers.
 */
void loonggpu_device_program_register_sequence(struct loonggpu_device *adev,
					     const u32 *registers,
					     const u32 array_size)
{
	u32 tmp, reg, and_mask, or_mask;
	int i;

	if (array_size % 3)
		return;

	for (i = 0; i < array_size; i += 3) {
		reg = registers[i + 0];
		and_mask = registers[i + 1];
		or_mask = registers[i + 2];

		if (and_mask == 0xffffffff) {
			tmp = or_mask;
		} else {
			tmp = RREG32(reg);
			tmp &= ~and_mask;
			tmp |= or_mask;
		}
		WREG32(reg, tmp);
	}
}

/**
 * loonggpu_device_pci_config_reset - reset the GPU
 *
 * @adev: loonggpu_device pointer
 *
 * Resets the GPU using the pci config reset sequence.
 * Only applicable to asics prior to vega10.
 */
void loonggpu_device_pci_config_reset(struct loonggpu_device *adev)
{
	pci_write_config_dword(adev->pdev, 0x7c, LOONGGPU_ASIC_RESET_DATA);
}

/*
 * loonggpu_device_wb_*()
 * Writeback is the method by which the GPU updates special pages in memory
 * with the status of certain GPU events (fences, ring pointers,etc.).
 */

/**
 * loonggpu_device_wb_fini - Disable Writeback and free memory
 *
 * @adev: loonggpu_device pointer
 *
 * Disables Writeback and frees the Writeback memory (all asics).
 * Used at driver shutdown.
 */
static void loonggpu_device_wb_fini(struct loonggpu_device *adev)
{
	if (adev->wb.wb_obj) {
		loonggpu_bo_free_kernel(&adev->wb.wb_obj,
				      &adev->wb.gpu_addr,
				      (void **)&adev->wb.wb);
		adev->wb.wb_obj = NULL;
	}
}

/**
 * loonggpu_device_wb_init- Init Writeback driver info and allocate memory
 *
 * @adev: loonggpu_device pointer
 *
 * Initializes writeback and allocates writeback memory (all asics).
 * Used at driver startup.
 * Returns 0 on success or an -error on failure.
 */
static int loonggpu_device_wb_init(struct loonggpu_device *adev)
{
	int r;

	if (adev->wb.wb_obj == NULL) {
		/* LOONGGPU_MAX_WB * sizeof(uint32_t) * 8 = LOONGGPU_MAX_WB 256bit slots */
		r = loonggpu_bo_create_kernel(adev, LOONGGPU_MAX_WB * sizeof(uint32_t) * 8,
					    PAGE_SIZE, LOONGGPU_GEM_DOMAIN_GTT,
					    &adev->wb.wb_obj, &adev->wb.gpu_addr,
					    (void **)&adev->wb.wb);
		if (r) {
			dev_warn(adev->dev, "(%d) create WB bo failed\n", r);
			return r;
		}

		adev->wb.num_wb = LOONGGPU_MAX_WB;
		memset(&adev->wb.used, 0, sizeof(adev->wb.used));

		/* clear wb memory */
		memset((char *)adev->wb.wb, 0, LOONGGPU_MAX_WB * sizeof(uint32_t) * 8);
	}

	return 0;
}

/**
 * loonggpu_device_wb_get - Allocate a wb entry
 *
 * @adev: loonggpu_device pointer
 * @wb: wb index
 *
 * Allocate a wb slot for use by the driver (all asics).
 * Returns 0 on success or -EINVAL on failure.
 */
int loonggpu_device_wb_get(struct loonggpu_device *adev, u32 *wb)
{
	unsigned long offset = find_first_zero_bit(adev->wb.used, adev->wb.num_wb);

	if (offset < adev->wb.num_wb) {
		__set_bit(offset, adev->wb.used);
		*wb = offset << 3; /* convert to dw offset */
		return 0;
	} else {
		return -EINVAL;
	}
}

/**
 * loonggpu_device_wb_free - Free a wb entry
 *
 * @adev: loonggpu_device pointer
 * @wb: wb index
 *
 * Free a wb slot allocated for use by the driver (all asics)
 */
void loonggpu_device_wb_free(struct loonggpu_device *adev, u32 wb)
{
	wb >>= 3;
	if (wb < adev->wb.num_wb)
		__clear_bit(wb, adev->wb.used);
}

/**
 * loonggpu_device_vram_location - try to find VRAM location
 *
 * @adev: loonggpu device structure holding all necessary informations
 * @mc: memory controller structure holding memory informations
 * @base: base address at which to put VRAM
 *
 * Function will try to place VRAM at base address provided
 * as parameter.
 */
void loonggpu_device_vram_location(struct loonggpu_device *adev,
				 struct loonggpu_gmc *mc, u64 base)
{
	uint64_t limit = (uint64_t)loonggpu_vram_limit << 20;

	mc->vram_start = base;
	mc->vram_end = mc->vram_start + mc->mc_vram_size - 1;
	if (limit && limit < mc->real_vram_size)
		mc->real_vram_size = limit;
	dev_info(adev->dev, "Video RAM: %lluM 0x%016llX - 0x%016llX (%lluM used)\n",
			mc->mc_vram_size >> 20, mc->vram_start,
			mc->vram_end, mc->real_vram_size >> 20);
}

/**
 * loonggpu_device_gart_location - try to find GART location
 *
 * @adev: loonggpu device structure holding all necessary informations
 * @mc: memory controller structure holding memory informations
 *
 * Function will place try to place GART before or after VRAM.
 *
 * If GART size is bigger than space left then we ajust GART size.
 * Thus function will never fails.
 */
void loonggpu_device_gart_location(struct loonggpu_device *adev,
				 struct loonggpu_gmc *mc)
{
	u64 size_af, size_bf;

	size_af = adev->gmc.mc_mask - mc->vram_end;
	size_bf = mc->vram_start;
	if (size_bf > size_af) {
		if (mc->gart_size > size_bf) {
			dev_warn(adev->dev, "limiting GART\n");
			mc->gart_size = size_bf;
		}
		mc->gart_start = 0;
		mc->gart_size = 256 << 20;
	} else {
		if (mc->gart_size > size_af) {
			dev_warn(adev->dev, "limiting GART\n");
			mc->gart_size = size_af;
		}
		/* VCE doesn't like it when BOs cross a 4GB segment, so align */
		/* the GART base on a 4GB boundary as well. */
		mc->gart_start = ALIGN(mc->vram_end + 1, 0x100000000ULL);
	}

	mc->gart_start = 0;

#if (LOONGGPU_GPU_PAGE_SIZE == SZ_4K)
	/* HW limit 128MB virtual address when using 1 level page-table */
	mc->gart_size = 128ULL << 20;
#endif

#if (LOONGGPU_GPU_PAGE_SIZE == SZ_16K)
	mc->gart_size = 256ULL << 20;
#endif

	mc->gart_end = mc->gart_start + mc->gart_size - 1;
	dev_info(adev->dev, "GART: %lluM 0x%016llX - 0x%016llX\n",
			mc->gart_size >> 20, mc->gart_start, mc->gart_end);
}

/**
 * loonggpu_device_resize_fb_bar - try to resize FB BAR
 *
 * @adev: loonggpu_device pointer
 *
 * Try to resize FB BAR to make all VRAM CPU accessible. We try very hard not
 * to fail, but if any of the BARs is not accessible after the size we abort
 * driver loading by returning -ENODEV.
 */
int loonggpu_device_resize_fb_bar(struct loonggpu_device *adev)
{
	u64 space_needed = roundup_pow_of_two(adev->gmc.real_vram_size);
	u32 rbar_size = order_base_2(((space_needed >> 20) | 1)) - 1;
	struct pci_bus *root;
	struct resource *res;
	unsigned i;
	u16 cmd;
	int r;

	/* Check if the root BUS has 64bit memory resources */
	root = adev->pdev->bus;
	while (root->parent)
		root = root->parent;

	pci_bus_for_each_resource(root, res, i) {
		if (res && res->flags & (IORESOURCE_MEM | IORESOURCE_MEM_64) &&
		    res->start > 0x100000000ull)
			break;
	}

	/* Trying to resize is pointless without a root hub window above 4GB */
	if (!res)
		return 0;

	/* Disable memory decoding while we change the BAR addresses and size */
	pci_read_config_word(adev->pdev, PCI_COMMAND, &cmd);
	pci_write_config_word(adev->pdev, PCI_COMMAND,
			      cmd & ~PCI_COMMAND_MEMORY);

	pci_release_resource(adev->pdev, 3);

	pci_release_resource(adev->pdev, 0);

	r = lg_pci_resize_resource(adev->pdev, 0, rbar_size, 0);
	if (r == -ENOSPC)
		DRM_INFO("Not enough PCI address space for a large BAR.");
	else if (r && r != -ENOTSUPP)
		DRM_ERROR("Problem resizing BAR0 (%d).", r);

	pci_assign_unassigned_bus_resources(adev->pdev->bus);

	/* When the fb BAR isn't available we have no chance of
	 * using the device.
	 */
	if ((pci_resource_flags(adev->pdev, 0) & IORESOURCE_UNSET))
		return -ENODEV;

	pci_write_config_word(adev->pdev, PCI_COMMAND, cmd);

	return 0;
}

/*
 * GPU helpers function.
 */
/**
 * loonggpu_device_need_post - check if the hw need post or not
 *
 * @adev: loonggpu_device pointer
 *
 * Check if the asic has been initialized (all asics) at driver startup
 * or post is needed if  hw reset is performed.
 * Returns true if need or false if not.
 */
bool loonggpu_device_need_post(struct loonggpu_device *adev)
{
	if (adev->has_hw_reset) {
		adev->has_hw_reset = false;
	}

	return true;
}

/* Check if hw access should be skipped because of hotplug or device error */
bool loonggpu_device_skip_hw_access(struct loonggpu_device *adev)
{
	WARN_ON(!adev->doorbell.is_enabled);

	return adev->doorbell.is_enabled;
}

/* if we get transitioned to only one device, take VGA back */
/**
 * loonggpu_device_vga_set_decode - enable/disable vga decode
 *
 * @cookie: loonggpu_device pointer
 * @state: enable/disable vga decode
 *
 * Enable/disable vga decode (all asics).
 * Returns VGA resource flags.
 */
static unsigned int loonggpu_device_vga_set_decode(void *data, bool state)
{
	struct loonggpu_device *adev = data;
	loonggpu_asic_set_vga_state(adev, state);
	if (state)
		return VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM |
		       VGA_RSRC_NORMAL_IO | VGA_RSRC_NORMAL_MEM;
	else
		return VGA_RSRC_NORMAL_IO | VGA_RSRC_NORMAL_MEM;
}

/**
 * loonggpu_device_check_block_size - validate the vm block size
 *
 * @adev: loonggpu_device pointer
 *
 * Validates the vm block size specified via module parameter.
 * The vm block size defines number of bits in page table versus page directory,
 * a page is 4KB so we have 12 bits offset, minimum 9 bits in the
 * page table and the remaining bits are in the page directory.
 */
static void loonggpu_device_check_block_size(struct loonggpu_device *adev)
{
	/* defines number of bits in page table versus page directory,
	 * a page is 4KB so we have 12 bits offset, minimum 9 bits in the
	 * page table and the remaining bits are in the page directory */
	if (loonggpu_vm_block_size == -1)
		return;

	if (loonggpu_vm_block_size < LOONGGPU_PAGE_PTE_SHIFT) {
		dev_warn(adev->dev, "VM page table size (%d) too small\n",
			 loonggpu_vm_block_size);
		loonggpu_vm_block_size = -1;
	}
}

/**
 * loonggpu_device_check_vm_size - validate the vm size
 *
 * @adev: loonggpu_device pointer
 *
 * Validates the vm size in GB specified via module parameter.
 * The VM size is the size of the GPU virtual memory space in GB.
 */
static void loonggpu_device_check_vm_size(struct loonggpu_device *adev)
{
	/* no need to check the default value */
	if (loonggpu_vm_size == -1)
		return;

	if (loonggpu_vm_size < 1) {
		dev_warn(adev->dev, "VM size (%d) too small, min is 1GB\n",
			 loonggpu_vm_size);
		loonggpu_vm_size = -1;
	}
}

/**
 * loonggpu_device_check_arguments - validate module params
 *
 * @adev: loonggpu_device pointer
 *
 * Validates certain module parameters and updates
 * the associated values used by the driver (all asics).
 */
static void loonggpu_device_check_arguments(struct loonggpu_device *adev)
{
	if (loonggpu_sched_jobs < 4) {
		dev_warn(adev->dev, "sched jobs (%d) must be at least 4\n",
			loonggpu_sched_jobs);
		loonggpu_sched_jobs = 4;
	} else if (!is_power_of_2(loonggpu_sched_jobs)) {
		dev_warn(adev->dev, "sched jobs (%d) must be a power of 2\n",
			 loonggpu_sched_jobs);
		loonggpu_sched_jobs = roundup_pow_of_two(loonggpu_sched_jobs);
	}

	if (loonggpu_gart_size != -1 && loonggpu_gart_size < 32) {
		/* gart size must be greater or equal to 32M */
		dev_warn(adev->dev, "gart size (%d) too small\n",
			 loonggpu_gart_size);
		loonggpu_gart_size = -1;
	}

	if (loonggpu_gtt_size != -1 && loonggpu_gtt_size < 32) {
		/* gtt size must be greater or equal to 32M */
		dev_warn(adev->dev, "gtt size (%d) too small\n",
				 loonggpu_gtt_size);
		loonggpu_gtt_size = -1;
	}

	loonggpu_device_check_vm_size(adev);

	loonggpu_device_check_block_size(adev);

	if (loonggpu_vram_page_split != -1 && (loonggpu_vram_page_split < 16 ||
	    !is_power_of_2(loonggpu_vram_page_split))) {
		dev_warn(adev->dev, "invalid VRAM page split (%d)\n",
			 loonggpu_vram_page_split);
		loonggpu_vram_page_split = 2048;
	}

	if (loonggpu_lockup_timeout == 0) {
		dev_warn(adev->dev, "lockup_timeout msut be > 0, adjusting to 10000\n");
		loonggpu_lockup_timeout = 10000;
	}

	if (loonggpu_gfx_lockup_timeout == 0) {
		dev_warn(adev->dev, "gfx_lockup_timeout msut be > 0, adjusting to 2000\n");
		loonggpu_gfx_lockup_timeout = 2000;
	}
}

/**
 * loonggpu_switcheroo_set_state - set switcheroo state
 *
 * @pdev: pci dev pointer
 * @state: vga_switcheroo state
 *
 * Callback for the switcheroo driver.  Suspends or resumes the
 * the asics before or after it is powered up using ACPI methods.
 */
static void loonggpu_switcheroo_set_state(struct pci_dev *pdev, enum vga_switcheroo_state state)
{
	struct drm_device *dev = pci_get_drvdata(pdev);

	if (state == VGA_SWITCHEROO_OFF)
		return;

	if (state == VGA_SWITCHEROO_ON) {
		pr_info("loonggpu: switched on\n");
		/* don't suspend or resume card normally */
		dev->switch_power_state = DRM_SWITCH_POWER_CHANGING;

		loonggpu_device_resume(dev, true, true);

		dev->switch_power_state = DRM_SWITCH_POWER_ON;
		drm_kms_helper_poll_enable(dev);
	} else {
		pr_info("loonggpu: switched off\n");
		drm_kms_helper_poll_disable(dev);
		dev->switch_power_state = DRM_SWITCH_POWER_CHANGING;
		loonggpu_device_suspend(dev, true, true);
		dev->switch_power_state = DRM_SWITCH_POWER_OFF;
	}
}

/**
 * loonggpu_switcheroo_can_switch - see if switcheroo state can change
 *
 * @pdev: pci dev pointer
 *
 * Callback for the switcheroo driver.  Check of the switcheroo
 * state can be changed.
 * Returns true if the state can be changed, false if not.
 */
static bool loonggpu_switcheroo_can_switch(struct pci_dev *pdev)
{
	struct drm_device *dev = pci_get_drvdata(pdev);

	/*
	* FIXME: open_count is protected by drm_global_mutex but that would lead to
	* locking inversion with the driver load path. And the access here is
	* completely racy anyway. So don't bother with locking for now.
	*/
	return lg_atomic_read_drm_open_count(dev) == 0;
}

static const struct vga_switcheroo_client_ops loonggpu_switcheroo_ops = {
	.set_gpu_state = loonggpu_switcheroo_set_state,
	.can_switch = loonggpu_switcheroo_can_switch,
};

/**
 * loonggpu_device_ip_wait_for_idle - wait for idle
 *
 * @adev: loonggpu_device pointer
 * @block_type: Type of hardware IP (SMU, GFX, UVD, etc.)
 *
 * Waits for the request hardware IP to be idle.
 * Returns 0 for success or a negative error code on failure.
 */
int loonggpu_device_ip_wait_for_idle(struct loonggpu_device *adev,
				   enum loonggpu_ip_block_type block_type)
{
	int i, r;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (!adev->ip_blocks[i].status.valid)
			continue;
		if (adev->ip_blocks[i].version->type == block_type) {
			r = adev->ip_blocks[i].version->funcs->wait_for_idle((void *)adev);
			if (r)
				return r;
			break;
		}
	}
	return 0;

}

/**
 * loonggpu_device_ip_is_idle - is the hardware IP idle
 *
 * @adev: loonggpu_device pointer
 * @block_type: Type of hardware IP (SMU, GFX, UVD, etc.)
 *
 * Check if the hardware IP is idle or not.
 * Returns true if it the IP is idle, false if not.
 */
bool loonggpu_device_ip_is_idle(struct loonggpu_device *adev,
			      enum loonggpu_ip_block_type block_type)
{
	int i;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (!adev->ip_blocks[i].status.valid)
			continue;
		if (adev->ip_blocks[i].version->type == block_type)
			return adev->ip_blocks[i].version->funcs->is_idle((void *)adev);
	}
	return true;

}

/**
 * loonggpu_device_ip_get_ip_block - get a hw IP pointer
 *
 * @adev: loonggpu_device pointer
 * @type: Type of hardware IP (SMU, GFX, UVD, etc.)
 *
 * Returns a pointer to the hardware IP block structure
 * if it exists for the asic, otherwise NULL.
 */
struct loonggpu_ip_block *
loonggpu_device_ip_get_ip_block(struct loonggpu_device *adev,
			      enum loonggpu_ip_block_type type)
{
	int i;

	for (i = 0; i < adev->num_ip_blocks; i++)
		if (adev->ip_blocks[i].version->type == type)
			return &adev->ip_blocks[i];

	return NULL;
}

/**
 * loonggpu_device_ip_block_version_cmp
 *
 * @adev: loonggpu_device pointer
 * @type: enum loonggpu_ip_block_type
 * @major: major version
 * @minor: minor version
 *
 * return 0 if equal or greater
 * return 1 if smaller or the ip_block doesn't exist
 */
int loonggpu_device_ip_block_version_cmp(struct loonggpu_device *adev,
				       enum loonggpu_ip_block_type type,
				       u32 major, u32 minor)
{
	struct loonggpu_ip_block *ip_block = loonggpu_device_ip_get_ip_block(adev, type);

	if (ip_block && ((ip_block->version->major > major) ||
			((ip_block->version->major == major) &&
			(ip_block->version->minor >= minor))))
		return 0;

	return 1;
}

/**
 * loonggpu_device_ip_block_add
 *
 * @adev: loonggpu_device pointer
 * @ip_block_version: pointer to the IP to add
 *
 * Adds the IP block driver information to the collection of IPs
 * on the asic.
 */
int loonggpu_device_ip_block_add(struct loonggpu_device *adev,
			       const struct loonggpu_ip_block_version *ip_block_version)
{
	if (!ip_block_version)
		return -EINVAL;

	DRM_DEBUG_DRIVER("add ip block number %d <%s>\n", adev->num_ip_blocks,
			 ip_block_version->funcs->name);

	adev->ip_blocks[adev->num_ip_blocks++].version = ip_block_version;

	return 0;
}

/**
 * loonggpu_device_ip_early_init - run early init for hardware IPs
 *
 * @adev: loonggpu_device pointer
 *
 * Early initialization pass for hardware IPs.  The hardware IPs that make
 * up each asic are discovered each IP's early_init callback is run.  This
 * is the first stage in initializing the asic.
 * Returns 0 on success, negative error code on failure.
 */
static int loonggpu_device_ip_early_init(struct loonggpu_device *adev)
{
	int i, r;

	r = loonggpu_set_ip_blocks(adev);
	if (r)
		return r;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (adev->ip_blocks[i].version->funcs->early_init) {
			r = adev->ip_blocks[i].version->funcs->early_init((void *)adev);
			if (r == -ENOENT) {
				adev->ip_blocks[i].status.valid = false;
			} else if (r) {
				DRM_ERROR("early_init of IP block <%s> failed %d\n",
					  adev->ip_blocks[i].version->funcs->name, r);
				return r;
			} else {
				adev->ip_blocks[i].status.valid = true;
			}
		} else {
			adev->ip_blocks[i].status.valid = true;
		}
	}

	loonggpu_lgkcd_device_probe(adev);

	return 0;
}

/**
 * loonggpu_device_ip_init - run init for hardware IPs
 *
 * @adev: loonggpu_device pointer
 *
 * Main initialization pass for hardware IPs.  The list of all the hardware
 * IPs that make up the asic is walked and the sw_init and hw_init callbacks
 * are run.  sw_init initializes the software state associated with each IP
 * and hw_init initializes the hardware associated with each IP.
 * Returns 0 on success, negative error code on failure.
 */
static int loonggpu_device_ip_init(struct loonggpu_device *adev)
{
	int i, r;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (!adev->ip_blocks[i].status.valid)
			continue;
		r = adev->ip_blocks[i].version->funcs->sw_init((void *)adev);
		if (r) {
			DRM_ERROR("sw_init of IP block <%s> failed %d\n",
				  adev->ip_blocks[i].version->funcs->name, r);
			return r;
		}
		adev->ip_blocks[i].status.sw = true;

		/* need to do gmc hw init early so we can allocate gpu mem */
		if (adev->ip_blocks[i].version->type == LOONGGPU_IP_BLOCK_TYPE_GMC) {
			r = loonggpu_device_vram_scratch_init(adev);
			if (r) {
				DRM_ERROR("loonggpu_vram_scratch_init failed %d\n", r);
				return r;
			}
			r = adev->ip_blocks[i].version->funcs->hw_init((void *)adev);
			if (r) {
				DRM_ERROR("hw_init %d failed %d\n", i, r);
				return r;
			}
			r = loonggpu_device_wb_init(adev);
			if (r) {
				DRM_ERROR("loonggpu_device_wb_init failed %d\n", r);
				return r;
			}
			adev->ip_blocks[i].status.hw = true;
		}
	}

	if (adev->family_type != CHIP_NO_GPU)
		r = loonggpu_ib_pool_init(adev);

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (!adev->ip_blocks[i].status.sw)
			continue;
		if (adev->ip_blocks[i].status.hw)
			continue;
		r = adev->ip_blocks[i].version->funcs->hw_init((void *)adev);
		if (r) {
			DRM_ERROR("hw_init of IP block <%s> failed %d\n",
				  adev->ip_blocks[i].version->funcs->name, r);
			return r;
		}
		adev->ip_blocks[i].status.hw = true;
	}

	if (adev->family_type != CHIP_NO_GPU)
		loonggpu_lgkcd_device_init(adev);

	return 0;
}

/**
 * loonggpu_device_fill_reset_magic - writes reset magic to gart pointer
 *
 * @adev: loonggpu_device pointer
 *
 * Writes a reset magic value to the gart pointer in VRAM.  The driver calls
 * this function before a GPU reset.  If the value is retained after a
 * GPU reset, VRAM has not been lost.  Some GPU resets may destry VRAM contents.
 */
static void loonggpu_device_fill_reset_magic(struct loonggpu_device *adev)
{
	memcpy(adev->reset_magic, adev->gart.ptr, LOONGGPU_RESET_MAGIC_NUM);
}

/**
 * loonggpu_device_check_vram_lost - check if vram is valid
 *
 * @adev: loonggpu_device pointer
 *
 * Checks the reset magic value written to the gart pointer in VRAM.
 * The driver calls this after a GPU reset to see if the contents of
 * VRAM is lost or now.
 * returns true if vram is lost, false if not.
 */
static bool loonggpu_device_check_vram_lost(struct loonggpu_device *adev)
{
	return !!memcmp(adev->gart.ptr, adev->reset_magic,
			LOONGGPU_RESET_MAGIC_NUM);
}

/**
 * loonggpu_device_ip_late_init - run late init for hardware IPs
 *
 * @adev: loonggpu_device pointer
 *
 * Late initialization pass for hardware IPs.  The list of all the hardware
 * IPs that make up the asic is walked and the late_init callbacks are run.
 * late_init covers any special initialization that an IP requires
 * after all of the have been initialized or something that needs to happen
 * late in the init process.
 * Returns 0 on success, negative error code on failure.
 */
static int loonggpu_device_ip_late_init(struct loonggpu_device *adev)
{
	int i = 0, r;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (!adev->ip_blocks[i].status.valid)
			continue;
		if (adev->ip_blocks[i].version->funcs->late_init) {
			r = adev->ip_blocks[i].version->funcs->late_init((void *)adev);
			if (r) {
				DRM_ERROR("late_init of IP block <%s> failed %d\n",
					  adev->ip_blocks[i].version->funcs->name, r);
				return r;
			}
			adev->ip_blocks[i].status.late_initialized = true;
		}
	}

	queue_delayed_work(system_wq, &adev->late_init_work,
			   msecs_to_jiffies(LOONGGPU_RESUME_MS));

	if (adev->family_type != CHIP_NO_GPU)
		loonggpu_device_fill_reset_magic(adev);

	return 0;
}

/**
 * loonggpu_device_ip_fini - run fini for hardware IPs
 *
 * @adev: loonggpu_device pointer
 *
 * Main teardown pass for hardware IPs.  The list of all the hardware
 * IPs that make up the asic is walked and the hw_fini and sw_fini callbacks
 * are run.  hw_fini tears down the hardware associated with each IP
 * and sw_fini tears down any software state associated with each IP.
 * Returns 0 on success, negative error code on failure.
 */
static int loonggpu_device_ip_fini(struct loonggpu_device *adev)
{
	int i, r;

	loonggpu_lgkcd_device_fini_sw(adev);

	for (i = adev->num_ip_blocks - 1; i >= 0; i--) {
		if (!adev->ip_blocks[i].status.hw)
			continue;

		r = adev->ip_blocks[i].version->funcs->hw_fini((void *)adev);
		/* XXX handle errors */
		if (r) {
			DRM_DEBUG("hw_fini of IP block <%s> failed %d\n",
				  adev->ip_blocks[i].version->funcs->name, r);
		}

		adev->ip_blocks[i].status.hw = false;
	}


	for (i = adev->num_ip_blocks - 1; i >= 0; i--) {
		if (!adev->ip_blocks[i].status.sw)
			continue;

		if (adev->ip_blocks[i].version->type == LOONGGPU_IP_BLOCK_TYPE_GMC) {
			loonggpu_device_wb_fini(adev);
			loonggpu_device_vram_scratch_fini(adev);
		}

		r = adev->ip_blocks[i].version->funcs->sw_fini((void *)adev);
		/* XXX handle errors */
		if (r) {
			DRM_DEBUG("sw_fini of IP block <%s> failed %d\n",
				  adev->ip_blocks[i].version->funcs->name, r);
		}
		adev->ip_blocks[i].status.sw = false;
		adev->ip_blocks[i].status.valid = false;
	}

	for (i = adev->num_ip_blocks - 1; i >= 0; i--) {
		if (!adev->ip_blocks[i].status.late_initialized)
			continue;
		if (adev->ip_blocks[i].version->funcs->late_fini)
			adev->ip_blocks[i].version->funcs->late_fini((void *)adev);
		adev->ip_blocks[i].status.late_initialized = false;
	}

	return 0;
}

/**
 * loonggpu_device_ip_late_init_func_handler - work handler for clockgating
 *
 * @work: work_struct
 *
 * Work handler for loonggpu_device_ip_late_set_cg_state.  We put the
 * clockgating setup into a worker thread to speed up driver init and
 * resume from suspend.
 */
static void loonggpu_device_ip_late_init_func_handler(struct work_struct *work)
{
#if 1
	return;
#else
	struct loonggpu_device *adev =
		container_of(work, struct loonggpu_device, late_init_work.work);
	int r;

	r = loonggpu_ib_ring_tests(adev);
	if (r)
		DRM_ERROR("ib ring test failed (%d).\n", r);

	r = loonggpu_ring_test_xdma(&adev->xdma.instance[0].ring, msecs_to_jiffies(5000));
	if (r)
		DRM_ERROR("xdma test failed (%d).\n", r);
#endif
}

/**
 * loonggpu_device_ip_suspend_phase1 - run suspend for hardware IPs (phase 1)
 *
 * @adev: loonggpu_device pointer
 *
 * Main suspend function for hardware IPs.  The list of all the hardware
 * IPs that make up the asic is walked, clockgating is disabled and the
 * suspend callbacks are run.  suspend puts the hardware and software state
 * in each IP into a state suitable for suspend.
 * Returns 0 on success, negative error code on failure.
 */
static int loonggpu_device_ip_suspend_phase1(struct loonggpu_device *adev)
{
	int i, r;

	for (i = adev->num_ip_blocks - 1; i >= 0; i--) {
		if (!adev->ip_blocks[i].status.valid)
			continue;
		/* displays are handled separately */
		if (adev->ip_blocks[i].version->type == LOONGGPU_IP_BLOCK_TYPE_DCE) {
			/* XXX handle errors */
			r = adev->ip_blocks[i].version->funcs->suspend(adev);
			/* XXX handle errors */
			if (r) {
				DRM_ERROR("suspend of IP block <%s> failed %d\n",
					  adev->ip_blocks[i].version->funcs->name, r);
			}
		}
	}
	return 0;
}

/**
 * loonggpu_device_ip_suspend_phase2 - run suspend for hardware IPs (phase 2)
 *
 * @adev: loonggpu_device pointer
 *
 * Main suspend function for hardware IPs.  The list of all the hardware
 * IPs that make up the asic is walked, clockgating is disabled and the
 * suspend callbacks are run.  suspend puts the hardware and software state
 * in each IP into a state suitable for suspend.
 * Returns 0 on success, negative error code on failure.
 */
static int loonggpu_device_ip_suspend_phase2(struct loonggpu_device *adev)
{
	int i, r;

	/* call smu to disable gfx off feature first when suspend */

	for (i = adev->num_ip_blocks - 1; i >= 0; i--) {
		if (!adev->ip_blocks[i].status.valid)
			continue;
		/* displays are handled in phase1 */
		if (adev->ip_blocks[i].version->type == LOONGGPU_IP_BLOCK_TYPE_DCE)
			continue;
		/* XXX handle errors */
		r = adev->ip_blocks[i].version->funcs->suspend(adev);
		/* XXX handle errors */
		if (r) {
			DRM_ERROR("suspend of IP block <%s> failed %d\n",
				  adev->ip_blocks[i].version->funcs->name, r);
		}
	}

	return 0;
}

/**
 * loonggpu_device_ip_suspend - run suspend for hardware IPs
 *
 * @adev: loonggpu_device pointer
 *
 * Main suspend function for hardware IPs.  The list of all the hardware
 * IPs that make up the asic is walked, clockgating is disabled and the
 * suspend callbacks are run.  suspend puts the hardware and software state
 * in each IP into a state suitable for suspend.
 * Returns 0 on success, negative error code on failure.
 */
int loonggpu_device_ip_suspend(struct loonggpu_device *adev)
{
	int r;

	r = loonggpu_device_ip_suspend_phase1(adev);
	if (r)
		return r;
	r = loonggpu_device_ip_suspend_phase2(adev);

	return r;
}


/**
 * loonggpu_device_ip_resume_phase1 - run resume for hardware IPs
 *
 * @adev: loonggpu_device pointer
 *
 * First resume function for hardware IPs.  The list of all the hardware
 * IPs that make up the asic is walked and the resume callbacks are run for
 * COMMON, GMC, and IH.  resume puts the hardware into a functional state
 * after a suspend and updates the software state as necessary.  This
 * function is also used for restoring the GPU after a GPU reset.
 * Returns 0 on success, negative error code on failure.
 */
static int loonggpu_device_ip_resume_phase1(struct loonggpu_device *adev)
{
	int i, r;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (!adev->ip_blocks[i].status.valid)
			continue;
		if (adev->ip_blocks[i].version->type == LOONGGPU_IP_BLOCK_TYPE_COMMON ||
		    adev->ip_blocks[i].version->type == LOONGGPU_IP_BLOCK_TYPE_GMC ||
		    adev->ip_blocks[i].version->type == LOONGGPU_IP_BLOCK_TYPE_IH) {
			r = adev->ip_blocks[i].version->funcs->resume(adev);
			if (r) {
				DRM_ERROR("resume of IP block <%s> failed %d\n",
					  adev->ip_blocks[i].version->funcs->name, r);
				return r;
			}
		}
	}

	return 0;
}

/**
 * loonggpu_device_ip_resume_phase2 - run resume for hardware IPs
 *
 * @adev: loonggpu_device pointer
 *
 * First resume function for hardware IPs.  The list of all the hardware
 * IPs that make up the asic is walked and the resume callbacks are run for
 * all blocks except COMMON, GMC, and IH.  resume puts the hardware into a
 * functional state after a suspend and updates the software state as
 * necessary.  This function is also used for restoring the GPU after a GPU
 * reset.
 * Returns 0 on success, negative error code on failure.
 */
static int loonggpu_device_ip_resume_phase2(struct loonggpu_device *adev)
{
	int i, r;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (!adev->ip_blocks[i].status.valid)
			continue;
		if (adev->ip_blocks[i].version->type == LOONGGPU_IP_BLOCK_TYPE_COMMON ||
		    adev->ip_blocks[i].version->type == LOONGGPU_IP_BLOCK_TYPE_GMC ||
		    adev->ip_blocks[i].version->type == LOONGGPU_IP_BLOCK_TYPE_IH)
			continue;
		r = adev->ip_blocks[i].version->funcs->resume(adev);
		if (r) {
			DRM_ERROR("resume of IP block <%s> failed %d\n",
				  adev->ip_blocks[i].version->funcs->name, r);
			return r;
		}
	}

	return 0;
}

/**
 * loonggpu_device_ip_resume - run resume for hardware IPs
 *
 * @adev: loonggpu_device pointer
 *
 * Main resume function for hardware IPs.  The hardware IPs
 * are split into two resume functions because they are
 * are also used in in recovering from a GPU reset and some additional
 * steps need to be take between them.  In this case (S3/S4) they are
 * run sequentially.
 * Returns 0 on success, negative error code on failure.
 */
static int loonggpu_device_ip_resume(struct loonggpu_device *adev)
{
	int r;

	r = loonggpu_device_ip_resume_phase1(adev);
	if (r)
		return r;
	r = loonggpu_device_ip_resume_phase2(adev);

	return r;
}

/**
 * loonggpu_device_prepare_dpm - prepare the dpm func
 *
 * @adev: loonggpu_device pointer
 *
 * get bios gpu dpm suppport state.
 */
static int loonggpu_device_prepare_dpm(struct loonggpu_device *adev)
{
	uint32_t state;

	state = RREG32(LOONGGPU_LG2XX_DPM_STATE);
	if ((state & LG2XX_DPM_STATE_READY) == LG2XX_DPM_STATE_READY) {
		adev->pm.dpm.bios_support = true;
		DRM_INFO("The current BIOS support the GPU DPM function.\n");
		DRM_INFO("The GPU DPM function is %s.\n", loonggpu_dpm? "enabled":"disabled");
		return 0;
	}

	/* The current BIOS does not support the DPM function */
	loonggpu_dpm = 0;
	adev->pm.dpm.bios_support = false;
	DRM_INFO("The current BIOS not support the GPU DPM function.\n");

	return 0;
}

static void loonggpu_device_lg2xx_early_init(struct loonggpu_device *adev)
{
	uint32_t reg_val = 0;

	/* enable switch ejtag to ec132 */
	reg_val = loonggpu_mm_rreg(adev, 0x40, LOONGGPU_REGS_NO_KIQ);
	reg_val = reg_val | 0x10;
	loonggpu_mm_wreg(adev, 0x40, reg_val);

	/* enable lg210 ec132 uart gpio */
	reg_val = loonggpu_mm_rreg(adev, 0x10, LOONGGPU_REGS_NO_KIQ);
	reg_val = (reg_val&~0xff00) | 0x5500;
	loonggpu_mm_wreg(adev, 0x10, reg_val);
}

/**
 * loonggpu_device_init - initialize the driver
 *
 * @adev: loonggpu_device pointer
 * @ddev: drm dev pointer
 * @pdev: pci dev pointer
 * @flags: driver flags
 *
 * Initializes the driver info and hw (all asics).
 * Returns 0 for success or an error on failure.
 * Called at driver startup.
 */
int loonggpu_device_init(struct loonggpu_device *adev,
		       struct drm_device *ddev,
		       struct pci_dev *pdev,
		       uint32_t flags)
{
	int r;
	bool runtime = false;
	u32 max_MBps;
	u32 hw_inf;

	adev->shutdown = false;
	adev->dev = &pdev->dev;
	adev->ddev = ddev;
	adev->pdev = pdev;
	adev->loongson_dc = to_pci_dev(ddev->dev);
	adev->flags = flags;
	adev->family_type = flags & GAGPU_ASIC_MASK;
	adev->usec_timeout = LOONGGPU_MAX_USEC_TIMEOUT;
	adev->gmc.gart_size = 512 * 1024 * 1024;
	adev->accel_working = false;
	adev->num_rings = 0;
	adev->mman.buffer_funcs = NULL;
	adev->mman.buffer_funcs_ring = NULL;
	adev->vm_manager.vm_pte_funcs = NULL;
	adev->vm_manager.vm_pte_num_rings = 0;
	adev->gmc.gmc_funcs = NULL;
	adev->fence_context = dma_fence_context_alloc(LOONGGPU_MAX_RINGS);

	/* mutex initialization are all done here so we
	 * can recall function without having locking issues */
	atomic_set(&adev->irq.ih.lock, 0);
	mutex_init(&adev->firmware.mutex);
	mutex_init(&adev->gfx.gpu_clock_mutex);
	mutex_init(&adev->srbm_mutex);
	mutex_init(&adev->grbm_idx_mutex);
	mutex_init(&adev->mn_lock);
	hash_init(adev->mn_hash);
	mutex_init(&adev->lock_reset);
	spin_lock_init(&adev->dc_mmio_lock);

	loonggpu_device_check_arguments(adev);

	spin_lock_init(&adev->mmio_idx_lock);
	spin_lock_init(&adev->pcie_idx_lock);
	spin_lock_init(&adev->se_cac_idx_lock);
	spin_lock_init(&adev->mm_stats.lock);

	INIT_LIST_HEAD(&adev->shadow_list);
	mutex_init(&adev->shadow_list_lock);

	INIT_LIST_HEAD(&adev->ring_lru_list);
	spin_lock_init(&adev->ring_lru_list_lock);

	INIT_DELAYED_WORK(&adev->late_init_work,
			  loonggpu_device_ip_late_init_func_handler);

	/* pci get dc revision */
	if (adev->family_type != CHIP_LG210) {
		adev->wc_support = true;
		adev->dc_revision = adev->loongson_dc->revision;
		DRM_DEBUG_DRIVER("LOONGGPU dc revision id 0x%x\n", adev->dc_revision);
		if (adev->loongson_dc->device == 0x7a36) {
			if (adev->dc_revision == 0x10) {
				adev->chip = dev_2k2000;
				DRM_INFO(
					"Set 2K2000 device in loonggpu driver\n");
			} else if (adev->dc_revision < 0x10) {
				adev->chip = dev_7a2000;
				DRM_INFO(
					"Set 7A2000-%s device in loonggpu driver\n",
					(adev->dc_revision == 0x1) ? "aa" :
					(adev->dc_revision == 0x2) ? "ba/bb" :
					(adev->dc_revision == 0x3) ? "cb" :
								     "unknown");
			}
		} else if (adev->loongson_dc->device == 0x7a46) {
			adev->chip = dev_2k3000;
			DRM_INFO("Set 2K3000-%s device in loonggpu driver\n",
				 (adev->dc_revision == 0x10) ? "aa" :
				 (adev->dc_revision == 0x1) ? "ca" : "unknown");
		} else if (adev->loongson_dc->device == 0x7a06) {
			adev->chip = dev_7a1000;
			DRM_INFO("Set 7A1000 device in loonggpu driver\n");
		}
	} else {
		adev->chip = dev_9a1000;
		adev->wc_support = false;
		DRM_INFO("Set 9A1000 device in loonggpu driver\n");
	}

	if (loonggpu_write_combine != -1)
		adev->wc_support = loonggpu_write_combine;

	DRM_INFO("write combine %s\n",
		adev->wc_support ? "enabled" : "disabled");

	if (adev->family_type != CHIP_LG210) {
		adev->rmmio_base = pci_resource_start(adev->pdev, 0);
		adev->rmmio_size = pci_resource_len(adev->pdev, 0);
	} else {
		adev->rmmio_base = pci_resource_start(adev->pdev, 2);
		adev->rmmio_size = pci_resource_len(adev->pdev, 2);
	}

	adev->rmmio = ioremap(adev->rmmio_base, adev->rmmio_size);

	if (adev->family_type == CHIP_LG210) {
		loonggpu_device_lg2xx_early_init(adev);
		adev->rmmio_sc = adev->rmmio;
		adev->rmmio += LG2XX_EC132_OFFSET;
	}

	if (adev->rmmio == NULL) {
		return -ENOMEM;
	}

	if (adev->family_type == CHIP_LG200) {
		hw_inf = RREG32(LOONGGPU_HWINF) & 0xfff;
		if (hw_inf == 0x211)
			adev->family_type = CHIP_LG211;
	}

	DRM_INFO(
		"initializing kernel modesetting (%s 0x%04X:0x%04X 0x%04X:0x%04X 0x%02X).\n",
		loonggpu_family_name[adev->family_type], pdev->vendor,
		 pdev->device, pdev->subsystem_vendor, pdev->subsystem_device,
		 pdev->revision);
	DRM_INFO("register mmio base: 0x%08X\n", (uint32_t)adev->rmmio_base);
	DRM_INFO("register mmio size: %u\n", (unsigned)adev->rmmio_size);

	if (adev->family_type != CHIP_LG210) {
		/* loongson dc */
		adev->loongson_dc_rmmio_base = pci_resource_start(adev->loongson_dc, 0);
		adev->loongson_dc_rmmio_size = pci_resource_len(adev->loongson_dc, 0);
		adev->loongson_dc_rmmio = pci_iomap(adev->loongson_dc, 0, adev->loongson_dc_rmmio_size);
		if (adev->loongson_dc_rmmio == NULL) {
			return -ENOMEM;
		}
		DRM_INFO("loonggpu dc register mmio base: 0x%08X\n", (uint32_t)adev->loongson_dc_rmmio_base);
		DRM_INFO("loonggpu dc register mmio size: %u\n", (unsigned)adev->loongson_dc_rmmio_size);
		adev->io_base = ioremap(LS7A_CHIPCFG_REG_BASE, 0xf);
		if (adev->io_base == NULL)
			return -ENOMEM;
		DRM_INFO("loonggpu dc io base: 0x%lx\n", (unsigned long)adev->io_base);
	} else { /* 9a1000 dc */
		adev->loongson_dc_rmmio_base = adev->rmmio_base + 0x300000; //9a1000 dc offset
		adev->loongson_dc_rmmio_size = adev->rmmio_size - 0x300000;

		adev->loongson_dc_rmmio = pci_iomap(adev->pdev, 2, adev->rmmio_size);
		if (adev->loongson_dc_rmmio == NULL) {
			return -ENOMEM;
		}

		adev->loongson_dc_rmmio += 0x300000;
		DRM_INFO("loonggpu dc register mmio base: %#llX\n", (uint64_t)adev->loongson_dc_rmmio_base);
		DRM_INFO("loonggpu dc rmmio: %#llX\n", (uint64_t)adev->loongson_dc_rmmio);
		DRM_INFO("loonggpu dc register mmio size: %u\n", (unsigned)adev->loongson_dc_rmmio_size);
 	}
 
	if (adev->family_type == CHIP_LG200)
		loonggpu_device_prepare_dpm(adev);

	/* early init functions */
	r = loonggpu_device_ip_early_init(adev);
	if (r)
		return r;

	if (adev->family_type != CHIP_LG210) {
		/* if we have > 1 VGA cards, then disable the loonggpu VGA resources */
		/* this will fail for cards that aren't VGA class devices, just
		 * ignore it */
		vga_client_register(adev->loongson_dc, loonggpu_device_vga_set_decode, adev);
	}
	vga_switcheroo_register_client(adev->loongson_dc,
				&loonggpu_switcheroo_ops, true);

	/* Fence driver */
	r = loonggpu_fence_driver_init(adev);
	if (r) {
		dev_err(adev->dev, "loonggpu_fence_driver_init failed\n");
		goto failed;
	}

	/* init the mode config */
	drm_mode_config_init(adev->ddev);

	if (adev->family_type != CHIP_NO_GPU) {
		r = loonggpu_cp_init(adev);
		if (r) {
			/* failed in exclusive mode due to timeout */
			dev_err(adev->dev, "loonggpu_cp_init failed\n");
			goto failed;
		}
	}

	r = loonggpu_device_ip_init(adev);
	if (r) {
		/* failed in exclusive mode due to timeout */
		dev_err(adev->dev, "loonggpu_device_ip_init failed\n");
		goto failed;
	}

	adev->accel_working = true;

	/* Initialize the buffer migration limit. */
	if (loonggpu_moverate >= 0)
		max_MBps = loonggpu_moverate;
	else
		max_MBps = 8; /* Allow 8 MB/s. */
	/* Get a log2 for easy divisions. */
	adev->mm_stats.log2_max_MBps = ilog2(max(1u, max_MBps));
	if (adev->family_type != CHIP_NO_GPU) {
		r = loonggpu_ib_pool_init(adev);
		if (r) {
			dev_err(adev->dev, "IB initialization failed (%d).\n", r);
			goto failed;
		}
	}
	loonggpu_fbdev_init(adev);

	loonggpu_dc_scale_init(adev);

	r = loonggpu_pm_sysfs_init(adev);
	if (r)
		DRM_ERROR("registering pm debugfs failed (%d).\n", r);

	r = loonggpu_gpu_info_sysfs_init(adev);
	if (r)
		DRM_ERROR("registering gpu_info sysfs failed (%d).\n", r);

	r = loonggpu_debugfs_gem_init(adev);
	if (r)
		DRM_ERROR("registering gem debugfs failed (%d).\n", r);

	r = loonggpu_debugfs_sema_init(adev);
	if (r)
		DRM_ERROR("registering sema debugfs failed (%d).\n", r);

	r = loonggpu_debugfs_regs_init(adev);
	if (r)
		DRM_ERROR("registering register debugfs failed (%d).\n", r);

	r = loonggpu_debugfs_firmware_init(adev);
	if (r)
		DRM_ERROR("registering firmware debugfs failed (%d).\n", r);

	r = loonggpu_debugfs_init(adev);
	if (r)
		DRM_ERROR("Creating debugfs files failed (%d).\n", r);

	if ((loonggpu_testing & 1)) {
		if (adev->accel_working)
			loonggpu_test_moves(adev);
		else
			DRM_INFO("loonggpu: acceleration disabled, skipping move tests\n");
	}
	if (loonggpu_benchmarking) {
		if (adev->accel_working)
			loonggpu_benchmark(adev, loonggpu_benchmarking);
		else
			DRM_INFO("loonggpu: acceleration disabled, skipping benchmarks\n");
	}

	/* enable clockgating, etc. after ib tests, etc. since some blocks require
	 * explicit gating rather than handling it automatically.
	 */
	r = loonggpu_device_ip_late_init(adev);
	if (r) {
		dev_err(adev->dev, "loonggpu_device_ip_late_init failed\n");
		goto failed;
	}

	if (adev->family_type != CHIP_NO_GPU)
		xdma_ring_test_xdma_loop(&adev->xdma.instance[0].ring, msecs_to_jiffies(5000));

	return 0;

failed:
	if (runtime)
		vga_switcheroo_fini_domain_pm_ops(adev->dev);

	return r;
}

/**
 * loonggpu_device_fini - tear down the driver
 *
 * @adev: loonggpu_device pointer
 *
 * Tear down the driver info (all asics).
 * Called at driver shutdown.
 */
void loonggpu_device_fini(struct loonggpu_device *adev)
{
	int r;

	adev->shutdown = true;
	/* disable all interrupts */
	loonggpu_irq_disable_all(adev);
	if (adev->mode_info.mode_config_initialized)
		drm_atomic_helper_shutdown(adev->ddev);

	loonggpu_ib_pool_fini(adev);
	loonggpu_fence_driver_fini(adev);
	loonggpu_dc_scale_fini(adev);
	loonggpu_fbdev_fini(adev);
	r = loonggpu_device_ip_fini(adev);
	if (adev->firmware.gpu_info_fw) {
		release_firmware(adev->firmware.gpu_info_fw);
		adev->firmware.gpu_info_fw = NULL;
	}
	adev->accel_working = false;
	cancel_delayed_work_sync(&adev->late_init_work);

	loonggpu_cp_fini(adev);

	kfree(adev->bios);
	adev->bios = NULL;

	vga_switcheroo_unregister_client(adev->loongson_dc);
	vga_switcheroo_unregister_handler();

	if (adev->flags & LOONGGPU_IS_PX)
		vga_switcheroo_fini_domain_pm_ops(adev->dev);
	if (adev->family_type != CHIP_LG210)
		vga_client_register(adev->loongson_dc, NULL, adev);

	iounmap(adev->rmmio);
	adev->rmmio = NULL;
	adev->rmmio_sc = NULL;

	loonggpu_gpu_info_sysfs_exit(adev);
	loonggpu_pm_sysfs_exit(adev);
	loonggpu_debugfs_regs_cleanup(adev);
}

static int loonggpu_zip_gem_bo_validate(int id, void *ptr, void *data)
{
	struct drm_gem_object *gobj = ptr;
	struct loonggpu_bo *bo = gem_to_loonggpu_bo(gobj);
	int r, i;
	struct ttm_operation_ctx ctx = { false, false };
	unsigned int domain;

	domain = loonggpu_mem_type_to_domain(lg_get_bo_mem_type(bo));

	if (bo->flags & LOONGGPU_GEM_CREATE_COMPRESSED_MASK) {

		loonggpu_bo_reserve(bo, false);

		domain = bo->preferred_domains;

		/* force to pin into visible video ram */
		if (!(bo->flags & LOONGGPU_GEM_CREATE_NO_CPU_ACCESS))
			bo->flags |= LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
		loonggpu_bo_placement_from_domain(bo, domain);
		for (i = 0; i < bo->placement.num_placement; i++) {
			unsigned int fpfn, lpfn;

			fpfn = bo->node_offset;
			lpfn = bo->node_offset + lg_tbo_to_num_pages(&bo->tbo);

			if (fpfn > bo->placements[i].fpfn)
				bo->placements[i].fpfn = fpfn;
			if (!bo->placements[i].lpfn ||
				(lpfn && lpfn < bo->placements[i].lpfn))
				bo->placements[i].lpfn = lpfn;
		}

		r = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
		if (unlikely(r)) {
			DRM_ERROR("loonggpu zip bo validate failed %d\n", r);
		}

		loonggpu_bo_unreserve(bo);

	}

	return 0;
}

static int loonggpu_zip_gem_bo_evict(int id, void *ptr, void *data)
{
	struct drm_gem_object *gobj = ptr;
	struct loonggpu_bo *bo = gem_to_loonggpu_bo(gobj);
	int r, i;
	struct ttm_operation_ctx ctx = { false, false };
	struct loonggpu_res_cursor __maybe_unused cursor;
	unsigned domain;

	domain = loonggpu_mem_type_to_domain(lg_get_bo_mem_type(bo));

	if (bo->flags & LOONGGPU_GEM_CREATE_COMPRESSED_MASK && domain == LOONGGPU_GEM_DOMAIN_VRAM) {

		loonggpu_bo_reserve(bo, false);

		loonggpu_res_first(bo->tbo.resource, 0, gobj->size, &cursor);
		bo->node_offset = cursor.start >> PAGE_SHIFT;

		for (i = 0; i < bo->placement.num_placement; i++) {
			bo->placements[i].lpfn = 0;
			bo->flags &= ~LOONGGPU_GEM_CREATE_PREEMPTIBLE;
		}

		r = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
		if (unlikely(r))
			DRM_ERROR("loonggpu zip bo evict failed %d\n", r);

		loonggpu_bo_unreserve(bo);
	}

	return 0;
}

/*
 * Suspend & resume.
 */
/**
 * loonggpu_device_suspend - initiate device suspend
 *
 * @dev: drm dev pointer
 * @suspend: suspend state
 * @fbcon : notify the fbdev of suspend
 *
 * Puts the hw in the suspend state (all asics).
 * Returns 0 for success or an error on failure.
 * Called at driver suspend.
 */
int loonggpu_device_suspend(struct drm_device *dev, bool suspend, bool fbcon)
{
	struct loonggpu_device *adev;
	int r;
	struct drm_file *file;

	if (dev == NULL || dev->dev_private == NULL) {
		return -ENODEV;
	}

	adev = dev->dev_private;

	if (dev->switch_power_state == DRM_SWITCH_POWER_OFF)
		return 0;

	drm_kms_helper_poll_disable(dev);

	if (fbcon)
		loonggpu_fbdev_set_suspend(adev, 1);

	loonggpu_lgkcd_suspend(adev, false);

	r = loonggpu_device_ip_suspend_phase1(adev);

	r = mutex_lock_interruptible(&dev->filelist_mutex);
	if (r)
		return r;

	list_for_each_entry(file, &dev->filelist, lhead) {
		spin_lock(&file->table_lock);
		idr_for_each(&file->object_idr, loonggpu_zip_gem_bo_evict, NULL);
		spin_unlock(&file->table_lock);
	}

	mutex_unlock(&dev->filelist_mutex);

	/* evict vram memory */
	loonggpu_bo_evict_vram(adev);

	loonggpu_fence_driver_suspend(adev);

	r = loonggpu_device_ip_suspend_phase2(adev);

	/* evict remaining vram memory
	 * This second call to evict vram is to evict the gart page table
	 * using the CPU.
	 */
	loonggpu_bo_evict_vram(adev);

	pci_save_state(adev->pdev);
	if (suspend) {
		/* Shut down the device */
		pci_disable_device(adev->pdev);
		pci_set_power_state(adev->pdev, PCI_D3hot);
	} else {
		r = loonggpu_asic_reset(adev);
		if (r)
			DRM_ERROR("loonggpu asic reset failed\n");
	}

	return 0;
}

/**
 * loonggpu_device_resume - initiate device resume
 *
 * @dev: drm dev pointer
 * @resume: resume state
 * @fbcon : notify the fbdev of resume
 *
 * Bring the hw back to operating state (all asics).
 * Returns 0 for success or an error on failure.
 * Called at driver resume.
 */
int loonggpu_device_resume(struct drm_device *dev, bool resume, bool fbcon)
{
	struct loonggpu_device *adev = dev->dev_private;
	struct loonggpu_dc *dc = adev->dc;
	int r = 0;
	int i;
	struct drm_file *file;
	struct loonggpu_backlight *ls_bl;
	struct loonggpu_crtc *acrtc;
	struct loonggpu_dc_crtc *dc_crtc;

	if (dev->switch_power_state == DRM_SWITCH_POWER_OFF)
		return 0;

	for (i = 0; i < 2; i++) {
		dc_crtc = adev->dc->link_info[i].crtc;
		if (adev->i2c[i]) {
			dc_interface_i2c_set(dc_crtc, adev->i2c[i]->gpio_i2c);
			if (dc->hw_ops->dc_i2c_resume)
				dc->hw_ops->dc_i2c_resume(adev, i);
		}
		ls_bl = adev->mode_info.backlights[i];
		if (ls_bl && ls_bl->power)
			ls_bl->power(ls_bl, false);
		acrtc = adev->mode_info.crtcs[i];
		dc->hw_ops->crtc_enable(dc_crtc, false);
	}

	if (resume) {
		pci_set_power_state(adev->pdev, PCI_D0);
		pci_restore_state(adev->pdev);
		r = pci_enable_device(adev->pdev);
		if (r)
			return r;
	}
	if (adev->family_type != CHIP_NO_GPU) {
		r = loonggpu_cp_gfx_load_microcode(adev);
		if (r) {
			DRM_ERROR(" loonggpu_cp_gfx_load_microcode fail\n");
			return r;
		}

		r = loonggpu_cp_enable(adev, true);
		if (r) {
			DRM_ERROR(" loonggpu_cp_enable fail\n");
			return r;
		}
	}

	r = mutex_lock_interruptible(&dev->filelist_mutex);
	if (r)
		return r;

	list_for_each_entry(file, &dev->filelist, lhead) {
		spin_lock(&file->table_lock);
		idr_for_each(&file->object_idr, loonggpu_zip_gem_bo_validate, NULL);
		spin_unlock(&file->table_lock);
	}

	mutex_unlock(&dev->filelist_mutex);

	r = loonggpu_device_ip_resume(adev);
	if (r) {
		DRM_ERROR("loonggpu_device_ip_resume failed (%d).\n", r);
		return r;
	}
	loonggpu_fence_driver_resume(adev);


	r = loonggpu_device_ip_late_init(adev);
	if (r)
		return r;

	r = loonggpu_lgkcd_resume(adev, false);
	if (r)
		return r;

	/* Make sure IB tests flushed */
	flush_delayed_work(&adev->late_init_work);

	/* blat the mode back in */
	if (fbcon)
		loonggpu_fbdev_set_suspend(adev, 0);

	drm_kms_helper_poll_enable(dev);

	for (i = 0; i < 2; i++) {
		dc_crtc = adev->dc->link_info[i].crtc;
		ls_bl = adev->mode_info.backlights[i];
		if (ls_bl && ls_bl->power)
			ls_bl->power(ls_bl, true);
		acrtc = adev->mode_info.crtcs[i];
		dc->hw_ops->crtc_enable(dc_crtc, true);
	}

	/*
	 * Most of the connector probing functions try to acquire runtime pm
	 * refs to ensure that the GPU is powered on when connector polling is
	 * performed. Since we're calling this from a runtime PM callback,
	 * trying to acquire rpm refs will cause us to deadlock.
	 *
	 * Since we're guaranteed to be holding the rpm lock, it's safe to
	 * temporarily disable the rpm helpers so this doesn't deadlock us.
	 */
#ifdef CONFIG_PM
	dev->dev->power.disable_depth++;
#endif
	drm_kms_helper_hotplug_event(dev);
#ifdef CONFIG_PM
	dev->dev->power.disable_depth--;
#endif
	return 0;
}

/**
 * loonggpu_device_ip_check_soft_reset - did soft reset succeed
 *
 * @adev: loonggpu_device pointer
 *
 * The list of all the hardware IPs that make up the asic is walked and
 * the check_soft_reset callbacks are run.  check_soft_reset determines
 * if the asic is still hung or not.
 * Returns true if any of the IPs are still in a hung state, false if not.
 */
static bool loonggpu_device_ip_check_soft_reset(struct loonggpu_device *adev)
{
	int i;
	bool asic_hang = false;

	if (loonggpu_asic_need_full_reset(adev))
		return true;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (!adev->ip_blocks[i].status.valid)
			continue;
		if (adev->ip_blocks[i].version->funcs->check_soft_reset)
			adev->ip_blocks[i].status.hang =
				adev->ip_blocks[i].version->funcs->check_soft_reset(adev);
		if (adev->ip_blocks[i].status.hang) {
			DRM_INFO("IP block:%s is hung!\n", adev->ip_blocks[i].version->funcs->name);
			asic_hang = true;
		}
	}
	return asic_hang;
}

/**
 * loonggpu_device_ip_pre_soft_reset - prepare for soft reset
 *
 * @adev: loonggpu_device pointer
 *
 * The list of all the hardware IPs that make up the asic is walked and the
 * pre_soft_reset callbacks are run if the block is hung.  pre_soft_reset
 * handles any IP specific hardware or software state changes that are
 * necessary for a soft reset to succeed.
 * Returns 0 on success, negative error code on failure.
 */
static int loonggpu_device_ip_pre_soft_reset(struct loonggpu_device *adev)
{
	int i, r = 0;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (!adev->ip_blocks[i].status.valid)
			continue;
		if (adev->ip_blocks[i].status.hang &&
		    adev->ip_blocks[i].version->funcs->pre_soft_reset) {
			r = adev->ip_blocks[i].version->funcs->pre_soft_reset(adev);
			if (r)
				return r;
		}
	}

	return 0;
}

/**
 * loonggpu_device_ip_need_full_reset - check if a full asic reset is needed
 *
 * @adev: loonggpu_device pointer
 *
 * Some hardware IPs cannot be soft reset.  If they are hung, a full gpu
 * reset is necessary to recover.
 * Returns true if a full asic reset is required, false if not.
 */
static bool loonggpu_device_ip_need_full_reset(struct loonggpu_device *adev)
{
	int i;

	if (loonggpu_asic_need_full_reset(adev))
		return true;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (!adev->ip_blocks[i].status.valid)
			continue;
		if ((adev->ip_blocks[i].version->type == LOONGGPU_IP_BLOCK_TYPE_GMC) ||
		    (adev->ip_blocks[i].version->type == LOONGGPU_IP_BLOCK_TYPE_DCE)) {
			if (adev->ip_blocks[i].status.hang) {
				DRM_INFO("Some block need full reset!\n");
				return true;
			}
		}
	}
	return false;
}

/**
 * loonggpu_device_ip_soft_reset - do a soft reset
 *
 * @adev: loonggpu_device pointer
 *
 * The list of all the hardware IPs that make up the asic is walked and the
 * soft_reset callbacks are run if the block is hung.  soft_reset handles any
 * IP specific hardware or software state changes that are necessary to soft
 * reset the IP.
 * Returns 0 on success, negative error code on failure.
 */
static int loonggpu_device_ip_soft_reset(struct loonggpu_device *adev)
{
	int i, r = 0;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (!adev->ip_blocks[i].status.valid)
			continue;
		if (adev->ip_blocks[i].status.hang &&
		    adev->ip_blocks[i].version->funcs->soft_reset) {
			r = adev->ip_blocks[i].version->funcs->soft_reset(adev);
			if (r)
				return r;
		}
	}

	return 0;
}

/**
 * loonggpu_device_ip_post_soft_reset - clean up from soft reset
 *
 * @adev: loonggpu_device pointer
 *
 * The list of all the hardware IPs that make up the asic is walked and the
 * post_soft_reset callbacks are run if the asic was hung.  post_soft_reset
 * handles any IP specific hardware or software state changes that are
 * necessary after the IP has been soft reset.
 * Returns 0 on success, negative error code on failure.
 */
static int loonggpu_device_ip_post_soft_reset(struct loonggpu_device *adev)
{
	int i, r = 0;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (!adev->ip_blocks[i].status.valid)
			continue;
		if (adev->ip_blocks[i].status.hang &&
		    adev->ip_blocks[i].version->funcs->post_soft_reset)
			r = adev->ip_blocks[i].version->funcs->post_soft_reset(adev);
		if (r)
			return r;
	}

	return 0;
}

/**
 * loonggpu_device_recover_vram_from_shadow - restore shadowed VRAM buffers
 *
 * @adev: loonggpu_device pointer
 * @ring: loonggpu_ring for the engine handling the buffer operations
 * @bo: loonggpu_bo buffer whose shadow is being restored
 * @fence: dma_fence associated with the operation
 *
 * Restores the VRAM buffer contents from the shadow in GTT.  Used to
 * restore things like GPUVM page tables after a GPU reset where
 * the contents of VRAM might be lost.
 * Returns 0 on success, negative error code on failure.
 */
static int loonggpu_device_recover_vram_from_shadow(struct loonggpu_device *adev,
						  struct loonggpu_ring *ring,
						  struct loonggpu_bo *bo,
						  struct dma_fence **fence)
{
	uint32_t domain;
	int r;

	if (!bo->shadow)
		return 0;

	r = loonggpu_bo_reserve(bo, true);
	if (r)
		return r;
	domain = loonggpu_mem_type_to_domain(lg_get_bo_mem_type(bo));
	/* if bo has been evicted, then no need to recover */
	if (domain == LOONGGPU_GEM_DOMAIN_VRAM) {
		r = loonggpu_bo_validate(bo->shadow);
		if (r) {
			DRM_ERROR("bo validate failed!\n");
			goto err;
		}

		r = loonggpu_bo_restore_from_shadow(adev, ring, bo,
						 NULL, fence, true);
		if (r) {
			DRM_ERROR("recover page table failed!\n");
			goto err;
		}
	}
err:
	loonggpu_bo_unreserve(bo);
	return r;
}

/**
 * loonggpu_device_handle_vram_lost - Handle the loss of VRAM contents
 *
 * @adev: loonggpu_device pointer
 *
 * Restores the contents of VRAM buffers from the shadows in GTT.  Used to
 * restore things like GPUVM page tables after a GPU reset where
 * the contents of VRAM might be lost.
 * Returns 0 on success, 1 on failure.
 */
static int loonggpu_device_handle_vram_lost(struct loonggpu_device *adev)
{
	struct loonggpu_ring *ring = adev->mman.buffer_funcs_ring;
	struct loonggpu_bo *bo, *tmp;
	struct dma_fence *fence = NULL, *next = NULL;
	long r = 1;
	int i = 0;
	long tmo;

	tmo = msecs_to_jiffies(100);

	DRM_INFO("recover vram bo from shadow start\n");
	mutex_lock(&adev->shadow_list_lock);
	list_for_each_entry_safe(bo, tmp, &adev->shadow_list, shadow_list) {
		next = NULL;
		loonggpu_device_recover_vram_from_shadow(adev, ring, bo, &next);
		if (fence) {
			r = dma_fence_wait_timeout(fence, false, tmo);
			if (r == 0)
				pr_err("wait fence %p[%d] timeout\n", fence, i);
			else if (r < 0)
				pr_err("wait fence %p[%d] interrupted\n", fence, i);
			if (r < 1) {
				dma_fence_put(fence);
				fence = next;
				break;
			}
			i++;
		}

		dma_fence_put(fence);
		fence = next;
	}
	mutex_unlock(&adev->shadow_list_lock);

	if (fence) {
		r = dma_fence_wait_timeout(fence, false, tmo);
		if (r == 0)
			pr_err("wait fence %p[%d] timeout\n", fence, i);
		else if (r < 0)
			pr_err("wait fence %p[%d] interrupted\n", fence, i);

	}
	dma_fence_put(fence);

	if (r > 0)
		DRM_INFO("recover vram bo from shadow done\n");
	else
		DRM_ERROR("recover vram bo from shadow failed\n");

	return (r > 0) ? 0 : 1;
}

/**
 * loonggpu_device_reset - reset ASIC/GPU for bare-metal or passthrough
 *
 * @adev: loonggpu device pointer
 *
 * attempt to do soft-reset or full-reset and reinitialize Asic
 * return 0 means succeeded otherwise failed
 */
static int loonggpu_device_reset(struct loonggpu_device *adev)
{
	bool need_full_reset, vram_lost = 0;
	int r;

	need_full_reset = loonggpu_device_ip_need_full_reset(adev);

	if (!need_full_reset) {
		loonggpu_device_ip_pre_soft_reset(adev);
		r = loonggpu_device_ip_soft_reset(adev);
		loonggpu_device_ip_post_soft_reset(adev);
		if (r || loonggpu_device_ip_check_soft_reset(adev)) {
			DRM_INFO("soft reset failed, will fallback to full reset!\n");
			need_full_reset = true;
		}
	}

	if (need_full_reset) {
		r = loonggpu_device_ip_suspend(adev);

retry:
		r = loonggpu_asic_reset(adev);

		if (!r) {
			dev_info(adev->dev, "GPU reset succeeded, trying to resume\n");
			r = loonggpu_device_ip_resume_phase1(adev);
			if (r)
				goto out;

			vram_lost = loonggpu_device_check_vram_lost(adev);
			if (vram_lost) {
				DRM_ERROR("VRAM is lost!\n");
				atomic_inc(&adev->vram_lost_counter);
			}

			r = loonggpu_gtt_mgr_recover(lg_bdev_to_ttm_man(&adev->mman.bdev, TTM_PL_TT));
			if (r)
				goto out;

			r = loonggpu_device_ip_resume_phase2(adev);
			if (r)
				goto out;

			if (vram_lost)
				loonggpu_device_fill_reset_magic(adev);
		}
	}

out:
	if (!r) {
		r = loonggpu_ib_ring_tests(adev);
		if (r) {
			dev_err(adev->dev, "ib ring test failed (%d).\n", r);
			r = loonggpu_device_ip_suspend(adev);
			need_full_reset = true;
			goto retry;
		}
	}

	if (!r && ((need_full_reset && !(adev->flags & LOONGGPU_IS_APU)) || vram_lost))
		r = loonggpu_device_handle_vram_lost(adev);

	return r;
}

/**
 * loonggpu_device_gpu_recover - reset the asic and recover scheduler
 *
 * @adev: loonggpu device pointer
 * @job: which job trigger hang
 * @force: forces reset regardless of loonggpu_gpu_recovery
 *
 * Attempt to reset the GPU if it has hung (all asics).
 * Returns 0 for success or an error on failure.
 */
int loonggpu_device_gpu_recover(struct loonggpu_device *adev,
			      struct loonggpu_job *job, bool force)
{
	int i, r, resched;

	if (!force && !loonggpu_device_ip_check_soft_reset(adev)) {
		DRM_INFO("No hardware hang detected. Did some blocks stall?\n");
		return 0;
	}

	if (!force && (loonggpu_gpu_recovery == 0 || loonggpu_gpu_recovery == -1)) {
		DRM_INFO("GPU recovery disabled.\n");
		return 0;
	}

	dev_info(adev->dev, "GPU reset begin!\n");

	mutex_lock(&adev->lock_reset);
	atomic_inc(&adev->gpu_reset_counter);
	adev->in_gpu_reset = 1;

	/* Block kcd */
	loonggpu_lgkcd_pre_reset(adev);

	/* block TTM */
	resched = lg_ttm_bo_lock_delayed_workqueue(adev);

	/* block all schedulers and reset given job's ring */
	for (i = 0; i < LOONGGPU_MAX_RINGS; ++i) {
		struct loonggpu_ring *ring = adev->rings[i];

		if (!lg_ring_sched_thread_avai(ring))
			continue;

		lg_ring_sched_thread_park(ring);

		if (job && job->base.sched == &ring->sched)
			continue;
		if (adev->family_type == CHIP_LG200 &&
		   (job && strcmp(job->base.sched->name, "gfx") == 0)) {

		} else {
			lg_drm_sched_stop(&ring->sched, job ? &job->base : NULL);

			/* after all hw jobs are reset, hw fence is meaningless, so force_completion */
			loonggpu_fence_driver_force_completion(ring);
		}
	}

	if (adev->family_type == CHIP_LG200 && job) {
		if(strcmp(job->base.sched->name, "gfx") == 0)
			r = loonggpu_asic_reset(adev);
	} else {
		r = loonggpu_device_reset(adev);
	}

	for (i = 0; i < LOONGGPU_MAX_RINGS; ++i) {
		struct loonggpu_ring *ring = adev->rings[i];

		if (!lg_ring_sched_thread_avai(ring))
			continue;

		/* only need recovery sched of the given job's ring
		 * or all rings (in the case @job is NULL)
		 * after above loonggpu_reset accomplished
		 */
#if 0
		/* XXX
		 * LOONGGPU: Oops!! We Can't Find A nice method to Rework This Hanged Job
		 * So We Ignore this*/
		if ((!job || job->base.sched == &ring->sched) && !r)
			drm_sched_job_recovery(&ring->sched);
#endif
		lg_ring_sched_thread_unpark(ring);
	}

	lg_ttm_bo_unlock_delayed_workqueue(adev, resched);

	if (r) {
		/* bad news, how to tell it to userspace ? */
		dev_info(adev->dev, "GPU reset(%d) failed\n", atomic_read(&adev->gpu_reset_counter));
	} else {
		dev_info(adev->dev, "GPU reset(%d) succeeded!\n", atomic_read(&adev->gpu_reset_counter));
	}

	/*unlock kcd */
	loonggpu_lgkcd_post_reset(adev);
	adev->in_gpu_reset = 0;

	if (!r)
		adev->accel_working = true;

	mutex_unlock(&adev->lock_reset);
	return r;
}

/**
 * loonggpu_device_is_peer_accessible - Check peer access through PCIe BAR
 *
 * @adev: loonggpu_device pointer
 * @peer_adev: loonggpu_device pointer for peer device trying to access @adev
 *
 * Return true if @peer_adev can access (DMA) @adev through the PCIe
 * BAR, i.e. @adev is "large BAR" and the BAR matches the DMA mask of
 * @peer_adev.
 */
bool loonggpu_device_is_peer_accessible(struct loonggpu_device *adev,
					struct loonggpu_device *peer_adev)
{
#ifdef CONFIG_VDD_LOONGSON_P2P
	uint64_t address_mask = peer_adev->dev->dma_mask ?
		~*peer_adev->dev->dma_mask : ~((1ULL << 32) - 1);
	resource_size_t aper_limit =
		adev->gmc.aper_base + adev->gmc.aper_size - 1;
	bool p2p_access =
		!(pci_p2pdma_distance(adev->pdev, peer_adev->dev, false) < 0);

	return loonggpu_pcie_p2p && p2p_access &&
		(adev->gmc.visible_vram_size &&
		 adev->gmc.real_vram_size == adev->gmc.visible_vram_size &&
		 !(adev->gmc.aper_base & address_mask ||
		   aper_limit & address_mask));
#else
	return false;
#endif
}
