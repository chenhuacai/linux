#include <linux/irq.h>
#include <linux/pm_runtime.h>
#include <linux/pci.h>

#include "loonggpu.h"
#include <drm/drm_vblank.h>
#include <drm/drm_crtc_helper.h>
#include "loonggpu_drm.h"

#include "loonggpu_ih.h"
#include "loonggpu_trace.h"
#include "loonggpu_dc_irq.h"
#include "loonggpu_dc_reg.h"
#include "loonggpu_irq.h"

#define LOONGGPU_WAIT_IDLE_TIMEOUT 200

/**
 * loonggpu_irq_reset_work_func - execute GPU reset
 *
 * @work: work struct pointer
 *
 * Execute scheduled GPU reset (Cayman+).
 * This function is called when the IRQ handler thinks we need a GPU reset.
 */
static void loonggpu_irq_reset_work_func(struct work_struct *work)
{
	struct loonggpu_device *adev = container_of(work, struct loonggpu_device,
						  reset_work);

	loonggpu_device_gpu_recover(adev, NULL, false);
}

/**
 * loonggpu_irq_disable_all - disable *all* interrupts
 *
 * @adev: loonggpu device pointer
 *
 * Disable all types of interrupts from all sources.
 */
void loonggpu_irq_disable_all(struct loonggpu_device *adev)
{
	unsigned long irqflags;
	unsigned i, j, k;
	int r;

	spin_lock_irqsave(&adev->irq.lock, irqflags);
	for (i = 0; i < LOONGGPU_IH_CLIENTID_MAX; ++i) {
		if (!adev->irq.client[i].sources)
			continue;

		for (j = 0; j < LOONGGPU_MAX_IRQ_SRC_ID; ++j) {
			struct loonggpu_irq_src *src = adev->irq.client[i].sources[j];

			if (!src || !src->funcs || !src->funcs->set || !src->num_types)
				continue;

			for (k = 0; k < src->num_types; ++k) {
				atomic_set(&src->enabled_types[k], 0);
				r = src->funcs->set(adev, src, k,
						    LOONGGPU_IRQ_STATE_DISABLE);
				if (r)
					DRM_ERROR("error disabling interrupt (%d)\n",
						  r);
			}
		}
	}
	spin_unlock_irqrestore(&adev->irq.lock, irqflags);
}

/**
 * loonggpu_irq_handler - IRQ handler
 *
 * @irq: IRQ number (unused)
 * @arg: pointer to DRM device
 *
 * IRQ handler for loonggpu driver (all ASICs).
 *
 * Returns:
 * result of handling the IRQ, as defined by &irqreturn_t
 */
irqreturn_t loonggpu_irq_handler(int irq, void *arg)
{
	struct drm_device *dev = (struct drm_device *) arg;
	struct loonggpu_device *adev = dev->dev_private;
	irqreturn_t ret;

	ret = loonggpu_ih_process(adev);
	if (ret == IRQ_HANDLED)
		pm_runtime_mark_last_busy(dev->dev);
	return ret;
}

/**
 * loonggpu_msi_ok - check whether MSI functionality is enabled
 *
 * @adev: loonggpu device pointer (unused)
 *
 * Checks whether MSI functionality has been disabled via module parameter
 * (all ASICs).
 *
 * Returns:
 * *true* if MSIs are allowed to be enabled or *false* otherwise
 */
static bool loonggpu_msi_ok(struct loonggpu_device *adev)
{
	if (loonggpu_msi == 1)
		return true;
	else if (loonggpu_msi == 0)
		return false;

	return true;
}

/**
 * loonggpu_irq_init - initialize interrupt handling
 *
 * @adev: loonggpu device pointer
 *
 * Sets up work functions for hotplug and reset interrupts, enables MSI
 * functionality, initializes vblank, hotplug and reset interrupt handling.
 *
 * Returns:
 * 0 on success or error code on failure
 */
int loonggpu_irq_init(struct loonggpu_device *adev)
{
	int r = 0, irq;

	spin_lock_init(&adev->irq.lock);

	/* Enable MSI if not disabled by module parameter */
	adev->irq.msi_enabled = false;
	if (loonggpu_msi_ok(adev)) {
		int ret = pci_enable_msi(adev->pdev);
		if (!ret) {
			adev->irq.msi_enabled = true;
			dev_dbg(adev->dev, "loonggpu: using MSI.\n");
		}
	}

	INIT_WORK(&adev->reset_work, loonggpu_irq_reset_work_func);

	adev->irq.installed = true;
	r = lg_drm_irq_install(adev->ddev, adev->pdev, &irq, 0, loonggpu_irq_handler);
	if (r) {
		adev->irq.installed = false;
		cancel_work_sync(&adev->reset_work);
		return r;
	}
	adev->irq.irq = irq;
	DRM_DEBUG("loonggpu: irq initialized.\n");
	return 0;
}

