#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include "loonggpu.h"
#include "loonggpu_ih.h"
#include "loonggpu_common.h"
#include "loonggpu_mmu.h"
#include "loonggpu_zip.h"
#include "loonggpu_gfx.h"
#include "loonggpu_pipe.h"
#include "loonggpu_xdma.h"
#include "loonggpu_dc_reg.h"
#include "loonggpu_doorbell.h"
#include "loonggpu_dvfs.h"
#include <asm/loongson.h>

static u32 loonggpu_get_clk(struct loonggpu_device *adev)
{
	DRM_DEBUG_DRIVER("%s Not implemented\n", __func__);

	return 0;
}

static void loonggpu_vga_set_state(struct loonggpu_device *adev, bool state)
{
	return;
/* TODO: In this function we should be enable\disable GPU&DC */
/*	u32 conf_reg;
	u32 i;

	for (i = 0; i < adev->mode_info.num_crtc; i++) {
		conf_reg = dc_readl(adev, CURRENT_REG(dc_reg_data[DI_REG_INCREASE], dc_reg_data[DI_CRTC_CFG_REG], i));
		if (state)
			conf_reg |= CRTC_CFG_ENABLE;
		else
			conf_reg &= ~CRTC_CFG_ENABLE;
		dc_writel(adev, CURRENT_REG(dc_reg_data[DI_REG_INCREASE], dc_reg_data[DI_CRTC_CFG_REG], i), conf_reg);
	}
*/
}

static bool loonggpu_read_bios_from_rom(struct loonggpu_device *adev,
				  u8 *bios, u32 length_bytes)
{
	DRM_DEBUG_DRIVER("%s Not implemented\n", __func__);

	return true;
}

static int loonggpu_read_register(struct loonggpu_device *adev, u32 se_num,
			    u32 sh_num, u32 reg_offset, u32 *value)
{
	DRM_DEBUG_DRIVER("%s Not implemented\n", __func__);

	if (reg_offset == LOONGGPU_LG2XX_IP_STATE)
		*value = RREG32(LOONGGPU_LG2XX_IP_STATE);
	else
		*value = 0;

	return 0;
}

static int loonggpu_gpu_pci_config_reset(struct loonggpu_device *adev)
{
	u32 i;

	dev_info(adev->dev, "GPU pci config reset\n");

	/* disable BM */
	pci_clear_master(adev->pdev);
	/* reset */
	loonggpu_device_pci_config_reset(adev);

	udelay(100);

	/* wait for asic to come out of reset */
	for (i = 0; i < adev->usec_timeout; i++) {
		if (1) {
			/* enable BM */
			pci_set_master(adev->pdev);
			adev->has_hw_reset = true;
			return 0;
		}
		udelay(1);
	}
	return -EINVAL;
}

static int loonggpu_reset(struct loonggpu_device *adev)
{
	int r;
	if (adev->family_type == CHIP_LG200) {
		loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_GPIPE, LG2XX_ICMD32_SOP_GPIPE_RESET), 0, 0);
	}

	/*XXX Set pcie config regs not Need*/
	return 0;

	r = loonggpu_gpu_pci_config_reset(adev);

	return r;
}

static bool loonggpu_need_full_reset(struct loonggpu_device *adev)
{
	switch (adev->family_type) {
	case CHIP_LG100:
	case CHIP_LG200:
	case CHIP_LG210:
	case CHIP_LG211:
	default:
		/* change this when we support soft reset */
		return true;
	}
}

static void loonggpu_init_doorbell_index(struct loonggpu_device *adev)
{
	adev->doorbell_index.max_assignment = LOONGGPU_DOORBELL_MAX_ASSIGNMENT;
}

static const struct loonggpu_asic_funcs loonggpu_asic_funcs = {
	.read_bios_from_rom = &loonggpu_read_bios_from_rom,
	.read_register = &loonggpu_read_register,
	.reset = &loonggpu_reset,
	.set_vga_state = &loonggpu_vga_set_state,
	.get_clk = &loonggpu_get_clk,
	.need_full_reset = &loonggpu_need_full_reset,
	.init_doorbell_index = &loonggpu_init_doorbell_index,
};

