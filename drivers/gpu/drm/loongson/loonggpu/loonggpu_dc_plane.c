#include <drm/drm_atomic_helper.h>
#include "loonggpu.h"
#include "loonggpu_display.h"
#include "loonggpu_dc_plane.h"
#include "loonggpu_helper.h"

static const uint32_t rgb_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static const u32 cursor_formats[] = {
	DRM_FORMAT_ARGB8888
};

static int dc_plane_helper_prepare_fb(struct drm_plane *plane,
				      struct drm_plane_state *new_state)
{
	struct loonggpu_framebuffer *afb;
	struct drm_gem_object *obj;
	struct loonggpu_device *adev;
	struct loonggpu_bo *rbo;
	uint32_t domain;
	void *fb_vaddr;
	int r;

	if (!new_state->fb) {
		DRM_DEBUG_DRIVER("No FB bound\n");
		return 0;
	}

	afb = to_loonggpu_framebuffer(new_state->fb);
	obj = new_state->fb->obj[0];
	rbo = gem_to_loonggpu_bo(obj);
	adev = loonggpu_ttm_adev(rbo->tbo.bdev);
	r = loonggpu_bo_reserve(rbo, false);
	if (unlikely(r != 0))
		return r;

	if (plane->type != DRM_PLANE_TYPE_CURSOR)
		domain = loonggpu_display_supported_domains(adev);
	else
		domain = LOONGGPU_GEM_DOMAIN_VRAM;

	if (rbo->is_other_gpu_share)
		domain = LOONGGPU_GEM_DOMAIN_GTT;

	r = loonggpu_bo_pin(rbo, domain);
	if (unlikely(r != 0)) {
		if (r != -ERESTARTSYS)
			DRM_ERROR("Failed to pin framebuffer with error %d\n", r);
		loonggpu_bo_unreserve(rbo);
		return r;
	}

	r = loonggpu_bo_kmap(rbo, &fb_vaddr);
	if (unlikely(r != 0)) {
		loonggpu_bo_unpin(rbo);
		loonggpu_bo_unreserve(rbo);
		DRM_ERROR("%p kmap failed\n", rbo);
		return r;
	}

	if (!rbo->is_other_gpu_share)
		afb->address = loonggpu_bo_gpu_offset(rbo);

	if (plane->type == DRM_PLANE_TYPE_PRIMARY) {
		DRM_DEBUG_DRIVER("fb kernel virtual addr: %p\n", fb_vaddr);
		DRM_DEBUG_DRIVER("fb physical addr: 0x%llx\n", afb->address);
	}

	loonggpu_bo_unreserve(rbo);
	loonggpu_bo_ref(rbo);

	return 0;
}

static void dc_plane_helper_cleanup_fb(struct drm_plane *plane,
				       struct drm_plane_state *old_state)
{
	struct loonggpu_bo *rbo;
	int r;

	if (!old_state->fb)
		return;

	rbo = gem_to_loonggpu_bo(old_state->fb->obj[0]);
	r = loonggpu_bo_reserve(rbo, false);
	if (unlikely(r)) {
		DRM_ERROR("failed to reserve rbo before unpin\n");
		return;
	}

	loonggpu_bo_unpin(rbo);
	loonggpu_bo_unreserve(rbo);
	loonggpu_bo_unref(&rbo);
}

static int dc_plane_atomic_check(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	return 0;
}

static const struct drm_plane_helper_funcs dc_plane_helper_funcs = {
	.prepare_fb = dc_plane_helper_prepare_fb,
	.cleanup_fb = dc_plane_helper_cleanup_fb,
	.atomic_check = dc_plane_atomic_check,
};

static void dc_plane_destroy(struct drm_plane *plane)
{
	struct loonggpu_plane *p = to_loonggpu_plane(plane);

	drm_plane_cleanup(plane);
	kfree(p);
}

static const struct drm_plane_funcs dc_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_helper_destroy,
	.atomic_create_state = drm_atomic_helper_plane_create_state,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

int loonggpu_dc_plane_init(struct loonggpu_device *adev,
			struct loonggpu_plane *aplane,
			unsigned long possible_crtcs)
{
	int res = -EPERM;

#if defined(LG_MODE_CONFIG_HAS_FB_MODIFIERS)
	adev->ddev->mode_config.fb_modifiers_not_supported = true;
#endif
	switch (aplane->base.type) {
	case DRM_PLANE_TYPE_PRIMARY:
		res = drm_universal_plane_init(
				adev->ddev,
				&aplane->base,
				possible_crtcs,
				&dc_plane_funcs,
				rgb_formats,
				ARRAY_SIZE(rgb_formats),
				NULL, aplane->base.type, NULL);
		break;
	case DRM_PLANE_TYPE_CURSOR:
		res = drm_universal_plane_init(
				adev->ddev,
				&aplane->base,
				possible_crtcs,
				&dc_plane_funcs,
				cursor_formats,
				ARRAY_SIZE(cursor_formats),
				NULL, aplane->base.type, NULL);
		break;
	default:
		break;
	}

	drm_plane_helper_add(&aplane->base, &dc_plane_helper_funcs);

	return res;
}

int initialize_plane(struct loonggpu_device *adev,
		     struct loonggpu_mode_info *mode_info,
		     int plane_id)
{
	struct loonggpu_plane *plane;
	unsigned long possible_crtcs;
	int ret = 0;

	plane = kzalloc(sizeof(struct loonggpu_plane), GFP_KERNEL);
	mode_info->planes[plane_id] = plane;

	if (!plane) {
		DRM_ERROR("KMS: Failed to allocate plane\n");
		return -ENOMEM;
	}
	plane->base.type = mode_info->plane_type[plane_id];

	possible_crtcs = 1 << plane_id;

	ret = loonggpu_dc_plane_init(adev, mode_info->planes[plane_id], possible_crtcs);

	if (ret) {
		DRM_ERROR("KMS: Failed to initialize plane\n");
		return ret;
	}

	return ret;
}
