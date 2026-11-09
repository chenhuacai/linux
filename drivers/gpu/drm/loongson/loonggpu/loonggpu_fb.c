#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/vga_switcheroo.h>

#include "loonggpu.h"
#include <drm/drm_fourcc.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/clients/drm_fbdev_helper.h>
#include "loonggpu_drm.h"
#include "loonggpu_display.h"
#include "loonggpu_helper.h"

/* object hierarchy -
   this contains a helper + a loonggpu fb
   the helper contains a pointer to loonggpu framebuffer baseclass.
*/

static int
loonggpufb_open(struct fb_info *info, int user)
{
	struct drm_fb_helper *fb_helper = info->par;
	struct drm_device *ddev = fb_helper->dev;
	int ret;

	ret = pm_runtime_get_sync(ddev->dev);
	if (ret < 0 && ret != -EACCES) {
		pm_runtime_mark_last_busy(ddev->dev);
		pm_runtime_put_autosuspend(ddev->dev);
		return ret;
	}
	return 0;
}

static int
loonggpufb_release(struct fb_info *info, int user)
{
	struct drm_fb_helper *fb_helper = info->par;
	struct drm_device *ddev = fb_helper->dev;

	pm_runtime_mark_last_busy(ddev->dev);
	pm_runtime_put_autosuspend(ddev->dev);
	return 0;
}

static struct fb_ops loonggpufb_ops = {
	.owner = THIS_MODULE,
	DRM_FB_HELPER_DEFAULT_OPS,
	.fb_open = loonggpufb_open,
	.fb_release = loonggpufb_release,
#if defined(__FB_DEFAULT_IOMEM_OPS_DRAW)
	__FB_DEFAULT_IOMEM_OPS_DRAW
#else
	.fb_fillrect = drm_fb_helper_cfb_fillrect,
	.fb_copyarea = drm_fb_helper_cfb_copyarea,
	.fb_imageblit = drm_fb_helper_cfb_imageblit,
#endif
};


int loonggpu_align_pitch(struct loonggpu_device *adev, int width, int cpp, bool tiled)
{
	int aligned = width;

	aligned *= cpp;
	aligned += 255;
	aligned &= ~255;

	return aligned;
}

static void loonggpufb_destroy_pinned_object(struct drm_gem_object *gobj)
{
	struct loonggpu_bo *abo = gem_to_loonggpu_bo(gobj);
	int ret;

	ret = loonggpu_bo_reserve(abo, true);
	if (likely(ret == 0)) {
		loonggpu_bo_kunmap(abo);
		loonggpu_bo_unpin(abo);
		loonggpu_bo_unreserve(abo);
	}
	lg_drm_gem_object_put(gobj);
}

static int loonggpufb_create_pinned_object(struct loonggpu_fbdev *rfbdev,
					 struct drm_mode_fb_cmd2 *mode_cmd,
					 struct drm_gem_object **gobj_p)
{
	struct loonggpu_device *adev = rfbdev->adev;
	struct drm_gem_object *gobj = NULL;
	const struct drm_format_info *info;
	struct loonggpu_bo *abo = NULL;
	bool fb_tiled = false; /* useful for testing */
	u32 tiling_flags = 0, domain;
	int ret;
	int aligned_size, size;
	int height = mode_cmd->height;
	u32 cpp;

	info = lg_drm_get_format_info(adev->ddev, mode_cmd);
	cpp = info->cpp[0];

	/* need to align pitch with crtc limits */
	mode_cmd->pitches[0] = loonggpu_align_pitch(adev, mode_cmd->width, cpp,
						  fb_tiled);
	domain = loonggpu_display_supported_domains(adev);

	height = ALIGN(mode_cmd->height, 8);
	size = mode_cmd->pitches[0] * height;
	aligned_size = ALIGN(size, PAGE_SIZE);

	ret = loonggpu_gem_object_create(adev, aligned_size, 0, domain,
				       LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED |
				       LOONGGPU_GEM_CREATE_VRAM_CONTIGUOUS |
				       LOONGGPU_GEM_CREATE_VRAM_CLEARED,
				       ttm_bo_type_device, NULL, &gobj);

