#include <linux/dma-buf.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>

#include "loonggpu.h"
#include <drm/drm_vblank.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/clients/drm_fbdev_helper.h>
#include "loonggpu_drm.h"
#include "loonggpu_helper.h"
#include "loonggpu_display.h"

MODULE_IMPORT_NS("DMA_BUF");

static void loonggpu_display_flip_callback(struct dma_fence *f,
					 struct dma_fence_cb *cb)
{
	struct loonggpu_flip_work *work =
		container_of(cb, struct loonggpu_flip_work, cb);

	dma_fence_put(f);
	schedule_work(&work->flip_work.work);
}

static bool loonggpu_display_flip_handle_fence(struct loonggpu_flip_work *work,
					     struct dma_fence **f)
{
	struct dma_fence *fence = *f;

	if (fence == NULL)
		return false;

	*f = NULL;

	if (!dma_fence_add_callback(fence, &work->cb,
				    loonggpu_display_flip_callback))
		return true;

	dma_fence_put(fence);
	return false;
}

static void loonggpu_display_flip_work_func(struct work_struct *__work)
{
	struct delayed_work *delayed_work =
		container_of(__work, struct delayed_work, work);
	struct loonggpu_flip_work *work =
		container_of(delayed_work, struct loonggpu_flip_work, flip_work);
	struct loonggpu_device *adev = work->adev;
	struct loonggpu_crtc *loonggpu_crtc = adev->mode_info.crtcs[work->crtc_id];

	struct drm_crtc *crtc = &loonggpu_crtc->base;
	unsigned long flags;
	unsigned i;
	int vpos, hpos;

	if (loonggpu_display_flip_handle_fence(work, &work->excl))
		return;

	for (i = 0; i < work->shared_count; ++i)
		if (loonggpu_display_flip_handle_fence(work, &work->shared[i]))
			return;

	/* Wait until we're out of the vertical blank period before the one
	 * targeted by the flip
	 */
	if (loonggpu_crtc->enabled &&
	    (loonggpu_display_get_crtc_scanoutpos(adev->ddev, work->crtc_id, 0,
						&vpos, &hpos, NULL, NULL,
						&crtc->hwmode)
	     & (DRM_SCANOUTPOS_VALID | DRM_SCANOUTPOS_IN_VBLANK)) ==
	    (DRM_SCANOUTPOS_VALID | DRM_SCANOUTPOS_IN_VBLANK) &&
	    (int)(work->target_vblank -
		  loonggpu_get_vblank_counter_kms(adev->ddev, loonggpu_crtc->crtc_id)) > 0) {
		schedule_delayed_work(&work->flip_work, usecs_to_jiffies(1000));
		return;
	}

	/* We borrow the event spin lock for protecting flip_status */
	spin_lock_irqsave(&crtc->dev->event_lock, flags);

	/* Do the flip (mmio) */
	adev->mode_info.funcs->page_flip(adev, work->crtc_id, work->base, work->async);

	/* Set the flip status */
	loonggpu_crtc->pflip_status = LOONGGPU_FLIP_SUBMITTED;
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);


	DRM_DEBUG_DRIVER("crtc:%d[%p], pflip_stat:LOONGGPU_FLIP_SUBMITTED, work: %p,\n",
					 loonggpu_crtc->crtc_id, loonggpu_crtc, work);

}

/*
 * Handle unpin events outside the interrupt handler proper.
 */
static void loonggpu_display_unpin_work_func(struct work_struct *__work)
{
	struct loonggpu_flip_work *work =
		container_of(__work, struct loonggpu_flip_work, unpin_work);
	int r;

	/* unpin of the old buffer */
	r = loonggpu_bo_reserve(work->old_abo, true);
	if (likely(r == 0)) {
		r = loonggpu_bo_unpin(work->old_abo);
		if (unlikely(r != 0)) {
			DRM_ERROR("failed to unpin buffer after flip\n");
		}
		loonggpu_bo_unreserve(work->old_abo);
	} else
		DRM_ERROR("failed to reserve buffer after flip\n");

	loonggpu_bo_unref(&work->old_abo);
	kfree(work->shared);
	kfree(work);
}

