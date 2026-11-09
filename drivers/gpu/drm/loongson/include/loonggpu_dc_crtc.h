#ifndef __DC_CRTC_H__
#define __DC_CRTC_H__

#include "loonggpu_dc.h"
#include "loonggpu_dc_interface.h"
#include "loonggpu_dc_video.h"

#define MAX_DC_INTERFACES	6
#define MAX_DC_HDMI	2

struct dc_primary_plane;

struct loonggpu_dc_crtc {
	struct crtc_resource *resource;
	struct loonggpu_dc *dc;
	struct list_head node;
	struct dc_timing_info *timing;
	int array_mode;
	unsigned int cursor;
	struct dc_interface intf[MAX_DC_INTERFACES];
	unsigned int interfaces;
	unsigned int hdmi_ctrl[MAX_DC_HDMI];
	struct loonggpu_dc_video dc_video;
};

enum fb_color_format {
	DC_FB_FORMAT_NONE = 0,
	DC_FB_FORMAT12,
	DC_FB_FORMAT15,
	DC_FB_FORMAT16,
	DC_FB_FORMAT24,
	DC_FB_FORMAT32 = DC_FB_FORMAT24
};

enum cursor_format {
	CUR_FORMAT_NONE,
	CUR_FORMAT_MONO,
	CUR_FORMAT_ARGB8888,
};

struct pixel_clock {
	u32 l2_div;
	u32 l1_loopc;
	u32 l1_frefc;
};

struct loonggpu_dc_crtc *dc_crtc_construct(struct loonggpu_dc *dc, struct crtc_resource *resource);
int loonggpu_dc_crtc_init(struct loonggpu_device *adev,
		       struct drm_plane *plane, uint32_t crtc_index);
u32 dc_vblank_get_counter(struct loonggpu_device *adev, int crtc_num);
int dc_crtc_get_scanoutpos(struct loonggpu_device *adev, int crtc_num, u32 *vbl, u32 *position);

void dc_crtc_destroy(struct loonggpu_dc_crtc *crtc);
bool dc_crtc_enable(struct loonggpu_dc_crtc *crtc, bool enable);
bool set_screen_open(struct loonggpu_dc_crtc *dc_crtc);
bool dc_crtc_timing_set(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing);
u32 dc_crtc_get_vblank_counter(struct loonggpu_dc_crtc *crtc);
int dc_set_vblank(struct drm_crtc *crtc, bool enable);
bool crtc_primary_plane_set(struct loonggpu_dc_crtc *crtc,
			    struct dc_primary_plane *primary);
enum drm_mode_status ls7a2000_dc_crtc_mode_valid(struct loonggpu_dc_crtc *crtc,
				const struct drm_display_mode *mode);
enum drm_mode_status ls2k2000_dc_crtc_mode_valid(struct loonggpu_dc_crtc *crtc,
				const struct drm_display_mode *mode);
enum drm_mode_status ls2k3000_dc_crtc_mode_valid(struct loonggpu_dc_crtc *crtc,
				const struct drm_display_mode *mode);
void ls2k2000_dc_crtc_cfg_adjust(u32 array_mode, u32 *crtc_cfg);
bool ls7a2000_dc_pll_set(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing);
bool ls2k3000_dc_pll_set(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing);
#endif /* __DC_CRTC_H__ */