	if (ret) {
		pr_err("failed to allocate framebuffer (%d)\n", aligned_size);
		return -ENOMEM;
	}
	abo = gem_to_loonggpu_bo(gobj);

	ret = loonggpu_bo_reserve(abo, false);
	if (unlikely(ret != 0))
		goto out_unref;

	if (tiling_flags) {
		ret = loonggpu_bo_set_tiling_flags(abo,
						 tiling_flags);
		if (ret)
			dev_err(adev->dev, "FB failed to set tiling flags\n");
	}

	ret = loonggpu_bo_pin(abo, domain);
	if (ret) {
		loonggpu_bo_unreserve(abo);
		goto out_unref;
	}

	ret = loonggpu_ttm_alloc_gart(&abo->tbo);
	if (ret) {
		loonggpu_bo_unreserve(abo);
		dev_err(adev->dev, "%p bind failed\n", abo);
		goto out_unref;
	}

	ret = loonggpu_bo_kmap(abo, NULL);
	loonggpu_bo_unreserve(abo);
	if (ret) {
		goto out_unref;
	}

	*gobj_p = gobj;
	return 0;
out_unref:
	loonggpufb_destroy_pinned_object(gobj);
	*gobj_p = NULL;
	return ret;
}

int loonggpufb_create(struct drm_fb_helper *helper,
			   struct drm_fb_helper_surface_size *sizes)
{
	struct loonggpu_fbdev *rfbdev = (struct loonggpu_fbdev *)helper;
	struct loonggpu_device *adev = rfbdev->adev;
	struct fb_info *info;
	struct drm_framebuffer *fb = NULL;
	struct drm_mode_fb_cmd2 mode_cmd;
	struct drm_gem_object *gobj = NULL;
	struct loonggpu_bo *abo = NULL;
	unsigned long tmp;
	int ret;

	mode_cmd.width = sizes->surface_width;
	mode_cmd.height = sizes->surface_height;

	if (sizes->surface_bpp == 24)
		sizes->surface_bpp = 32;

	mode_cmd.pixel_format = drm_mode_legacy_fb_format(sizes->surface_bpp,
							  sizes->surface_depth);

	ret = loonggpufb_create_pinned_object(rfbdev, &mode_cmd, &gobj);
	if (ret) {
		DRM_ERROR("failed to create fbcon object %d\n", ret);
		return ret;
	}

	abo = gem_to_loonggpu_bo(gobj);

	/* okay we have an object now allocate the framebuffer */
	info = lg_drm_fb_helper_alloc_info(helper);
	if (IS_ERR(info)) {
		ret = PTR_ERR(info);
		goto out;
	}

	info->par = rfbdev;
	info->skip_vt_switch = false;

	ret = loonggpu_display_framebuffer_init(adev->ddev, &rfbdev->rfb, &mode_cmd,
						lg_drm_get_format_info(adev->ddev, &mode_cmd), gobj);

	if (ret) {
		DRM_ERROR("failed to initialize framebuffer %d\n", ret);
		goto out;
	}

	fb = &rfbdev->rfb.base;

	/* setup helper */
	rfbdev->helper.fb = fb;

	info->fbops = &loonggpufb_ops;

	/* not alloc from VRAM but GTT */
	tmp = loonggpu_bo_gpu_offset(abo) - adev->gmc.vram_start;
	info->fix.smem_start = adev->gmc.aper_base + tmp;
	info->fix.smem_len = loonggpu_bo_size(abo);
	info->screen_base = loonggpu_bo_kptr(abo);
	info->screen_size = loonggpu_bo_size(abo);

	lg_drm_fb_helper_fill_info(info, &rfbdev->helper, sizes);

	/* setup aperture base/size for vesafb takeover */
	lg_set_info_apertures(info, adev);

	/* Use default scratch pixmap (info->pixmap.flags = FB_PIXMAP_SYSTEM) */

	if (info->screen_base == NULL) {
		ret = -ENOSPC;
		goto out;
	}

