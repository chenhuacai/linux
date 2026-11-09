#ifndef __LOONGGPU_MODE_H__
#define __LOONGGPU_MODE_H__

#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fixed.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/clients/drm_fbdev_helper.h>

#include "loonggpu_irq.h"
#include "loonggpu_dc_irq.h"
#include "loonggpu_dc_interface.h"

struct loonggpu_bo;
struct loonggpu_device;
struct loonggpu_encoder;

#define to_loonggpu_crtc(x) container_of(x, struct loonggpu_crtc, base)
#define to_loonggpu_connector(x) container_of(x, struct loonggpu_connector, base)
#define to_loonggpu_encoder(x) container_of(x, struct loonggpu_encoder, base)
#define to_loonggpu_framebuffer(x) container_of(x, struct loonggpu_framebuffer, base)
#define to_loonggpu_plane(x)	container_of(x, struct loonggpu_plane, base)

#define LOONGGPU_MAX_HPD_PINS 3
#define LOONGGPU_MAX_CRTCS 4
#define LOONGGPU_MAX_PLANES 4

enum loonggpu_rmx_type {
	RMX_OFF,
	RMX_FULL,
	RMX_CENTER,
	RMX_ASPECT
};

enum loonggpu_flip_status {
	LOONGGPU_FLIP_NONE,
	LOONGGPU_FLIP_PENDING,
	LOONGGPU_FLIP_SUBMITTED
};

enum loonggpu_flip_copy_status {
	LOONGGPU_FLIP_COPY_UNKNOWN,
	LOONGGPU_FLIP_COPY_NONE,
	LOONGGPU_FLIP_COPY_DONE
};

struct loonggpu_display_funcs {
	/* get frame count */
	u32 (*vblank_get_counter)(struct loonggpu_device *adev, int crtc);
	/* set backlight level */
	void (*backlight_set_level)(struct loonggpu_encoder *loonggpu_encoder,
				    u8 level);
	/* get backlight level */
	u8 (*backlight_get_level)(struct loonggpu_encoder *loonggpu_encoder);
	/* hotplug detect */
	bool (*hpd_sense)(struct loonggpu_device *adev);
	u32 (*hpd_get_gpio_reg)(struct loonggpu_device *adev);
	/* pageflipping */
	void (*page_flip)(struct loonggpu_device *adev,
			  int crtc_id, u64 crtc_base, bool async);
	int (*page_flip_get_scanoutpos)(struct loonggpu_device *adev, int crtc,
					u32 *vbl, u32 *position);
};

struct loonggpu_framebuffer {
	struct drm_framebuffer base;

	/* caching for later use */
	uint64_t address;
};

struct loonggpu_fbdev {
	struct drm_fb_helper helper;
	struct loonggpu_framebuffer rfb;
	struct list_head fbdev_list;
	struct loonggpu_device *adev;
};

struct loonggpu_crtc {
	struct drm_crtc base;
	int crtc_id;
	bool enabled;
	bool can_tile;
	uint32_t crtc_offset;
	enum dc_irq_source irq_source_vsync;
	struct drm_gem_object *cursor_bo;
	uint64_t cursor_addr;
	int cursor_x;
	int cursor_y;
	int cursor_hot_x;
	int cursor_hot_y;
	int cursor_width;
	int cursor_height;
	int max_cursor_width;
	int max_cursor_height;
	struct mutex cursor_lock;
	struct loonggpu_flip_work *pflip_works;
	enum loonggpu_flip_status pflip_status;
	u32 lb_vblank_lead_lines;
	struct drm_display_mode hw_mode;
	struct drm_pending_vblank_event *event;

	struct work_struct disp_work;
	int disp_work_status;
	int disp_scale;
	int pflip_copy;
};

struct loonggpu_plane {
	struct drm_plane base;
	enum drm_plane_type plane_type;
};

struct loonggpu_encoder {
	struct drm_encoder base;
	uint32_t encoder_enum;
	uint32_t encoder_id;
	struct loonggpu_bridge_phy *bridge;
};

enum loonggpu_connector_audio {
	LOONGGPU_AUDIO_DISABLE = 0,
	LOONGGPU_AUDIO_ENABLE = 1,
	LOONGGPU_AUDIO_AUTO = 2
};

struct loonggpu_connector {
	struct drm_connector base;
	uint32_t connector_id;
	uint32_t devices;
	enum loonggpu_connector_audio audio;
	int num_modes;
	int pixel_clock_mhz;
	struct mutex hpd_lock;
	enum dc_irq_source irq_source_i2c;
	enum dc_irq_source irq_source_hpd[MAX_DC_INTERFACES];
	enum dc_irq_source irq_source_vga_hpd;
};

struct loonggpu_mode_info {
	bool mode_config_initialized;
	struct loonggpu_crtc *crtcs[LOONGGPU_MAX_CRTCS];
	struct loonggpu_plane *planes[LOONGGPU_MAX_PLANES];
	struct loonggpu_connector *connectors[4];
	struct loonggpu_encoder *encoders[4];
	struct loonggpu_backlight *backlights[2];
	struct loonggpu_connector *dp_connector;
	/* underscan */
	struct drm_property *underscan_property;
	struct drm_property *underscan_hborder_property;
	struct drm_property *underscan_vborder_property;
	/* audio */
	struct drm_property *audio_property;
	/* pointer to fbdev info structure */
	struct loonggpu_fbdev *rfbdev;
	int			num_crtc; /* number of crtcs */
	int			num_hpd; /* number of hpd pins */
	int			num_i2c;
	int			disp_priority;
	const struct loonggpu_display_funcs *funcs;
	const enum drm_plane_type *plane_type;
};

/* Driver internal use only flags of loonggpu_display_get_crtc_scanoutpos() */
#define DRM_SCANOUTPOS_VALID        (1 << 0)
#define DRM_SCANOUTPOS_IN_VBLANK    (1 << 1)
#define DRM_SCANOUTPOS_ACCURATE     (1 << 2)
#define USE_REAL_VBLANKSTART		(1 << 30)
#define GET_DISTANCE_TO_VBLANKSTART	(1 << 31)

int loonggpu_display_get_crtc_scanoutpos(struct drm_device *dev,
			unsigned int pipe, unsigned int flags, int *vpos,
			int *hpos, ktime_t *stime, ktime_t *etime,
			const struct drm_display_mode *mode);

int loonggpu_display_framebuffer_init(struct drm_device *dev,
				   struct loonggpu_framebuffer *rfb,
				   const struct drm_mode_fb_cmd2 *mode_cmd,
				   const struct drm_format_info *info,
				   struct drm_gem_object *obj);

int loonggpu_fbdev_init(struct loonggpu_device *adev);
void loonggpu_fbdev_fini(struct loonggpu_device *adev);
void loonggpu_fbdev_set_suspend(struct loonggpu_device *adev, int state);
int loonggpu_fbdev_total_size(struct loonggpu_device *adev);
bool loonggpu_fbdev_robj_is_fb(struct loonggpu_device *adev, struct loonggpu_bo *robj);
int loonggpu_align_pitch(struct loonggpu_device *adev, int width, int bpp, bool tiled);
int loonggpu_display_modeset_create_props(struct loonggpu_device *adev);
int loonggpufb_create(struct drm_fb_helper *helper, struct drm_fb_helper_surface_size *sizes);
#endif /* __LOONGGPU_MODE_H__ */