int loonggpu_display_crtc_page_flip_target(struct drm_crtc *crtc,
				struct drm_framebuffer *fb,
				struct drm_pending_vblank_event *event,
				uint32_t page_flip_flags, uint32_t target,
				struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_device *dev = crtc->dev;
	struct loonggpu_device *adev = dev->dev_private;
	struct loonggpu_crtc *loonggpu_crtc = to_loonggpu_crtc(crtc);
	struct drm_gem_object *obj;
	struct loonggpu_flip_work *work;
	struct loonggpu_bo *new_abo;
	lg_dma_resv_t *resv;
	unsigned long flags;
	u64 tiling_flags;
	int i, r;
	void *fb_vaddr = NULL;

	work = kzalloc(sizeof *work, GFP_KERNEL);
	if (work == NULL)
		return -ENOMEM;

	INIT_DELAYED_WORK(&work->flip_work, loonggpu_display_flip_work_func);
	INIT_WORK(&work->unpin_work, loonggpu_display_unpin_work_func);

	work->event = event;
	work->adev = adev;
	work->crtc_id = loonggpu_crtc->crtc_id;
	work->async = (page_flip_flags & DRM_MODE_PAGE_FLIP_ASYNC) != 0;

	/* schedule unpin of the old buffer */
	obj = crtc->primary->fb->obj[0];

	/* take a reference to the old object */
	work->old_abo = gem_to_loonggpu_bo(obj);
	loonggpu_bo_ref(work->old_abo);

	obj = fb->obj[0];
	new_abo = gem_to_loonggpu_bo(obj);

	/* pin the new buffer */
	r = loonggpu_bo_reserve(new_abo, false);
	if (unlikely(r != 0)) {
		DRM_ERROR("failed to reserve new abo buffer before flip\n");
		goto cleanup;
	}

	r = loonggpu_bo_pin(new_abo, loonggpu_display_supported_domains(adev));
	if (unlikely(r != 0)) {
		DRM_ERROR("failed to pin new abo buffer before flip\n");
		goto unreserve;
	}

	r = loonggpu_ttm_alloc_gart(&new_abo->tbo);
	if (unlikely(r != 0)) {
		DRM_ERROR("%p bind failed\n", new_abo);
		goto unpin;
	}

	resv = to_dma_resv(new_abo);
	r = lg_dma_resv_get_fences_rcu(resv, &work->excl, &work->shared_count,
				       &work->shared, LG_DMA_RESV_USAGE_WRITE);
	if (unlikely(r != 0)) {
		DRM_ERROR("failed to get fences for buffer\n");
		goto unpin;
	}

	loonggpu_bo_get_tiling_flags(new_abo, &tiling_flags);

	loonggpu_bo_kmap(new_abo, &fb_vaddr);

	work->base = loonggpu_bo_gpu_offset(new_abo);

	loonggpu_bo_unreserve(new_abo);

	work->target_vblank = target - (uint32_t)drm_crtc_vblank_count(crtc) +
		loonggpu_get_vblank_counter_kms(dev, work->crtc_id);

	/* we borrow the event spin lock for protecting flip_wrok */
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (loonggpu_crtc->pflip_status != LOONGGPU_FLIP_NONE) {
		DRM_DEBUG_DRIVER("flip queue: crtc already busy\n");
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		r = -EBUSY;
		goto pflip_cleanup;
	}

	loonggpu_crtc->pflip_status = LOONGGPU_FLIP_PENDING;
	loonggpu_crtc->pflip_works = work;


	DRM_DEBUG_DRIVER("crtc:%d[%p], pflip_stat:LOONGGPU_FLIP_PENDING, work: %p,\n",
					 loonggpu_crtc->crtc_id, loonggpu_crtc, work);
	/* update crtc fb */
	crtc->primary->fb = fb;
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	loonggpu_display_flip_work_func(&work->flip_work.work);
	return 0;

pflip_cleanup:
	if (unlikely(loonggpu_bo_reserve(new_abo, false) != 0)) {
		DRM_ERROR("failed to reserve new abo in error path\n");
		goto cleanup;
	}
unpin:
	if (unlikely(loonggpu_bo_unpin(new_abo) != 0)) {
		DRM_ERROR("failed to unpin new abo in error path\n");
	}
unreserve:
	loonggpu_bo_unreserve(new_abo);

cleanup:
	loonggpu_bo_unref(&work->old_abo);
	dma_fence_put(work->excl);
	for (i = 0; i < work->shared_count; ++i)
		dma_fence_put(work->shared[i]);
	kfree(work->shared);
	kfree(work);

	return r;
}

