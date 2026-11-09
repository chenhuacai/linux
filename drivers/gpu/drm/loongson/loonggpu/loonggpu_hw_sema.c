#include "loonggpu.h"
#include "loonggpu_drm.h"
#include "loonggpu_hw_sema.h"
#include <drm/drm_debugfs.h>


static void loonggpu_sema_reset(struct loonggpu_device *adev, unsigned id)
{
	struct loonggpu_hw_sema_mgr *sema_mgr = &adev->hw_sema_mgr;
	struct loonggpu_hw_sema *sema = &sema_mgr->sema[id];

	mutex_lock(&sema_mgr->lock);
	sema->own = false;
	sema->pasid = 0ULL;
	sema->ctx = 0ULL;
	sema->vm = NULL;
	mutex_unlock(&sema_mgr->lock);
}

static int loonggpu_sema_grab_new(struct loonggpu_device *adev,
			       struct loonggpu_vm *vm,
			       struct drm_loonggpu_hw_sema *drm_sema)
{

	struct loonggpu_hw_sema_mgr *sema_mgr = &adev->hw_sema_mgr;
	struct loonggpu_hw_sema *idle;
	uint32_t ret = 0;

	mutex_lock(&sema_mgr->lock);

	list_for_each_entry(idle, &sema_mgr->sema_list, list) {
		if (idle->own == false)
			goto get_id;
	}

	if (&idle->list == &sema_mgr->sema_list) {
		ret = -ENODATA;
		drm_sema->id = ~0ULL;
		goto error;
	}

get_id:
	if (vm) {
		loonggpu_vm_set_task_info(vm);
		idle->pasid = vm->pasid;
		idle->vm = vm;
	}

	idle->ctx = drm_sema->ctx_id;
	idle->own = true;

	drm_sema->id = (idle - sema_mgr->sema);

error:
	mutex_unlock(&sema_mgr->lock);

	return ret;
}

int loonggpu_hw_sema_op_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	uint32_t ret = 0;
	uint64_t ops;
	struct loonggpu_device *adev = dev->dev_private;
	struct drm_loonggpu_hw_sema  *drm_sema = data;
	struct loonggpu_fpriv *fpriv = filp->driver_priv;
	struct loonggpu_vm *vm = &fpriv->vm;

	ops = drm_sema->ops;

	switch (ops) {
	case LOONGGPU_HW_SEMA_GET:
		ret = loonggpu_sema_grab_new(adev, vm, drm_sema);
		break;
	case LOONGGPU_HW_SEMA_PUT:
		loonggpu_sema_reset(adev, drm_sema->id);
		ret = 0;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

void loonggpu_sema_free(struct loonggpu_device *adev, struct loonggpu_vm *vm)
{
	struct loonggpu_hw_sema_mgr *sema_mgr = &adev->hw_sema_mgr;
	struct loonggpu_hw_sema *hw_sema;

	list_for_each_entry(hw_sema, &sema_mgr->sema_list, list) {
		if (hw_sema->vm == vm)
			loonggpu_sema_reset(adev, (hw_sema-sema_mgr->sema));
	}
}

int loonggpu_hw_sema_mgr_init(struct loonggpu_device *adev)
{
	unsigned i;

	struct loonggpu_hw_sema_mgr *sema_mgr = &adev->hw_sema_mgr;

	/* TODO Should get from EC*/
	sema_mgr->num_ids = LOONGGPU_NUM_SEMA;

	mutex_init(&sema_mgr->lock);
	INIT_LIST_HEAD(&sema_mgr->sema_list);

	for (i = 0; i < sema_mgr->num_ids; ++i) {
		loonggpu_sema_reset(adev, i);
		list_add_tail(&sema_mgr->sema[i].list, &sema_mgr->sema_list);
	}

	return 0;
}

void loonggpu_hw_sema_mgr_fini(struct loonggpu_device *adev)
{

	unsigned i;

	struct loonggpu_hw_sema_mgr *sema_mgr = &adev->hw_sema_mgr;

	mutex_destroy(&sema_mgr->lock);
	for (i = 0; i < LOONGGPU_NUM_SEMA; ++i) {
		loonggpu_sema_reset(adev, i);
	}

}


#if defined(CONFIG_DEBUG_FS)

static int loonggpu_debugfs_sema_info(struct seq_file *m, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *dev = node->minor->dev;
	struct loonggpu_device *adev = dev->dev_private;
	struct loonggpu_hw_sema_mgr *sema_mgr = &adev->hw_sema_mgr;
	struct loonggpu_hw_sema *sema;


	mutex_lock(&sema_mgr->lock);

	rcu_read_lock();
	list_for_each_entry(sema, &sema_mgr->sema_list, list) {

		if (sema->own) {

			if (sema->vm) {
				struct loonggpu_task_info task_info;
				loonggpu_vm_get_task_info(adev, sema->pasid, &task_info);
				seq_printf(m, "pid %8d process:%s \t",
					   task_info.pid,
					   task_info.task_name);
			}

			seq_printf(m, "id %d \tctx_id %d\n",
				   (u32)(sema - sema_mgr->sema), sema->ctx);

		} else
			seq_printf(m, "id %d available\n", (u32)(sema - sema_mgr->sema));

	}

	rcu_read_unlock();

	mutex_unlock(&sema_mgr->lock);

	return 0;
}

static const struct drm_info_list loonggpu_debugfs_sema_list[] = {
	{"loonggpu_hw_sema_info", &loonggpu_debugfs_sema_info, 0, NULL},
};
#endif
int loonggpu_debugfs_sema_init(struct loonggpu_device *adev)
{
#if defined(CONFIG_DEBUG_FS)
	return loonggpu_debugfs_add_files(adev, loonggpu_debugfs_sema_list, 1);
#endif
	return 0;
}
