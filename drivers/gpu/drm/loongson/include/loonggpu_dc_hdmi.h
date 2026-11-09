#ifndef __LOONGGPU_HDMI_H__
#define __LOONGGPU_HDMI_H__

#define HDMI_PACKET_ENABLE_CTRL         BIT(1)
#define HDMI_VIDEO_LEADING_GB_DISABLE   BIT(3)

int ls7a2000_hdmi_audio_init(struct loonggpu_dc_crtc *crtc, int intf);
int ls7a2000_hdmi_noaudio_init(struct loonggpu_dc_crtc *crtc, int intf);
int ls7a2000_hdmi_init(struct loonggpu_dc_crtc *crtc, int intf);
void ls7a2000_hdmi_suspend(struct loonggpu_dc_crtc *crtc, int intf);
int ls7a2000_hdmi_resume(struct loonggpu_dc_crtc *crtc, int intf);
bool ls7a2000_hdmi_enable(struct loonggpu_dc_crtc *crtc, int intf, bool enable);
void ls7a2000_hdmi_pll_set(struct loonggpu_dc_crtc *crtc, int intf, int clock);
void ls7a2000_hdmi_i2c_set(struct loonggpu_dc_crtc *crtc, int intf, bool use_gpio_i2c);
void ls7a1000_hdmi_i2c_set(struct loonggpu_dc_crtc *crtc, int intf, bool use_gpio_i2c);

int ls2k3000_hdmi_audio_init(struct loonggpu_dc_crtc *crtc, int intf);
int ls2k3000_hdmi_noaudio_init(struct loonggpu_dc_crtc *crtc, int intf);
int ls2k3000_hdmi_init(struct loonggpu_dc_crtc *crtc, int intf);
void ls2k3000_hdmi_suspend(struct loonggpu_dc_crtc *crtc, int intf);
int ls2k3000_hdmi_resume(struct loonggpu_dc_crtc *crtc, int intf);
bool ls2k3000_hdmi_enable(struct loonggpu_dc_crtc *crtc, int intf, bool enable);
void ls2k3000_hdmi_pll_set(struct loonggpu_dc_crtc *crtc, int intf, int clock);
void ls2k3000_hdmi_i2c_set(struct loonggpu_dc_crtc *crtc, int intf, bool use_gpio_i2c);

int ls9a1000_hdmi_audio_init(struct loonggpu_dc_crtc *crtc, int intf);
int ls9a1000_hdmi_noaudio_init(struct loonggpu_dc_crtc *crtc, int intf);
int ls9a1000_hdmi_init(struct loonggpu_dc_crtc *crtc, int intf);
void ls9a1000_hdmi_suspend(struct loonggpu_dc_crtc *crtc, int intf);
int ls9a1000_hdmi_resume(struct loonggpu_dc_crtc *crtc, int intf);
bool ls9a1000_hdmi_enable(struct loonggpu_dc_crtc *crtc, int intf, bool enable);
void ls9a1000_hdmi_pll_set(struct loonggpu_dc_crtc *crtc, int intf, int clock);
void ls9a1000_hdmi_i2c_set(struct loonggpu_dc_crtc *crtc, int intf, bool use_gpio_i2c);
void ls9a1000_hdmi_timing_set(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing, int intf);

#endif /* __LOONGGPU_HDMI_H__ */