/**
 * loonggpu_irq_fini - shut down interrupt handling
 *
 * @adev: loonggpu device pointer
 *
 * Tears down work functions for hotplug and reset interrupts, disables MSI
 * functionality, shuts down vblank, hotplug and reset interrupt handling,
 * turns off interrupts from all sources (all ASICs).
 */
void loonggpu_irq_fini(struct loonggpu_device *adev)
{
	unsigned i, j;

	if (adev->irq.installed) {
		lg_drm_irq_uninstall(adev->ddev, adev->irq.irq);
		adev->irq.installed = false;
		if (adev->irq.msi_enabled)
			pci_disable_msi(adev->pdev);
		cancel_work_sync(&adev->reset_work);
	}

	for (i = 0; i < LOONGGPU_IH_CLIENTID_MAX; ++i) {
		if (!adev->irq.client[i].sources)
			continue;

		for (j = 0; j < LOONGGPU_MAX_IRQ_SRC_ID; ++j) {
			struct loonggpu_irq_src *src = adev->irq.client[i].sources[j];

			if (!src)
				continue;

			kfree(src->enabled_types);
			src->enabled_types = NULL;
			if (src->data) {
				kfree(src->data);
				kfree(src);
				adev->irq.client[i].sources[j] = NULL;
			}
		}
		kfree(adev->irq.client[i].sources);
		adev->irq.client[i].sources = NULL;
	}
}

/**
 * loonggpu_irq_add_id - register IRQ source
 *
 * @adev: loonggpu device pointer
 * @client_id: client id
 * @src_id: source id
 * @source: IRQ source pointer
 *
 * Registers IRQ source on a client.
 *
 * Returns:
 * 0 on success or error code otherwise
 */
int loonggpu_irq_add_id(struct loonggpu_device *adev,
		      unsigned client_id, unsigned src_id,
		      struct loonggpu_irq_src *source)
{
	if (client_id >= LOONGGPU_IH_CLIENTID_MAX)
		return -EINVAL;

	if (src_id >= LOONGGPU_MAX_IRQ_SRC_ID)
		return -EINVAL;

	if (!source->funcs)
		return -EINVAL;

	if (!adev->irq.client[client_id].sources) {
		adev->irq.client[client_id].sources =
			kcalloc(LOONGGPU_MAX_IRQ_SRC_ID,
				sizeof(struct loonggpu_irq_src *),
				GFP_KERNEL);
		if (!adev->irq.client[client_id].sources)
			return -ENOMEM;
	}

	if (adev->irq.client[client_id].sources[src_id] != NULL)
		return -EINVAL;

	if (source->num_types && !source->enabled_types) {
		atomic_t *types;

		types = kcalloc(source->num_types, sizeof(atomic_t),
				GFP_KERNEL);
		if (!types)
			return -ENOMEM;

		source->enabled_types = types;
	}

	adev->irq.client[client_id].sources[src_id] = source;
	return 0;
}

/**
 * loonggpu_irq_dispatch - dispatch IRQ to IP blocks
 *
 * @adev: loonggpu device pointer
 * @entry: interrupt vector pointer
 *
 * Dispatches IRQ to IP blocks.
 */
void loonggpu_irq_dispatch(struct loonggpu_device *adev,
			 struct loonggpu_iv_entry *entry)
{
	unsigned client_id = entry->client_id;
	unsigned src_id = entry->src_id;
	struct loonggpu_irq_src *src;
	int r;

	trace_loonggpu_iv(entry);

	if (client_id >= LOONGGPU_IH_CLIENTID_MAX) {
		DRM_DEBUG("Invalid client_id in IV: %d\n", client_id);
		return;
	}

	if (src_id >= LOONGGPU_MAX_IRQ_SRC_ID) {
		DRM_DEBUG("Invalid src_id in IV: %d\n", src_id);
		return;
	}

	if (!adev->irq.client[client_id].sources) {
		DRM_DEBUG("Unregistered interrupt client_id: %d src_id: %d\n",
			  client_id, src_id);
		return;
	}

	src = adev->irq.client[client_id].sources[src_id];
	if (!src) {
		DRM_DEBUG("Unhandled interrupt src_id: %d\n", src_id);
		return;
	}