static int loonggpu_display_framebuffer_dirty(struct drm_framebuffer *fb,
					   struct drm_file *file,
					   unsigned flags, unsigned color,
					   struct drm_clip_rect *clips,
					   unsigned num_clips)
{
	int ret;

	if (!fb || !gem_to_loonggpu_bo(fb->obj[0])->is_other_gpu_share)
		return 0;

	drm_modeset_lock_all(fb->dev);
	ret = dma_buf_begin_cpu_access(fb->obj[0]->import_attach->dmabuf, DMA_FROM_DEVICE);
	drm_modeset_unlock_all(fb->dev);
	return ret;
}

static const struct drm_framebuffer_funcs loonggpu_fb_funcs = {
	.destroy = drm_gem_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
	.dirty = loonggpu_display_framebuffer_dirty,
};

uint32_t loonggpu_display_supported_domains(struct loonggpu_device *adev)
{
	uint32_t domain = LOONGGPU_GEM_DOMAIN_VRAM;

	return domain;
}

int loonggpu_display_framebuffer_init(struct drm_device *dev,
				    struct loonggpu_framebuffer *rfb,
				    const struct drm_mode_fb_cmd2 *mode_cmd,
				    const struct drm_format_info *info,
				    struct drm_gem_object *obj)
{
	int ret;
	rfb->base.obj[0] = obj;
	lg_drm_helper_mode_fill_fb_struct(dev, &rfb->base, info, mode_cmd);
	ret = drm_framebuffer_init(dev, &rfb->base, &loonggpu_fb_funcs);
	if (ret) {
		rfb->base.obj[0] = NULL;
		return ret;
	}
	return 0;
}

struct drm_framebuffer *
loonggpu_display_user_framebuffer_create(struct drm_device *dev,
				       struct drm_file *file_priv,
				       const struct drm_format_info *info,
				       const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_gem_object *obj;
	struct loonggpu_framebuffer *loonggpu_fb;
	struct loonggpu_device *adev = dev->dev_private;
	struct loonggpu_bo *bo;
	int ret;

	obj = drm_gem_object_lookup(file_priv, mode_cmd->handles[0]);
	if (obj ==  NULL) {
		dev_err(&adev->pdev->dev, "No GEM object associated to handle 0x%08X, "
			"can't create framebuffer\n", mode_cmd->handles[0]);
		return ERR_PTR(-ENOENT);
	}

	/* Handle is imported dma-buf, so cannot be migrated to VRAM for scanout */
	bo = gem_to_loonggpu_bo(obj);
	bo->is_other_gpu_share = !!bo->prime_shared_count;
	if (obj->import_attach && !bo->is_other_gpu_share) {
		DRM_DEBUG_KMS("Cannot create framebuffer from imported dma_buf\n");
		return ERR_PTR(-EINVAL);
	}

	loonggpu_fb = kzalloc(sizeof(*loonggpu_fb), GFP_KERNEL);
	if (loonggpu_fb == NULL) {
		lg_drm_gem_object_put(obj);
		return ERR_PTR(-ENOMEM);
	}

	ret = loonggpu_display_framebuffer_init(dev, loonggpu_fb, mode_cmd, info, obj);
	if (ret) {
		kfree(loonggpu_fb);
		lg_drm_gem_object_put(obj);
		return ERR_PTR(ret);
	}

	return &loonggpu_fb->base;
}

static const struct drm_prop_enum_list loonggpu_audio_enum_list[] = {
	{ LOONGGPU_AUDIO_DISABLE, "off" },
	{ LOONGGPU_AUDIO_ENABLE, "on" },
	{ LOONGGPU_AUDIO_AUTO, "auto" },
};