	DRM_INFO("fb mappable at 0x%lX\n",  info->fix.smem_start);
	DRM_INFO("vram apper at 0x%lX\n",  (unsigned long)adev->gmc.aper_base);
	DRM_INFO("size %lu\n", (unsigned long)loonggpu_bo_size(abo));
	DRM_INFO("fb depth is %d\n", fb->format->depth);
	DRM_INFO("   pitch is %d\n", fb->pitches[0]);
	DRM_INFO("screen_base 0x%llX\n", (u64)info->screen_base);
	DRM_INFO("screen_size 0x%lX\n", info->screen_size);

	return 0;

out:
	if (abo) {

	}
	if (fb && ret) {
		lg_drm_gem_object_put(gobj);
		drm_framebuffer_unregister_private(fb);
		drm_framebuffer_cleanup(fb);
		kfree(fb);
	}
	return ret;
}

static int loonggpu_fbdev_destroy(struct drm_device *dev, struct loonggpu_fbdev *rfbdev)
{
	struct loonggpu_framebuffer *rfb = &rfbdev->rfb;

	lg_drm_fb_helper_unregister_info(&rfbdev->helper);

	if (rfb->base.obj[0]) {
		loonggpufb_destroy_pinned_object(rfb->base.obj[0]);
		rfb->base.obj[0] = NULL;
		drm_framebuffer_unregister_private(&rfb->base);
		drm_framebuffer_cleanup(&rfb->base);
	}
	drm_fb_helper_fini(&rfbdev->helper);

	return 0;
}

static const struct drm_fb_helper_funcs loonggpu_fb_helper_funcs = {
};

int loonggpu_fbdev_init(struct loonggpu_device *adev)
{
	struct loonggpu_fbdev *rfbdev;
	int bpp_sel = 32;
	int ret;

	/* don't init fbdev on hw without DCE */
	if (!adev->mode_info.mode_config_initialized)
		return 0;

	/* don't init fbdev if there are no connectors */
	if (list_empty(&adev->ddev->mode_config.connector_list))
		return 0;

	rfbdev = kzalloc(sizeof(struct loonggpu_fbdev), GFP_KERNEL);
	if (!rfbdev)
		return -ENOMEM;

	rfbdev->adev = adev;
	adev->mode_info.rfbdev = rfbdev;

	lg_drm_fb_helper_prepare(adev->ddev, &rfbdev->helper,
				bpp_sel, &loonggpu_fb_helper_funcs);

	ret = lg_drm_fb_helper_init(adev->ddev, &rfbdev->helper,
				 LOONGGPUFB_CONN_LIMIT);
	if (ret) {
		kfree(rfbdev);
		return ret;
	}

	if (!rfbdev->helper.client.dev) {
		ret = drm_client_init(adev->ddev, &rfbdev->helper.client,
					"drm_fb_helper", NULL);
		if (ret) {
			kfree(rfbdev);
			return ret;
		}
	}

	lg_drm_fb_helper_initial_config(&rfbdev->helper, bpp_sel);

	return 0;
}

void loonggpu_fbdev_fini(struct loonggpu_device *adev)
{
	if (!adev->mode_info.rfbdev)
		return;

	loonggpu_fbdev_destroy(adev->ddev, adev->mode_info.rfbdev);
	kfree(adev->mode_info.rfbdev);
	adev->mode_info.rfbdev = NULL;
}

void loonggpu_fbdev_set_suspend(struct loonggpu_device *adev, int state)
{
	if (adev->mode_info.rfbdev)
		drm_fb_helper_set_suspend_unlocked(&adev->mode_info.rfbdev->helper,
						   state);
}

int loonggpu_fbdev_total_size(struct loonggpu_device *adev)
{
	struct loonggpu_bo *robj;
	int size = 0;

	if (!adev->mode_info.rfbdev)
		return 0;

	robj = gem_to_loonggpu_bo(adev->mode_info.rfbdev->rfb.base.obj[0]);
	size += loonggpu_bo_size(robj);
	return size;
}

bool loonggpu_fbdev_robj_is_fb(struct loonggpu_device *adev, struct loonggpu_bo *robj)
{
	if (!adev->mode_info.rfbdev)
		return false;
	if (robj == gem_to_loonggpu_bo(adev->mode_info.rfbdev->rfb.base.obj[0]))
		return true;
	return false;
}
