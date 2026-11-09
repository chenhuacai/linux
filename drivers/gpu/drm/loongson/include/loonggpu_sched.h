#ifndef __LOONGGPU_SCHED_H__
#define __LOONGGPU_SCHED_H__

#include "loonggpu.h"

int loonggpu_to_sched_priority(int loonggpu_priority, enum drm_sched_priority *prio);
int loonggpu_sched_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *filp);

#endif /* __LOONGGPU_SCHED_H__ */
