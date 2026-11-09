#ifndef __LOONGGPU_DRV_H__
#define __LOONGGPU_DRV_H__

#include <linux/firmware.h>
#include <linux/platform_device.h>

#include "loonggpu_shared.h"

#define DRIVER_AUTHOR		"Loongson graphics driver team"

#define DRIVER_NAME		"loonggpu"
#define DRIVER_DESC		"Loongson LGxx GPU Driver"

long loonggpu_drm_ioctl(struct file *filp,
		      unsigned int cmd, unsigned long arg);

#endif /* __LOONGGPU_DRV_H__ */
