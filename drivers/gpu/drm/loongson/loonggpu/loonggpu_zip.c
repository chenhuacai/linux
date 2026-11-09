#include <linux/firmware.h>
#include <drm/drm_cache.h>
#include "loonggpu.h"
#include "loonggpu_zip.h"

/**
 * zip_meta_enable - gart enable
 *
 * @adev: loonggpu_device pointer
 *
 * This sets up the TLBs, programs the page tables for VMID0,
 * sets up the hw for VMIDs 1-15 which are allocated on
 * demand, and sets up the global locations for the LDS, GDS,
 * and GPUVM for FSA64 clients ().
 * Returns 0 for success, errors for failure.
 */
static int zip_meta_enable(struct loonggpu_device *adev, bool clear)
{
	int r;

	if (adev->zip_meta.robj == NULL) {
		dev_err(adev->dev, "No VRAM object for PCIE ZIP_META.\n");
		return -EINVAL;
	}
	r = loonggpu_zip_meta_vram_pin(adev);
	if (r)
		return r;

	if (clear)
		memset(adev->zip_meta.ptr, 0x00, adev->zip_meta.table_size);
	
	if (adev->family_type == CHIP_LG100) {
		loonggpu_cmd_exec(adev, GSCMD(GSCMD_ZIP, ZIP_DISABLE), 0, 0);
		loonggpu_cmd_exec(adev, GSCMDi(GSCMD_ZIP, ZIP_SET_BASE, 0), \
				lower_32_bits(adev->zip_meta.table_addr), upper_32_bits(adev->zip_meta.table_addr));

		loonggpu_cmd_exec(adev, GSCMDi(GSCMD_ZIP, ZIP_SET_MASK, 0), \
				lower_32_bits(adev->zip_meta.mask), upper_32_bits(adev->zip_meta.mask));

		loonggpu_cmd_exec(adev, GSCMD(GSCMD_ZIP, ZIP_ENABLE), 0, 0);
	} else if (adev->family_type == CHIP_LG200 ||
		   adev->family_type == CHIP_LG210 ||
		   adev->family_type == CHIP_LG211) {
		loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_ZIP, LG2XX_ICMD32_SOP_ZIP_ZDIS), 0, 0);
		loonggpu_cmd_exec(adev, LG2XX_ICMD32i(LG2XX_ICMD32_MOP_ZIP, LG2XX_ICMD32_SOP_ZIP_UTAGADDR, 0), \
				lower_32_bits(adev->zip_meta.table_addr), upper_32_bits(adev->zip_meta.table_addr));

		loonggpu_cmd_exec(adev, LG2XX_ICMD32i(LG2XX_ICMD32_MOP_ZIP, LG2XX_ICMD32_SOP_ZIP_UTAGMASK, 0), \
				lower_32_bits(adev->zip_meta.mask), upper_32_bits(adev->zip_meta.mask));

		loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_ZIP, LG2XX_ICMD32_SOP_ZIP_ZEN), 0, 0);
	} else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);


	DRM_INFO("PCIE ZIP META of %uM enabled (table at 0x%016llX).\n",
		 (unsigned)(adev->zip_meta.table_size >> 20),
		 (unsigned long long)adev->zip_meta.table_addr);
	adev->zip_meta.ready = true;
	return 0;
}

static int zip_meta_init(struct loonggpu_device *adev)
{
	int r;

	if (adev->zip_meta.robj) {
		WARN(1, "LOONGGPU PCIE ZIP_META already initialized\n");
		return 0;
	}
	/* Initialize common zip_meta structure */
	r = loonggpu_zip_meta_init(adev);
	if (r)
		return r;
	adev->zip_meta.table_size = adev->zip_meta.num_gpu_pages *
		LOONGGPU_GPU_PAGE_SIZE;
	adev->zip_meta.mask = roundup_pow_of_two(adev->zip_meta.table_size) - 1;
	adev->zip_meta.pte_flags = 0;
	return loonggpu_zip_meta_vram_alloc(adev);
}

/**
 * zip_meta_v1_0_gart_disable - zip meta disable
 *
 * @adev: loonggpu_device pointer
 *
 * This disables all zip meta page table ().
 */
static int zip_meta_disable(struct loonggpu_device *adev)
{
	if (adev->zip_meta.robj == NULL) {
		dev_err(adev->dev, "No VRAM object for PCIE ZIP_META.\n");
		return -EINVAL;
	}

	if (adev->family_type == CHIP_LG100)
		loonggpu_cmd_exec(adev, GSCMD(GSCMD_ZIP, ZIP_DISABLE), 0, 0);
	else if (adev->family_type == CHIP_LG200 ||
		 adev->family_type == CHIP_LG210 ||
		 adev->family_type == CHIP_LG211)
		loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_ZIP, LG2XX_ICMD32_SOP_ZIP_ZDIS), 0, 0);
	else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);

	adev->zip_meta.ready = false;

	loonggpu_zip_meta_vram_unpin(adev);
	return 0;
}

static int zip_early_init(void *handle)
{
	return 0;
}

static int zip_late_init(void *handle)
{
	return 0;
}

static int zip_sw_init(void *handle)
{
	int r;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	r = zip_meta_init(adev);
	if (r)
		return r;

	return 0;
}

static int zip_sw_fini(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	loonggpu_zip_meta_vram_free(adev);
	loonggpu_zip_meta_fini(adev);

	return 0;
}

static int zip_hw_init(void *handle)
{
	int r;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	r = zip_meta_enable(adev, true);
	if (r)
		return r;

	return r;
}

static int zip_hw_fini(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	zip_meta_disable(adev);

	return 0;
}

static int zip_suspend(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	zip_hw_fini(adev);

	return 0;
}

static int zip_resume(void *handle)
{
	int r;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	r = zip_meta_enable(adev, false);
	if (r)
		return r;

	return 0;
}

static bool zip_is_idle(void *handle)
{
	return true;
}

static bool zip_check_soft_reset(void *handle)
{
	return false;
}

static int zip_pre_soft_reset(void *handle)
{
	return 0;
}

static int zip_soft_reset(void *handle)
{
	return 0;
}

static int zip_post_soft_reset(void *handle)
{
	return 0;
}

static const struct loonggpu_ip_funcs zip_ip_funcs = {
	.name = "zip",
	.early_init = zip_early_init,
	.late_init = zip_late_init,
	.sw_init = zip_sw_init,
	.sw_fini = zip_sw_fini,
	.hw_init = zip_hw_init,
	.hw_fini = zip_hw_fini,
	.suspend = zip_suspend,
	.resume = zip_resume,
	.is_idle = zip_is_idle,
	.wait_for_idle = NULL,
	.check_soft_reset = zip_check_soft_reset,
	.pre_soft_reset = zip_pre_soft_reset,
	.soft_reset = zip_soft_reset,
	.post_soft_reset = zip_post_soft_reset,
};

const struct loonggpu_ip_block_version zip_ip_block = {
	.type = LOONGGPU_IP_BLOCK_TYPE_ZIP,
	.major = 1,
	.minor = 0,
	.rev = 0,
	.funcs = &zip_ip_funcs,
};