int loonggpu_display_modeset_create_props(struct loonggpu_device *adev)
{
	int sz;

	drm_mode_create_scaling_mode_property(adev->ddev);

	sz = ARRAY_SIZE(loonggpu_audio_enum_list);
	adev->mode_info.audio_property =
		drm_property_create_enum(adev->ddev, 0,
					 "audio",
					 loonggpu_audio_enum_list, sz);

	return 0;
}

void loonggpu_display_update_priority(struct loonggpu_device *adev)
{
	/* adjustment options for the display watermarks */
	if ((loonggpu_disp_priority == 0) || (loonggpu_disp_priority > 2))
		adev->mode_info.disp_priority = 0;
	else
		adev->mode_info.disp_priority = loonggpu_disp_priority;

}

int loonggpu_display_get_crtc_scanoutpos(struct drm_device *dev,
			unsigned int pipe, unsigned int flags, int *vpos,
			int *hpos, ktime_t *stime, ktime_t *etime,
			const struct drm_display_mode *mode)
{
	u32 vbl = 0, position = 0;
	int vbl_start, vbl_end, vtotal, ret = 0;
	bool in_vbl = true;

	struct loonggpu_device *adev = dev->dev_private;

	/* preempt_disable_rt() should go right here in PREEMPT_RT patchset. */

	/* Get optional system timestamp before query. */
	if (stime)
		*stime = ktime_get();

	if (loonggpu_display_page_flip_get_scanoutpos(adev, pipe, &vbl, &position) == 0)
		ret |= DRM_SCANOUTPOS_VALID;

	/* Get optional system timestamp after query. */
	if (etime)
		*etime = ktime_get();

	/* preempt_enable_rt() should go right here in PREEMPT_RT patchset. */

	/* Decode into vertical and horizontal scanout position. */
	*vpos = position & 0x1fff;
	*hpos = (position >> 16) & 0x1fff;

	/* Valid vblank area boundaries from gpu retrieved? */
	if (vbl > 0) {
		/* Yes: Decode. */
		ret |= DRM_SCANOUTPOS_ACCURATE;
		vbl_start = vbl & 0x1fff;
		vbl_end = (vbl >> 16) & 0x1fff;
	} else {
		/* No: Fake something reasonable which gives at least ok results. */
		vbl_start = mode->crtc_vdisplay;
		vbl_end = 0;
	}

	/* Called from driver internal vblank counter query code? */
	if (flags & GET_DISTANCE_TO_VBLANKSTART) {
	    /* Caller wants distance from real vbl_start in *hpos */
	    *hpos = *vpos - vbl_start;
	}

	/* Fudge vblank to start a few scanlines earlier to handle the
	 * problem that vblank irqs fire a few scanlines before start
	 * of vblank. Some driver internal callers need the true vblank
	 * start to be used and signal this via the USE_REAL_VBLANKSTART flag.
	 *
	 * The cause of the "early" vblank irq is that the irq is triggered
	 * by the line buffer logic when the line buffer read position enters
	 * the vblank, whereas our crtc scanout position naturally lags the
	 * line buffer read position.
	 */
	if (!(flags & USE_REAL_VBLANKSTART))
		vbl_start -= adev->mode_info.crtcs[pipe]->lb_vblank_lead_lines;

	/* Test scanout position against vblank region. */
	if ((*vpos < vbl_start) && (*vpos >= vbl_end))
		in_vbl = false;

	/* In vblank? */
	if (in_vbl)
	    ret |= DRM_SCANOUTPOS_IN_VBLANK;

	/* Called from driver internal vblank counter query code? */
	if (flags & GET_DISTANCE_TO_VBLANKSTART) {
		/* Caller wants distance from fudged earlier vbl_start */
		*vpos -= vbl_start;
		return ret;
	}

	/* Check if inside vblank area and apply corrective offsets:
	 * vpos will then be >=0 in video scanout area, but negative
	 * within vblank area, counting down the number of lines until
	 * start of scanout.
	 */

	/* Inside "upper part" of vblank area? Apply corrective offset if so: */
	if (in_vbl && (*vpos >= vbl_start)) {
		vtotal = mode->crtc_vtotal;
		*vpos = *vpos - vtotal;
	}

	/* Correct for shifted end of vbl at vbl_end. */
	*vpos = *vpos - vbl_end;

	return ret;
}
