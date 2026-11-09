#ifndef __LOONGGPU_DISPLAY_H__
#define __LOONGGPU_DISPLAY_H__

uint32_t loonggpu_display_supported_domains(struct loonggpu_device *adev);
struct drm_framebuffer *
loonggpu_display_user_framebuffer_create(struct drm_device *dev,
				       struct drm_file *file_priv,
				       const struct drm_format_info *info,
				       const struct drm_mode_fb_cmd2 *mode_cmd);
int loonggpu_display_crtc_page_flip_target(struct drm_crtc *crtc,
                                struct drm_framebuffer *fb,
                                struct drm_pending_vblank_event *event,
                                uint32_t page_flip_flags, uint32_t target,
                                struct drm_modeset_acquire_ctx *ctx);

#endif /* __LOONGGPU_DISPLAY_H__ */
