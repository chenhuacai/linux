#include <linux/firmware.h>
#include "loonggpu.h"
#include "loonggpu_cp.h"

#define MAJOR_SHIFT			0
#define MINOR_SHIFT			8
#define REVISION_SHIFT			16
#define FAMILY_SHIFT			8
#define MAJOR_MASK			0xff
#define MINOR_MASK			0xff
#define REVISION_MASK			0xff
#define HWINF_MASK			0xfff
#define FAMILY_MASK			0xf

static int loonggpu_cp_wait_for_idle(struct loonggpu_device *adev)
{
	if (loonggpu_cp_wait_done(adev) == true)
		return 0;

	return -ETIMEDOUT;
}

int loonggpu_cp_enable(struct loonggpu_device *adev, bool enable)
{
	int i;
	u32 tmp = RREG32(LOONGGPU_EC_CTRL);

	if (enable) {
		tmp |= 1;
	} else {
		tmp &= ~1;
		for (i = 0; i < adev->gfx.num_gfx_rings; i++)
			adev->gfx.gfx_ring[i].ready = false;
	}
	WREG32(LOONGGPU_EC_CTRL, tmp);
	mdelay(100);
	if (enable)
		return loonggpu_cp_wait_for_idle(adev);
	else
		return 0;
}

static void loonggpu_free_microcode(struct loonggpu_device *adev)
{
	release_firmware(adev->gfx.cp_fw);
	adev->gfx.cp_fw = NULL;
}

static int loonggpu_init_microcode(struct loonggpu_device *adev)
{
	const char *chip_name;
	char fw_name[30];
	int err;

	DRM_DEBUG("\n");

	switch (adev->family_type) {
	case CHIP_LG100:
		chip_name = "lg100";
		break;
	case CHIP_LG200:
		chip_name = "lg200";
		break;
	case CHIP_LG210:
		chip_name = "lg210";
		break;
	case CHIP_LG211:
		chip_name = "lg211";
		break;
	default:
		BUG();
	}

	snprintf(fw_name, sizeof(fw_name), "loonggpu/%s_cp.bin", chip_name);
	err = request_firmware(&adev->gfx.cp_fw, fw_name, adev->dev);
	if (err)
		goto out;
	adev->gfx.cp_fw_version = 0;
	adev->gfx.cp_feature_version = 0;

out:
	if (err) {
		dev_err(adev->dev,
			"gfx8: Failed to load firmware \"%s\"\n",
			fw_name);
		release_firmware(adev->gfx.cp_fw);
		adev->gfx.cp_fw = NULL;
	}
	return err;
}

int loonggpu_cp_gfx_load_microcode(struct loonggpu_device *adev)
{
	const __le32 *fw_data;
	unsigned i, fw_size, fw_wptr;

	if (!adev->gfx.cp_fw)
		return -EINVAL;

	loonggpu_cp_enable(adev, false);

	/* CP */
	fw_data = (const __le32 *)(adev->gfx.cp_fw->data);
	fw_size = adev->gfx.cp_fw->size;

	if (fw_size > 0x10000)
		return -EINVAL;

	for (i = 0; i < fw_size; i += 4)
		WREG32(LOONGGPU_FW_WPORT, le32_to_cpup(fw_data++));

	fw_wptr = RREG32(LOONGGPU_FW_WPTR);
	if (adev->family_type >= CHIP_LG210) {
		if (((fw_size + 3) / 4) != ((fw_wptr + 3)/4))
			return -EINVAL;
	} else {
		if (fw_size != fw_wptr)
			return -EINVAL;
	}

	return 0;
}

static void loonggpu_get_version(struct loonggpu_device *adev)
{
	u32 cp_fw_version;
	u32 cp_feature_version;
	u32 hw_inf;
	u32 hw_version;
	u8 fw_major;
	u8 fw_minor;
	u8 fw_revision;

	if (adev->family_type == CHIP_LG100) {
		cp_fw_version = RREG32(LOONGGPU_FW_VERSION_OFFSET);
		cp_feature_version = RREG32(LOONGGPU_HW_FEATURE_OFFSET);
	}
	else if (adev->family_type == CHIP_LG200 ||
		 adev->family_type == CHIP_LG210 ||
		 adev->family_type == CHIP_LG211) {
		cp_fw_version = RREG32(LOONGGPU_LG2XX_FW_VERSION_OFFSET);
		cp_feature_version = 0;
	}
	else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);

	fw_major = (cp_fw_version >> MAJOR_SHIFT & MAJOR_MASK);
	fw_minor = (cp_fw_version >> MINOR_SHIFT) & MINOR_MASK;
	fw_revision = (cp_fw_version >> REVISION_SHIFT) & REVISION_MASK;
	hw_inf = RREG32(LOONGGPU_HWINF);
	hw_inf &= HWINF_MASK;
	hw_version = hw_inf;
	adev->gfx.cp_fw_version = cp_fw_version;
	adev->gfx.cp_feature_version = cp_feature_version;
	DRM_INFO("GPU Family: LG%x00 series LG%x, Feature:0x%08x", (hw_inf >> FAMILY_SHIFT) & FAMILY_MASK, hw_version, cp_feature_version);
	DRM_INFO("Firmware Version: %d.%d.%d", fw_major, fw_minor, fw_revision);
}

int loonggpu_cp_init(struct loonggpu_device *adev)
{
	int r;

	r = loonggpu_init_microcode(adev);
	if (r) {
		DRM_ERROR("Failed to load gfx firmware!\n");
		return r;
	}

	r = loonggpu_cp_gfx_load_microcode(adev);
	if (r)
		return r;

	r = loonggpu_cp_enable(adev, true);
	if (r)
		return r;

	loonggpu_get_version(adev);

	return 0;
}

int loonggpu_cp_fini(struct loonggpu_device *adev)
{
	loonggpu_free_microcode(adev);

	return 0;
}
