#include <linux/compat.h>

#include "loonggpu.h"
#include "loonggpu_drm.h"
#include "loonggpu_drv.h"

long loonggpu_kms_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	unsigned int nr = DRM_IOCTL_NR(cmd);
	int ret;

	if (nr < DRM_COMMAND_BASE)
		return 0;

	ret = loonggpu_drm_ioctl(filp, cmd, arg);

	return ret;
}
