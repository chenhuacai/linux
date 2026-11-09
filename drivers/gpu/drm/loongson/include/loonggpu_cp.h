#ifndef __LOONGGPU_CP_H__
#define __LOONGGPU_CP_H__

#include <linux/delay.h>

int loonggpu_cp_init(struct loonggpu_device *adev);

int loonggpu_cp_gfx_load_microcode(struct loonggpu_device *adev);
int loonggpu_cp_enable(struct loonggpu_device *adev, bool enable);

int loonggpu_cp_fini(struct loonggpu_device *adev);

static inline bool loonggpu_cp_wait_done(struct loonggpu_device *adev)
{
	int i;
	for (i = 0; i < adev->usec_timeout; i++) {

		if (RREG32(LOONGGPU_STATUS) == GSCMD_STS_DONE)
			return true;

		udelay(100);
	}

	dev_err(adev->dev, "\n loonggpu cp hang!!! \n");
	return false;
}

#endif /* __LOONGGPU_CP_H__ */