static void config_gpu_uart(struct loonggpu_device *adev)
{
	if (!loonggpu_gpu_uart)
		return;

	switch(adev->chip) {
	case dev_7a2000:
		*(int *)(LG_TO_UNCAC(LS7A_CHIPCFG_REG_BASE | 0x444)) |= 0x10; /* 444: bit4 */
		break;
	case dev_2k2000:
		*(int *)(LG_TO_UNCAC(LS7A_CHIPCFG_REG_BASE | 0x444)) |= 0x8000000; /* 440: bit 59 (same as 444: bit 27)*/
		break;
	case dev_2k3000:
		*(int *)(LG_TO_UNCAC(LS7A_CHIPCFG_REG_BASE | 0x440)) |= 0x80000000; /* 440: bit 31*/
		break;
	default:
		DRM_DEBUG_DRIVER("No matching chip!\n");
		break;
	}
}

static int loonggpu_common_early_init(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	adev->asic_funcs = &loonggpu_asic_funcs;
	config_gpu_uart(adev);

	return 0;
}

static int loonggpu_common_late_init(void *handle)
{
	return 0;
}

static int loonggpu_common_sw_init(void *handle)
{
	return 0;
}

static int loonggpu_common_sw_fini(void *handle)
{
	return 0;
}

static int loonggpu_common_hw_init(void *handle)
{
	return 0;
}

static int loonggpu_common_hw_fini(void *handle)
{
	return 0;
}

static int loonggpu_common_suspend(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	return loonggpu_common_hw_fini(adev);
}

static int loonggpu_common_resume(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	config_gpu_uart(adev);

	return loonggpu_common_hw_init(adev);
}

static bool loonggpu_common_is_idle(void *handle)
{
	return true;
}

static int loonggpu_common_wait_for_idle(void *handle)
{
	return 0;
}

static int loonggpu_common_soft_reset(void *handle)
{
	return 0;
}

static const struct loonggpu_ip_funcs loonggpu_common_ip_funcs = {
	.name = "loonggpu_common",
	.early_init = loonggpu_common_early_init,
	.late_init = loonggpu_common_late_init,
	.sw_init = loonggpu_common_sw_init,
	.sw_fini = loonggpu_common_sw_fini,
	.hw_init = loonggpu_common_hw_init,
	.hw_fini = loonggpu_common_hw_fini,
	.suspend = loonggpu_common_suspend,
	.resume = loonggpu_common_resume,
	.is_idle = loonggpu_common_is_idle,
	.wait_for_idle = loonggpu_common_wait_for_idle,
	.soft_reset = loonggpu_common_soft_reset,
};

static const struct loonggpu_ip_block_version loonggpu_common_ip_block = {
	.type = LOONGGPU_IP_BLOCK_TYPE_COMMON,
	.major = 1,
	.minor = 0,
	.rev = 0,
	.funcs = &loonggpu_common_ip_funcs,
};

int loonggpu_set_ip_blocks(struct loonggpu_device *adev)
{
	switch (adev->family_type) {
	case CHIP_NO_GPU:
		loonggpu_device_ip_block_add(adev, &loonggpu_common_ip_block);
		loonggpu_device_ip_block_add(adev, &dc_ip_block);
		break;
	case CHIP_LG100:
	case CHIP_LG200:
	case CHIP_LG211:
		loonggpu_device_ip_block_add(adev, &loonggpu_common_ip_block);
		loonggpu_device_ip_block_add(adev, &mmu_ip_block);
		loonggpu_device_ip_block_add(adev, &zip_ip_block);
		loonggpu_device_ip_block_add(adev, &loonggpu_ih_ip_block);
		loonggpu_device_ip_block_add(adev, &dc_ip_block);
		loonggpu_device_ip_block_add(adev, &gfx_ip_block);
		loonggpu_device_ip_block_add(adev, &xdma_ip_block);
		if (adev->family_type >= CHIP_LG200) {
			loonggpu_device_ip_block_add(adev, &bpipe_ip_block);
			loonggpu_device_ip_block_add(adev, &dvfs_ip_block_v1_0_0);
		}
		break;
	case CHIP_LG210:
		loonggpu_device_ip_block_add(adev, &loonggpu_common_ip_block);
		loonggpu_device_ip_block_add(adev, &mmu_ip_block);
		loonggpu_device_ip_block_add(adev, &zip_ip_block);
		loonggpu_device_ip_block_add(adev, &loonggpu_ih_ip_block);
		loonggpu_device_ip_block_add(adev, &dc_ip_block);
		loonggpu_device_ip_block_add(adev, &gfx_ip_block);
		loonggpu_device_ip_block_add(adev, &xdma_ip_block);
		loonggpu_device_ip_block_add(adev, &bpipe_ip_block);
		loonggpu_device_ip_block_add(adev, &dpipe_ip_block);
		loonggpu_device_ip_block_add(adev, &epipe_ip_block);
		break;
	default:
		/* FIXME: not supported yet */
		return -EINVAL;
	}

	return 0;
}
