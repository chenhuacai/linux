#ifndef __LOONGGPU_INTERFACE_H__
#define __LOONGGPU_INTERFACE_H__

enum dc_interface_type {
	INTERFACE_HDMI,
	INTERFACE_DVO,
	INTERFACE_VGA,
	INTERFACE_DP,
	INTERFACE_EDP
};

struct dc_interface {
	u32 type;
	u32 index;
	bool connected;
	bool enabled;
};

#include "loonggpu_dc_crtc.h"

struct loonggpu_connector;

void dc_interface_audio_set(struct loonggpu_connector *lconnector,
			    struct loonggpu_dc_crtc *crtc);
int dc_interface_noaudio_init(struct loonggpu_dc_crtc *crtc);
int dc_interface_audio_init(struct loonggpu_dc_crtc *crtc);
int dc_interface_init(struct loonggpu_dc_crtc *crtc);
void dc_interface_suspend(struct loonggpu_dc_crtc *crtc);
int dc_interface_resume(struct loonggpu_dc_crtc *crtc);
bool dc_interface_enable(struct loonggpu_dc_crtc *crtc, bool enable);
void dc_interface_pll_set(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing);
void dc_interface_pll_set(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing);
void dc_interface_i2c_set(struct loonggpu_dc_crtc *crtc, bool use_gpio_i2c);
bool dc_interface_status_changed(struct drm_connector *connector, struct loonggpu_dc_crtc *crtc);
#endif /* __LOONGGPU_INTERFACE_H__ */
