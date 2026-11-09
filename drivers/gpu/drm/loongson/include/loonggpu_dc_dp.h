#ifndef __LOONGGPU_DP_H__
#define __LOONGGPU_DP_H__

#define DP_LINK_1P62G                       6
#define DP_LINK_2P16G                       8
#define DP_LINK_2P43G                       9
#define DP_LINK_2P7G                        10
#define DP_LINK_3P24G                       12
#define DP_LINK_4P32G                       16
#define DP_LINK_5P4G                        20
#define DP_LINK_8P1G                        30
#define DP_LINK_X1                          0x1
#define DP_LINK_X2                          0x2
#define DP_LINK_X4                          0x4

#define DP_PHY_1P62G                        0x0
#define DP_PHY_2P16G                        0x1
#define DP_PHY_2P43G                        0x2
#define DP_PHY_2P7G                         0x3
#define DP_PHY_3P24G                        0x4
#define DP_PHY_4P32G                        0x5
#define DP_PHY_5P4G                         0x6
#define DP_PHY_8P1G                         0x7
#define DP_PHY_X1                           0x0
#define DP_PHY_X2                           0x1
#define DP_PHY_X4                           0x2

/* DPCD ADDR */
#define DPCD_REV_ADDR                           0x0
#define DPCD_MAX_LINK_RATE_ADDR                 0x1
#define DPCD_MAX_LANE_COUNT_ADDR                0x2
#define DPCD_TRAINING_AUX_RD_INTERVAL_ADDR      0xE
#define DPCD_SET_POWER_ADDR                     0x600
#define DPCD_LINK_BW_SET_ADDR                   0x100
#define DPCD_LANE_COUNT_SET_ADDR                0x101
#define DPCD_TRAINING_PATTERN_SET_ADDR          0x102
#define DPCD_TRAINING_LANE0_SET_ADDR            0x103
#define DPCD_TRAINING_LANE1_SET_ADDR            0x104
#define DPCD_TRAINING_LANE2_SET_ADDR            0x105
#define DPCD_TRAINING_LANE3_SET_ADDR            0x106
#define DPCD_DOWNSPREAD_CTRL_ADDR               0x107
#define DPCD_MSTM_CTRL_ADDR                     0x111
#define DPCD_LANE0_1_STATUS_ADDR                0x202
#define DPCD_LANE2_3_STATUS_ADDR                0x203
#define DPCD_LANE_ALIGN_STATUS_UPDATED_ADDR     0x204
#define DPCD_SINK_STATUS_ADDR                   0x205
#define DPCD_ADJUST_REQUEST_LANE0_1_ADDR        0x206
#define DPCD_ADJUST_REQUEST_LANE2_3_ADDR        0x207

/*TRAINING_PATTERN */
#define TRAINING_PATTERN_DISABLED      0x00  /* disable */
#define TRAINING_PATTERN_1             0x21  /* (bit1:0=01) */
#define TRAINING_PATTERN_2             0x22  /* (bit1:0=10) */
#define TRAINING_PATTERN_3             0x23  /* (bit1:0=11) */

/* POWER_STATE */
#define DP_POWER_STATE_D0              0x01  /* normal operation mode */
#define DP_POWER_STATE_D3              0x02  /* power down mode */


typedef struct aux_msg {
	unsigned int addr;
	unsigned int size;
	unsigned long data_low;
	unsigned long data_high;
} aux_msg_t;

typedef struct dp_feature {
	unsigned int dp_phy_rate;
	unsigned int dp_phy_xlane;
	unsigned int dp_pixclk;
	unsigned int dp_link_rate;
	unsigned int dp_link_xlane;
	unsigned int fixed_vsync_start;
	unsigned int fixed_vsync_end;
	unsigned int fixed_vsync_width;
} dp_feature_t;

typedef struct {
	int link_rate;
	int phy_rate;
	int clk;
} dp_rate_info_t;

typedef struct {
	unsigned int bw;
	unsigned int phy_rate;
	unsigned int phy_lane;
	unsigned int link_rate;
	unsigned int link_lane;
} dp_bandwidth_entry_t;

bool is_dp_hpd_irq(u32 reg);
void dp_aux_lock(struct loonggpu_device *adev, int intf, bool lock);
bool is_ls2k3000_laptop(struct loonggpu_dc_crtc *crtc);
unsigned int aux_config(struct loonggpu_device *adev, unsigned int rd_wr, aux_msg_t aux_msg, int intf);
int ls2k3000_dp_audio_init(struct loonggpu_dc_crtc *crtc, int intf);
int ls2k3000_dp_noaudio_init(struct loonggpu_dc_crtc *crtc, int intf);
int ls2k3000_dp_init(struct loonggpu_dc_crtc *crtc, int intf);
void ls2k3000_dp_suspend(struct loonggpu_dc_crtc *crtc, int intf);
int ls2k3000_dp_resume(struct loonggpu_dc_crtc *crtc, int intf);
bool ls2k3000_dp_enable(struct loonggpu_dc_crtc *crtc, int intf, bool enable);
void ls2k3000_dp_pll_set(struct loonggpu_dc_crtc *crtc, int intf, struct dc_timing_info *timing);
void l2k3000_dp_first_hdp_detect(struct loonggpu_dc_crtc *crtc, int intf);
void l2k3000_hpd_irq_handler(struct loonggpu_device *adev, struct loonggpu_iv_entry *entry);
void aux_preinit_for_edid_2k3000ca(struct loonggpu_device *adev, int intf);

int ls9a1000_dp_init(struct loonggpu_dc_crtc *crtc, int intf);
void ls9a1000_dp_pll_set(struct loonggpu_dc_crtc *crtc, int intf, struct dc_timing_info *timing);
bool ls9a1000_dp_enable(struct loonggpu_dc_crtc *crtc, int intf, bool enable);
void ls9a1000_dp_suspend(struct loonggpu_dc_crtc *crtc, int intf);
int ls9a1000_dp_resume(struct loonggpu_dc_crtc *crtc, int intf);
int ls9a1000_dp_audio_init(struct loonggpu_dc_crtc *crtc, int intf);
int ls9a1000_dp_noaudio_init(struct loonggpu_dc_crtc *crtc, int intf);
bool ls9a_dp_read_edid(struct loonggpu_device *adev, struct loonggpu_dc_crtc *dc_crtc, unsigned char *edid);

#endif /* __LOONGGPU_DP_H__ */