	if (src->funcs && src->funcs->process)
		r = src->funcs->process(adev, src, entry);
	if (r)
		DRM_ERROR("error processing interrupt (%d)\n", r);
}

/**
 * loonggpu_irq_update - update hardware interrupt state
 *
 * @adev: loonggpu device pointer
 * @src: interrupt source pointer
 * @type: type of interrupt
 *
 * Updates interrupt state for the specific source (all ASICs).
 */
int loonggpu_irq_update(struct loonggpu_device *adev,
			     struct loonggpu_irq_src *src, unsigned type)
{
	unsigned long irqflags;
	enum loonggpu_interrupt_state state;
	int r;

	if (!src->funcs)
		return -EINVAL;

	spin_lock_irqsave(&adev->irq.lock, irqflags);

	/* We need to determine after taking the lock, otherwise
	   we might disable just enabled interrupts again */
	if (loonggpu_irq_enabled(adev, src, type))
		state = LOONGGPU_IRQ_STATE_ENABLE;
	else
		state = LOONGGPU_IRQ_STATE_DISABLE;

	r = src->funcs->set(adev, src, type, state);
	spin_unlock_irqrestore(&adev->irq.lock, irqflags);
	return r;
}

/**
 * loonggpu_irq_gpu_reset_resume_helper - update interrupt states on all sources
 *
 * @adev: loonggpu device pointer
 *
 * Updates state of all types of interrupts on all sources on resume after
 * reset.
 */
void loonggpu_irq_gpu_reset_resume_helper(struct loonggpu_device *adev)
{
	int i, j, k;

	for (i = 0; i < LOONGGPU_IH_CLIENTID_MAX; ++i) {
		if (!adev->irq.client[i].sources)
			continue;

		for (j = 0; j < LOONGGPU_MAX_IRQ_SRC_ID; ++j) {
			struct loonggpu_irq_src *src = adev->irq.client[i].sources[j];

			if (!src)
				continue;
			for (k = 0; k < src->num_types; k++)
				loonggpu_irq_update(adev, src, k);
		}
	}
}

/**
 * loonggpu_irq_get - enable interrupt
 *
 * @adev: loonggpu device pointer
 * @src: interrupt source pointer
 * @type: type of interrupt
 *
 * Enables specified type of interrupt on the specified source (all ASICs).
 *
 * Returns:
 * 0 on success or error code otherwise
 */
int loonggpu_irq_get(struct loonggpu_device *adev, struct loonggpu_irq_src *src,
		   unsigned type)
{
	if (!adev->irq.installed)
		return -ENOENT;

	if (type >= src->num_types)
		return -EINVAL;

	if (!src->enabled_types || !src->funcs || !src->funcs->set)
		return -EINVAL;

	if (atomic_inc_return(&src->enabled_types[type]) == 1)
		return loonggpu_irq_update(adev, src, type);

	return 0;
}

/**
 * loonggpu_irq_put - disable interrupt
 *
 * @adev: loonggpu device pointer
 * @src: interrupt source pointer
 * @type: type of interrupt
 *
 * Enables specified type of interrupt on the specified source (all ASICs).
 *
 * Returns:
 * 0 on success or error code otherwise
 */
int loonggpu_irq_put(struct loonggpu_device *adev, struct loonggpu_irq_src *src,
		   unsigned type)
{
	if (!adev->irq.installed)
		return -ENOENT;

	if (type >= src->num_types)
		return -EINVAL;

	if (!src->enabled_types || !src->funcs || !src->funcs->set)
		return -EINVAL;

	if (atomic_dec_and_test(&src->enabled_types[type]))
		return loonggpu_irq_update(adev, src, type);

	return 0;
}

/**
 * loonggpu_irq_enabled - check whether interrupt is enabled or not
 *
 * @adev: loonggpu device pointer
 * @src: interrupt source pointer
 * @type: type of interrupt
 *
 * Checks whether the given type of interrupt is enabled on the given source.
 *
 * Returns:
 * *true* if interrupt is enabled, *false* if interrupt is disabled or on
 * invalid parameters
 */
bool loonggpu_irq_enabled(struct loonggpu_device *adev, struct loonggpu_irq_src *src,
			unsigned type)
{
	if (!adev->irq.installed)
		return false;

	if (type >= src->num_types)
		return false;

	if (!src->enabled_types || !src->funcs || !src->funcs->set)
		return false;

	return !!atomic_read(&src->enabled_types[type]);
}
