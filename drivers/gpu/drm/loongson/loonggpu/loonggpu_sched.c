#include <linux/fdtable.h>
#include <linux/pid.h>
#include <linux/file.h>
#include "loonggpu_drm.h"
#include "loonggpu.h"
#include "loonggpu_sched.h"
#include "loonggpu_scheduler_helper.h"
#include "loonggpu_vm.h"

int loonggpu_to_sched_priority(int loonggpu_priority,
			enum drm_sched_priority *prio)
{
	switch (loonggpu_priority) {
	case LOONGGPU_CTX_PRIORITY_VERY_HIGH:
		*prio = LG_DRM_SCHED_PRIORITY_HIGH_HW;
		break;
	case LOONGGPU_CTX_PRIORITY_HIGH:
		*prio = LG_DRM_SCHED_PRIORITY_HIGH_SW;
		break;
	case LOONGGPU_CTX_PRIORITY_NORMAL:
		*prio = DRM_SCHED_PRIORITY_NORMAL;
		break;
	case LOONGGPU_CTX_PRIORITY_LOW:
	case LOONGGPU_CTX_PRIORITY_VERY_LOW:
		*prio = LG_DRM_SCHED_PRIORITY_LOW;
		break;
	case LOONGGPU_CTX_PRIORITY_UNSET:
		*prio = LG_DRM_SCHED_PRIORITY_UNSET;
		break;
	default:
		WARN(1, "Invalid context priority %d\n", loonggpu_priority);
		*prio = LG_DRM_SCHED_PRIORITY_INVALID;
		return -EINVAL;
	}

	return 0;
}

static int loonggpu_sched_process_priority_override(struct loonggpu_device *adev,
						  int fd,
						  enum drm_sched_priority priority)
{
	struct file *filp = fget(fd);
	struct drm_file *file;
	struct loonggpu_fpriv *fpriv;
	struct loonggpu_ctx *ctx;
	uint32_t id;

	if (!filp)
		return -EINVAL;

	file = filp->private_data;
	fpriv = file->driver_priv;
	idr_for_each_entry(&fpriv->ctx_mgr.ctx_handles, ctx, id)
		loonggpu_ctx_priority_override(ctx, priority);

	fput(filp);

	return 0;
}

int loonggpu_sched_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *filp)
{
	union drm_loonggpu_sched *args = data;
	struct loonggpu_device *adev = dev->dev_private;
	enum drm_sched_priority priority;
	int r;

	r = loonggpu_to_sched_priority(args->in.priority, &priority);
	if (args->in.flags || r == -EINVAL)
		return -EINVAL;

	switch (args->in.op) {
	case LOONGGPU_SCHED_OP_PROCESS_PRIORITY_OVERRIDE:
		r = loonggpu_sched_process_priority_override(adev,
							   args->in.fd,
							   priority);
		break;
	default:
		DRM_ERROR("Invalid sched op specified: %d\n", args->in.op);
		r = -EINVAL;
		break;
	}

	return r;
}
