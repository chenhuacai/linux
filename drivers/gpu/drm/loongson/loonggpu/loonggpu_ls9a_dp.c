#include <linux/delay.h>
#include "loonggpu.h"
#include "loonggpu_dc.h"
#include "loonggpu_dc_dp.h"
#include "loonggpu_dc_crtc.h"
#include "loonggpu_dc_reg.h"
#include "loonggpu_dc_resource.h"
#include "loonggpu_helper.h"
#include "loonggpu_dc_vbios.h"
#include "loonggpu_dc_encoder.h"
#include "bridge_phy.h"

#define MAX_TU 52

static const dp_bandwidth_entry_t bw_table[] = {
	{ 162000 * 1 / 3, DP_PHY_1P62G, DP_PHY_X1 , DP_LINK_1P62G, DP_LINK_X1},
	{ 162000 * 2 / 3, DP_PHY_1P62G, DP_PHY_X2 , DP_LINK_1P62G, DP_LINK_X2},
	{ 162000 * 4 / 3, DP_PHY_1P62G, DP_PHY_X4 , DP_LINK_1P62G, DP_LINK_X4},
	{ 270000 * 1 / 3, DP_PHY_2P7G,  DP_PHY_X1 , DP_LINK_2P7G,  DP_LINK_X1},
	{ 270000 * 2 / 3, DP_PHY_2P7G,  DP_PHY_X2 , DP_LINK_2P7G,  DP_LINK_X2},
	{ 270000 * 4 / 3, DP_PHY_2P7G,  DP_PHY_X4 , DP_LINK_2P7G,  DP_LINK_X4},
	{ 540000 * 1 / 3, DP_PHY_5P4G,  DP_PHY_X1 , DP_LINK_5P4G,  DP_LINK_X1},
	{ 540000 * 2 / 3, DP_PHY_5P4G,  DP_PHY_X2 , DP_LINK_5P4G,  DP_LINK_X2},
	{ 540000 * 4 / 3, DP_PHY_5P4G,  DP_PHY_X4 , DP_LINK_5P4G,  DP_LINK_X4},
};

const dp_rate_info_t  rate_table[] = {
	{ DP_LINK_1P62G, DP_PHY_1P62G, 162 },
	{ DP_LINK_2P16G, DP_PHY_2P16G, 216 },
	{ DP_LINK_2P43G, DP_PHY_2P43G, 243 },
	{ DP_LINK_2P7G,  DP_PHY_2P7G,  270 },
	{ DP_LINK_3P24G, DP_PHY_3P24G, 324 },
	{ DP_LINK_4P32G, DP_PHY_4P32G, 432 },
	{ DP_LINK_5P4G,  DP_PHY_5P4G,  540 },
};

static inline int get_max_rate(unsigned int aux_rate)
{
	switch (aux_rate & 0xff) {
	case 0x6:
		return 162000;
	case 0xa:
		return 270000;
	case 0x14:
		return 540000;
	default:
		DRM_WARN("Invalid MAX_LINK_RATE value read: 0x%02x, use default value: 0x14\n", aux_rate & 0xff);
		return 540000;
	}
}

static inline int get_max_lane(unsigned int aux_lane)
{
	switch (aux_lane & 0xf) {
	case 0x1:
		return 1;
	case 0x2:
		return 2;
	case 0x4:
		return 4;
	default:
		DRM_WARN("Invalid MAX_LINK_LANE value read: 0x%02x, use default value: 0x4\n", aux_lane & 0xf);
		return 4;
	}
}

static inline int rate_lane_unsupported(int rate, int lane, int idx)
{
	if ((rate == 270000) && (idx >= 6))
		return 1;
	if ((rate == 162000) && (idx >= 3))
		return 1;
	if ((lane == 2) && (idx == 2 || idx == 5 || idx == 8))
		return 1;
	if ((lane == 1) && (idx == 1 || idx == 2 || idx == 4 || idx == 5 || idx == 7 || idx == 8))
		return 1;
	return 0;
}

static bool dp_check_bandwidth(u32 clock, dp_feature_t *dp_param)
{
	unsigned int i;

	for (i = 0; i < sizeof(bw_table) / sizeof(bw_table[0]); i++) {
		if (clock <= bw_table[i].bw) {
			dp_param->dp_phy_rate = bw_table[i].phy_rate;
			dp_param->dp_phy_xlane = bw_table[i].phy_lane;
			dp_param->dp_link_rate = bw_table[i].link_rate;
			dp_param->dp_link_xlane = bw_table[i].link_lane;
			dp_param->dp_pixclk = clock;

			return true;
		}
	}

	return false;
}

static unsigned int ls9a_aux_config(struct loonggpu_device *adev, unsigned int rd_wr, aux_msg_t aux_msg, int intf)
{
	unsigned int dp_detect_flag = 0;
	unsigned int val32;
	int retry_budget = 20;
	u32 value, err_reason;
	const char *op_str = (rd_wr == READ) ? "READ" : "WRITE";

	value = dc_readl(adev, gdc_reg->dp_reg[intf].aux_channel0);
	value |= 0x1 << 1;
	value &= ~(0x1 << 3);
	if (rd_wr == WRITE) {
		value |= (0x1 << 3);
	}
	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel0, value);

	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel1, aux_msg.addr & 0xfffff);
	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel2, aux_msg.size & 0xfffff);

	if (rd_wr == WRITE) {
		dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel3, aux_msg.data_low & 0xffffffff);
		dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel4, aux_msg.data_low >> 32);
		dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel5, aux_msg.data_high & 0xffffffff);
		dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel6, aux_msg.data_high >> 32);
	}

	value = dc_readl(adev, gdc_reg->dp_reg[intf].aux_channel0);
	value |= (0x1 << 2);
	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel0, value);

	do {
		msleep(10);
		val32 = dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor0);

		if ((val32 == 0) &&
			(dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor5) == 0) &&
			(dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor6) == 0) &&
			(dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor7) == 0)) {
			DRM_WARN("AUX %s: aux monitor0,5,6,7 is 0 !, addr:0x%x size:%d intf-%d\n", op_str, aux_msg.addr, aux_msg.size, intf);
			return -1;
		}

		if (val32 & 0x2) {
			DRM_DEBUG_DRIVER("AUX %s: dp transaction success\n", op_str);
			return 0;
		}

		if (val32 & 0x1) {
			DRM_INFO("AUX %s: dp transaction fail --> addr=0x%05x, size=%d.\n", op_str, aux_msg.addr, aux_msg.size);
			err_reason = dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor5);
			if ((err_reason & (1 << 4)) || (err_reason & (1 << 7))) { // Bit 4: sink_defer
				// Bit 4: sink_defer, Bit 7: i2c_defer
				const char *defer_type = (err_reason & (1 << 4)) ? "Sink" : "I2C";
				DRM_WARN("AUX %s: %s DEFER (device busy), %d retries left\n", op_str, defer_type, retry_budget);
				retry_budget--;
				if (retry_budget > 0) {
					value = dc_readl(adev, gdc_reg->dp_reg[intf].aux_channel0);
					value |= (0x1 << 2);
					dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel0, value);
					mdelay(10);
					continue;
				} else {
					DRM_ERROR("AUX %s: DEFER retries exhausted\n", op_str);
					goto fatal_error;
				}
			}

			if (err_reason & (1 << 0)) { // Bit 0: timeout
				DRM_WARN("AUX %s timeout, Retry remaining times: %d!\n", op_str, retry_budget - 1);
				retry_budget--;
				if (retry_budget > 0) {
					DRM_DEBUG_DRIVER("AUX %s FAILED, Retry remaining times: %d\n", op_str, retry_budget);
					continue;
				} else {
					DRM_ERROR("AUX %s FAILED, NO retry remaining\n", op_str);
					goto fatal_error;
				}
			} else if (err_reason & (1 << 1)) { // Bit 1: rx_len_out_bound
				DRM_ERROR("AUX %s ERROR: received data length error!\n", op_str);
				goto fatal_error;
			} else if (err_reason & (1 << 2)) { // Bit 2: reply_command_invalid
				DRM_ERROR("AUX %s ERROR: Invalid reply command!\n", op_str);
				goto fatal_error;
			} else if (err_reason & (1 << 3)) { // Bit 3: sink_nack
				DRM_ERROR("AUX %s ERROR: DP Sink device responds with NACK (address or command not accepted)!\n", op_str);
				goto fatal_error;
			} else if (err_reason & (1 << 5)) { // Bit 5: i2c mode and reply command invalid
				DRM_ERROR("AUX %s ERROR: i2c mode and reply command invalid!\n", op_str);
				goto fatal_error;
			} else if (err_reason & (1 << 6)) { // Bit 6: i2c mode and sink reply nack
				DRM_ERROR("AUX %s ERROR: i2c mode and sink reply nack!\n", op_str);
				goto fatal_error;
			} else {
				DRM_ERROR("AUX %s UNKNOW ERROR, err_reason: 0x%08x\n", op_str, err_reason);
				goto fatal_error;
			}
		}

		retry_budget--;

	} while (retry_budget > 0);

	DRM_INFO("AUX %s TIMEOUT, reg status:\n", op_str);
	DRM_INFO("AUX MON0:%#X, MON5:%#X, MON6:%#X, MON7:%#X\n",
			dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor0),
			dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor5),
			dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor6),
			dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor7));

	dp_detect_flag = 1;
	return dp_detect_flag;

fatal_error:
	DRM_INFO("AUX %s ERROR, reg status:\n", op_str);
	DRM_INFO("AUX MON0:%#X, MON5:%#X, MON6:%#X, MON7:%#X\n",
			val32,
			dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor5),
			dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor6),
			dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor7));

	return -1;
}

static void ls9a_dp_recheck_bandwidth(struct loonggpu_device *adev, 
				struct dc_timing_info *timing, dp_feature_t *dp_param, int intf)
{
	unsigned int i, aux_rate = 0, aux_lane = 0, tu_size = 0;
	unsigned int link_speed = 0, allow_fix = 0;
	unsigned int check_flag = 0;
	unsigned int max_tu_val = MAX_TU;
	unsigned int vback_porch = 0;
	aux_msg_t aux_data;
	int max_rate;
	int max_lane;
	int ret = 0;
	int range_flag = 0;
	bool last_index = 0;

	aux_data.addr = DPCD_MAX_LINK_RATE_ADDR;
	aux_data.size = 0;
	aux_data.data_low = 0;
	aux_data.data_high = 0;
	ret = ls9a_aux_config(adev, READ, aux_data, intf);
	if (ret != 0) {
		DRM_ERROR("Interface %d: Failed to read MAX_LINK_RATE! AUX returned: %d\n", intf, ret);
	}
	aux_rate = dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor1);

	aux_data.addr = DPCD_MAX_LANE_COUNT_ADDR;
	aux_data.size = 0;
	aux_data.data_low = 0;
	aux_data.data_high = 0;
	ret = ls9a_aux_config(adev, READ, aux_data, intf);
	if (ret != 0) {
		DRM_ERROR("Interface %d: Failed to read MAX_LINK_COUNT! AUX returned: %d\n", intf, ret);
	}
	aux_lane = dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor1);

	max_rate = get_max_rate(aux_rate);
	max_lane = get_max_lane(aux_lane);

	dp_param->fixed_vsync_width = 0;
	dp_param->dp_pixclk = timing->clock;
	vback_porch = timing->vtotal - timing->vsync_start;

	if (max_rate * max_lane > bw_table[ARRAY_SIZE(bw_table) - 1].bw * 3)
		range_flag = 1;

	for (i = 0; i < ARRAY_SIZE(bw_table); ) {
		const dp_bandwidth_entry_t *bw = &bw_table[i];

		if (bw->bw < timing->clock || rate_lane_unsupported(max_rate, max_lane, i)) {
			i++;
			continue;
		}

		dp_param->dp_phy_rate  = bw_table[i].phy_rate;
		dp_param->dp_phy_xlane = bw_table[i].phy_lane;
		dp_param->dp_link_rate = bw_table[i].link_rate;
		dp_param->dp_link_xlane = bw_table[i].link_lane;

		/* link speed */
		switch (dp_param->dp_link_rate) {
		case DP_LINK_1P62G:
			link_speed = 162000;
			break;
		case DP_LINK_2P7G:
			link_speed = 270000;
			break;
		case DP_LINK_5P4G:
			link_speed = 540000;
			break;
		default:
			break;
		}

		last_index = range_flag ? (i == ARRAY_SIZE(bw_table) - 1)
						: (max_rate * max_lane == bw_table[i].bw * 3);

		/* Calculate and check fixed vsync width */
		dp_param->fixed_vsync_width = (u32)((u64)65536 * 2 * timing->clock / link_speed / timing->htotal);
		if (dp_param->fixed_vsync_width >= vback_porch) {
			if (!last_index && !allow_fix) {
				i++;
				continue;
			}

			if (allow_fix) {
				/* second-pass fix */
				dp_param->fixed_vsync_end = timing->vtotal - 1;
				dp_param->fixed_vsync_start = dp_param->fixed_vsync_end - dp_param->fixed_vsync_width;
			}
		} else {
			dp_param->fixed_vsync_end = timing->vsync_start + dp_param->fixed_vsync_width;
			dp_param->fixed_vsync_start = timing->vsync_start;
		}

		tu_size = ((dp_param->dp_pixclk * 64) / bw_table[i].bw) + 1;

		if ((tu_size <= max_tu_val) && (dp_param->fixed_vsync_width < vback_porch)) {
			check_flag = 1;
			break;
		}

		if (last_index && allow_fix) {
			if (dp_param->dp_pixclk > (max_tu_val * bw_table[i].bw / 64))
				dp_param->dp_pixclk = (max_tu_val * bw_table[i].bw) / 64;
			check_flag = 1;
			break;
		}

		i++;
		/* first pass failed, retry with fix enabled */
		if (last_index && !check_flag && !allow_fix) {
			allow_fix = 1;
			i = 0;
		}
	}

	timing->fixed_vsync_start = dp_param->fixed_vsync_start;
	timing->fixed_vsync_end = dp_param->fixed_vsync_end;

	return;
}

static void ls9a_dp_phy_init(struct loonggpu_device *adev, dp_feature_t dp_param, int intf,
			uint32_t vswing, uint32_t preemp)
{
	uint64_t CLK_HS;
	uint64_t pixelclk_div_N;
	uint64_t pixelclk_div_F;
	uint32_t ln_vswing[4]= {0}, ln_preemp[4] = {0};
	uint32_t reg_val;
	uint32_t i;
	unsigned long timeout;

	/* enable phy vswing */
	reg_val = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg0 + 0x2c);
	reg_val |= (0xf << 6);
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0 + 0x2c, reg_val);

	reg_val = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg2);
	reg_val |= 0xf;
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg2, reg_val);

	/* set phy rate & lane */
	reg_val = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg0);
	reg_val |= (0x1 << 31);
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0, reg_val);

	reg_val = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg0);
	reg_val &= ~(0x7 << 0x8);
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0, reg_val);
	reg_val |= dp_param.dp_phy_rate << 0x8;
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0, reg_val);

	reg_val = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg0);
	reg_val &= ~(0x3 << 0x1);
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0, reg_val);
	reg_val |= dp_param.dp_phy_xlane << 0x1;
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0, reg_val);

	reg_val = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg0 + 0x24);
	reg_val |= (0x1 << 27);
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0 + 0x24, reg_val);

	reg_val = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg0 + 0x24);
	reg_val &= ~(0xf << 28);
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0 + 0x24, reg_val);
	reg_val |= (0xa << 28);
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0 + 0x24, reg_val);

	/* set vswing
	* 0 or 1, 1; 2, 3/4; 3, 1/2; others, 1/4
	*/
	for (i = 0; i < 4; i++) {
		ln_vswing[i] = vswing;
		ln_preemp[i] = preemp;
	}

	reg_val = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg7);
	reg_val &= ~(0xfff);
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg7, reg_val);
	reg_val |= ln_vswing[0] | (ln_vswing[1] << 3) | (ln_vswing[2] << 6) | (ln_vswing[3] << 9);
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg7, reg_val);
	msleep(100);

	// set pre-emphasiss
	reg_val = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg2);
	reg_val |= 0xf;
	reg_val &= ~(0xff << 8);
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg2, reg_val);
	reg_val |= (ln_preemp[0] << 8) | (ln_preemp[1] << 10) | (ln_preemp[2] << 12) | (ln_preemp[3] << 14);
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg2, reg_val);
	msleep(100);

	CLK_HS = 2700;
	if (dp_param.dp_phy_rate == DP_PHY_1P62G)
		CLK_HS = 3240;
	pixelclk_div_N = CLK_HS * 1000 / dp_param.dp_pixclk;
	pixelclk_div_F = CLK_HS * 1000 * 65536 / dp_param.dp_pixclk - pixelclk_div_N * 65536;

	/* DP/EDP phy reg 12 TODO*/
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg12, 0);
	reg_val = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg12);
	reg_val |= (0x1 << 31) | (pixelclk_div_F << 8) | pixelclk_div_N;
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg12, reg_val);

	/* enable phy tx */
	reg_val = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg0);
	reg_val &= ~0x1;
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0, reg_val);

	reg_val = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg0);
	reg_val |= 0x1;
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0, reg_val);

	/* wait phy ready */
	timeout = jiffies + msecs_to_jiffies(1000);
	do {
		reg_val = dc_readl(adev, gdc_reg->dp_reg[intf].phy_monitor0);
		if ((reg_val & 0x3) == 0x3)
			break;
		if (time_after(jiffies, timeout)) {
			DRM_ERROR("dp[%d] phy_monitor0 timeout, val=0x%x\n",
				  intf, reg_val);
			break;
		}
		usleep_range(100, 200);
	} while (1);
}

static void ls9a_dp_link_init(struct loonggpu_device *adev, dp_feature_t dp_param, int intf, struct dc_timing_info *timing)
{
	uint32_t phy_rate, phy_lane;
	uint32_t link_rate, link_lane;
	uint32_t mode_bpc, tu_video_size;
	uint64_t dp_link_clk;
	uint64_t tmp;
	uint64_t dp_color_ratio = 1;
	uint32_t dp_color_depth = 8; //需改为外部传入
	uint32_t valid_rate = 0;
	uint32_t value;
	uint32_t i;

	/* color mode */
	if(dp_color_depth == 8)
		mode_bpc = 0;
	else if(dp_color_depth == 10)
		mode_bpc = 1;
	else {
		DRM_DEBUG_DRIVER("dp color depth error! use default value\n");
		mode_bpc = 0;
	}

	/* dp MSA */
	value = dc_readl(adev,  gdc_reg->dp_reg[intf].sdp_cfg0);
	value |= 0x1 << 30;
	dc_writel(adev,  gdc_reg->dp_reg[intf].sdp_cfg0, value);

	value = ((timing->vtotal - timing->vsync_start) << 16) | (timing->htotal - timing->hsync_start);
	dc_writel(adev,  gdc_reg->dp_reg[intf].sdp_cfg62, value);

	value = (timing->vtotal << 16) | timing->htotal;
	dc_writel(adev,  gdc_reg->dp_reg[intf].sdp_cfg63, value);

	value = (timing->vdisplay << 16) | timing->hdisplay;
	dc_writel(adev,  gdc_reg->dp_reg[intf].sdp_cfg64, value);

	value = ((timing->vsync_end - timing->vsync_start) << 16) | (timing->hsync_end - timing->hsync_start);
	dc_writel(adev,  gdc_reg->dp_reg[intf].sdp_cfg65, value);

	dc_writel(adev,  gdc_reg->dp_reg[intf].sdp_cfg61, 0x1);

	value = (timing->vsync_end << 16) | timing->hsync_end;
	dc_writel(adev,  gdc_reg->dp_reg[intf].sdp_cfg0 + 0x108, value);
	value = (timing->vsync_start << 16) | timing->hsync_start;
	dc_writel(adev,  gdc_reg->dp_reg[intf].sdp_cfg0 + 0x11c, value);

	/* link rate & lane count */
	value = dc_readl(adev,  gdc_reg->dp_reg[intf].link_cfg1);
	value &= ~(0xf << 16);
	dc_writel(adev,  gdc_reg->dp_reg[intf].link_cfg1, value);
	value |= (mode_bpc << 16);
	dc_writel(adev,  gdc_reg->dp_reg[intf].link_cfg1, value);

	value = dc_readl(adev,  gdc_reg->dp_reg[intf].link_cfg1);
	value &= ~0xffff;
	value |= dp_param.dp_link_rate;
	value |= (dp_param.dp_link_xlane << 8);
	dc_writel(adev,  gdc_reg->dp_reg[intf].link_cfg1, value);

	phy_rate = (dc_readl(adev,  gdc_reg->dp_reg[intf].phy_cfg0) >> 8) & 0x7;
	phy_lane = (dc_readl(adev,  gdc_reg->dp_reg[intf].phy_cfg0) >> 1) & 0x3;
	link_rate = (dc_readl(adev,  gdc_reg->dp_reg[intf].link_cfg1) & 0xff);
	link_lane = (dc_readl(adev,  gdc_reg->dp_reg[intf].link_cfg1) >> 8) & 0xff;

	for (i = 0; i < sizeof(rate_table) / sizeof(rate_table[0]); i++) {
		if ((link_rate == rate_table[i].link_rate) && (phy_rate == rate_table[i].phy_rate)) {
			dp_link_clk = rate_table[i].clk;
			valid_rate = 1;
			break;
		}
	}

	if (!valid_rate) {
		DRM_ERROR("DP rate configuration error! Link rate: 0x%02x, PHY rate: 0x%x\n", link_rate, phy_rate);
	}

	if (!(((link_lane == DP_LINK_X1) && (phy_lane == DP_PHY_X1)) ||
	     ((link_lane == DP_LINK_X2) && (phy_lane == DP_PHY_X2)) ||
	     ((link_lane == DP_LINK_X4) && (phy_lane == DP_PHY_X4)))) {
		DRM_ERROR("DP lane configuration mismatch! Link lanes: %d, PHY lanes: 0x%x\n", link_lane, phy_lane);
	}

	/* set tu size */ //这里是按照8bit色深计算的
	tmp  = ((dp_param.dp_pixclk * 3 * dp_color_ratio * 64) / (dp_link_clk * link_lane)) / 1000;
	tu_video_size = (unsigned int)(tmp & 0xffffffff) + 1;
	if (tu_video_size > MAX_TU)
		tu_video_size = MAX_TU;

	value = dc_readl(adev,  gdc_reg->dp_reg[intf].link_cfg4);
	value &= ~(0xff);
	value |= tu_video_size;
	dc_writel(adev,  gdc_reg->dp_reg[intf].link_cfg4, value);
}

static void ls9a_dp_soft_training(struct loonggpu_device *adev, dp_feature_t dp_param, int intf)
{
	unsigned int ln_vswing[4] = {0}, ln_vswing_max[4] = {0};
	unsigned int ln_preemp[4] = {0}, ln_preemp_max[4] = {0};
	unsigned int ln_set[4] = {0};
	unsigned int dp_detect_flag = 0;
	unsigned int i, tmp, tps3_flag = 0, dpcd_rev_major, dpcd_rev_minor;
	uint32_t ln_cr_done[4] = {0}, cr_done = 0; // CR DONE
	uint32_t ln_eq_done[4] = {0}; // CHANNEL EQ DONE
	uint32_t ln_sl_done[4] = {0}; // SYMBOL LOCKED
	uint32_t interlane_align_done, lt_done, lt_cnt;
	aux_msg_t aux_data;
	uint32_t value, max_retries = 8; //4
	bool training_success = false;
	unsigned int active_xlane;
	unsigned int interval_value, cr_interval, eq_interval;//ms

	/* dp soft reset */
	value = dc_readl(adev, gdc_reg->dp_reg[intf].link_cfg0);
	value |= (1 << 31);
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, value);

	/* TPS1*/
	/* enable dp TPS1 */
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, 0x3850038b);

	aux_data.addr = DPCD_REV_ADDR;
	aux_data.size = 5;
	aux_data.data_low = 0;
	aux_data.data_high = 0;
	dp_detect_flag = ls9a_aux_config(adev, READ, aux_data, intf);
	if (dp_detect_flag == 1) {
		DRM_ERROR("No DP:%d device detected!\n", intf);
		goto training_end;
	}
	tmp = dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor1) & 0xFF;
	dpcd_rev_major = (tmp >> 4) & 0x0F;
	dpcd_rev_minor = tmp & 0x0F;

	aux_data.addr = DPCD_MAX_LANE_COUNT_ADDR;
	aux_data.size = 0;
	aux_data.data_low = 0;
	aux_data.data_high = 0;
	ls9a_aux_config(adev, READ, aux_data, intf);
	tps3_flag = (dc_readl(adev,  gdc_reg->dp_reg[intf].aux_monitor1) >> 6) & 0x1;

	aux_data.addr = DPCD_SET_POWER_ADDR;
	aux_data.size = 0;
	aux_data.data_low = DP_POWER_STATE_D3;
	aux_data.data_high = 0;
	ls9a_aux_config(adev, WRITE, aux_data, intf);
	udelay(10000);
	aux_data.data_low = DP_POWER_STATE_D0;
	ls9a_aux_config(adev, WRITE, aux_data, intf);
	udelay(10000);
	aux_data.data_low = DP_POWER_STATE_D0;
	ls9a_aux_config(adev, WRITE, aux_data, intf);
	udelay(10000);

	aux_data.addr = DPCD_LINK_BW_SET_ADDR;
	aux_data.size = 0;
	aux_data.data_low = dp_param.dp_link_rate;
	aux_data.data_high = 0;
	ls9a_aux_config(adev, WRITE, aux_data, intf);

	aux_data.addr = DPCD_LANE_COUNT_SET_ADDR;
	aux_data.data_low = 0;
	aux_data.data_high = 0;
	ls9a_aux_config(adev, READ, aux_data, intf);

	aux_data.addr = DPCD_LANE_COUNT_SET_ADDR;
	aux_data.data_low = 0x80 | dp_param.dp_link_xlane;
	aux_data.data_high = 0;
	ls9a_aux_config(adev, WRITE, aux_data, intf);

	aux_data.addr = DPCD_MSTM_CTRL_ADDR;
	aux_data.data_low = 0;
	ls9a_aux_config(adev, WRITE, aux_data, intf);

	aux_data.addr = DPCD_DOWNSPREAD_CTRL_ADDR;
	aux_data.data_low = 0x10;
	ls9a_aux_config(adev, WRITE, aux_data, intf);

	aux_data.addr = DPCD_LANE_COUNT_SET_ADDR;
	aux_data.data_low = 0x80 | dp_param.dp_link_xlane;
	ls9a_aux_config(adev, WRITE, aux_data, intf);

	aux_data.addr = DPCD_TRAINING_AUX_RD_INTERVAL_ADDR;
	aux_data.size = 0;
	aux_data.data_low = 0;
	aux_data.data_high = 0;
	ls9a_aux_config(adev, READ, aux_data, intf);
	interval_value = dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor1) & 0x7F;

	if (interval_value == 0x00) {
		cr_interval = 2;
		eq_interval = 8;
	} else if (interval_value <= 0x04) {
		cr_interval = eq_interval = interval_value * 4;
	} else {
		cr_interval = 10;
		eq_interval = 20;
	}

	if (dpcd_rev_major < 1 || (dpcd_rev_major == 1 && dpcd_rev_minor < 2)) { //dp < 1.2 , add time interval
		cr_interval = 10;
		eq_interval = 40;
	}

	aux_data.addr = DPCD_TRAINING_PATTERN_SET_ADDR;
	aux_data.data_low = TRAINING_PATTERN_1;
	ls9a_aux_config(adev, WRITE, aux_data, intf);

	active_xlane = dp_param.dp_link_xlane;
	mdelay(10);

	lt_cnt = 0;

	while (lt_cnt < max_retries) {
		udelay(cr_interval * 1000);

		/* training lane req from sink */
		aux_data.addr = DPCD_ADJUST_REQUEST_LANE0_1_ADDR;
		aux_data.size = 1;
		aux_data.data_low = 0;
		aux_data.data_high = 0;
		ls9a_aux_config(adev, READ, aux_data, intf);

		tmp = dc_readl(adev,  gdc_reg->dp_reg[intf].aux_monitor1);

		for (i = 0; i < 4; i++) {
			ln_preemp[i] = (tmp >> (2 + i * 4)) & 0x3;
			ln_vswing[i] = (tmp >> (i * 4)) & 0x3;
			ln_vswing_max[i] = (ln_vswing[i] == 0x3) ? 1 : 0;
			ln_preemp_max[i] = (ln_preemp[i] == 0x3) ? 1 : 0;
			ln_set[i] = ln_vswing[i] | (ln_vswing_max[i] << 2) |
				(ln_preemp[i] << 3) | (ln_preemp_max[i] << 5);
		}

		value = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg2);
		value |= 0xf;
		dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg2, value);
		msleep(100);
		
		value = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg7);
		value &= ~0xfff;
		dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg7, value);
		value |= (4 - ln_vswing[0]) | ((4 - ln_vswing[1]) << 3) |
			 ((4 - ln_vswing[2]) << 6) | ((4 - ln_vswing[3]) << 9);
		dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg7, value);
		msleep(100);

		value = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg2);
		value &= ~(0xff << 8);
		dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg2, value);
		value |= (ln_preemp[0] << 8) | (ln_preemp[1] << 10) |
			 (ln_preemp[2] << 12) | (ln_preemp[3] << 14);
		dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg2, value);
		msleep(100);

		for (i = 0; i < active_xlane; i++) {
			aux_data.addr = (DPCD_TRAINING_LANE0_SET_ADDR + i);
			aux_data.size = 0;
			aux_data.data_low = ln_set[i];
			aux_data.data_high = 0;
			ls9a_aux_config(adev, WRITE, aux_data,  intf);
		}
		udelay(cr_interval * 1000);

		aux_data.addr = DPCD_LANE0_1_STATUS_ADDR;
		aux_data.size = 5;
		aux_data.data_low = 0;
		aux_data.data_high = 0;
		ls9a_aux_config(adev, READ, aux_data, intf);
		tmp = dc_readl(adev,  gdc_reg->dp_reg[intf].aux_monitor1);

		for (i = 0; i < active_xlane; i++)
			ln_cr_done[i] = (tmp >> (i * 4)) & 0x1;

		cr_done = (active_xlane == 4) ? (ln_cr_done[0] & ln_cr_done[1] & ln_cr_done[2] & ln_cr_done[3]) :
				(active_xlane == 2) ? (ln_cr_done[0] & ln_cr_done[1]) :
				(active_xlane == 1) ? (ln_cr_done[0]) : 0;
		if (cr_done) {
			DRM_DEBUG("DP %d:TPS1 Clock Recovery successful\n",
				  intf);
			DRM_DEBUG("after %d attempts. cr_interval: %dms\n",
				  lt_cnt + 1, cr_interval);
			break;
		}

		lt_cnt++;
		cr_interval = cr_interval + 5;
	}

	if (!cr_done) {
		DRM_WARN("Interface %d: TPS1 Clock Recovery failed after %d attempts\n", intf, max_retries);
		goto skip_tps23;
	}

	if (tps3_flag == 1) {
		aux_data.addr = DPCD_TRAINING_PATTERN_SET_ADDR;
		aux_data.size = 0;
		aux_data.data_low = TRAINING_PATTERN_3;
		aux_data.data_high = 0;
		ls9a_aux_config(adev, WRITE, aux_data, intf);
		dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, 0x3810038b | (0x4 << 22)); // enable TPS3
	} else {
		aux_data.addr = DPCD_TRAINING_PATTERN_SET_ADDR;
		aux_data.size = 0;
		aux_data.data_low = TRAINING_PATTERN_2;
		aux_data.data_high = 0;
		ls9a_aux_config(adev, WRITE, aux_data, intf);
		dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, 0x3810038b | (0x2 << 22)); // enable TPS2
	}

	lt_cnt = 0;
	lt_done = 0;

	while (lt_cnt < max_retries) {
		DRM_DEBUG_DRIVER("wait a second!\n");
		udelay(eq_interval * 1000);

		/* training lane req from sink */
		aux_data.addr = DPCD_ADJUST_REQUEST_LANE0_1_ADDR;
		aux_data.size = 5;
		aux_data.data_low = 0;
		aux_data.data_high = 0;
		ls9a_aux_config(adev, READ, aux_data, intf);
		tmp = dc_readl(adev,  gdc_reg->dp_reg[intf].aux_monitor1);

		for (i = 0; i < active_xlane; i++) {
			ln_vswing[i] = (tmp >> (i * 4)) & 0x3;
			ln_preemp[i] = (tmp >> (2 + i * 4)) & 0x3;
			ln_vswing_max[i] = (ln_vswing[i] == 0x3) ? 1 : 0;
			ln_preemp_max[i] = (ln_preemp[i] == 0x3) ? 1 : 0;
			ln_set[i] = ln_vswing[i] | (ln_vswing_max[i] << 2) |
				(ln_preemp[i] << 3) | (ln_preemp_max[i] << 5);
		}

		/* set dp phy pe and vs */
		value = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg2);
		value |= 0xf;
		dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg2, value);

		value = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg7);
		value &= ~(0xfff);
		dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg7, value);
		value |= (4 - ln_vswing[0]) | ((4 - ln_vswing[1]) << 3) |
			 ((4 - ln_vswing[2]) << 6) | ((4 - ln_vswing[3]) << 9);
		dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg7, value);
		msleep(100);

		value = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg2);
		value &= ~(0xff << 8);
		dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg2, value);
		value |= (ln_preemp[0] << 8) | (ln_preemp[1] << 10) |
			 (ln_preemp[2] << 12) | (ln_preemp[3] << 14);
		dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg2, value);
		msleep(100);

		for (i = 0; i < active_xlane; i++) {
			aux_data.addr = (DPCD_TRAINING_LANE0_SET_ADDR + i);
			aux_data.size = 0;
			aux_data.data_low = ln_set[i];
			aux_data.data_high = 0;
			ls9a_aux_config(adev, WRITE, aux_data, intf);
		}

		DRM_DEBUG_DRIVER("wait a second!\n");
		udelay(eq_interval * 1000);

		aux_data.addr = DPCD_LANE0_1_STATUS_ADDR;
		aux_data.size = 5;
		aux_data.data_low = 0;
		aux_data.data_high = 0;
		ls9a_aux_config(adev, READ, aux_data, intf);
		tmp = dc_readl(adev,  gdc_reg->dp_reg[intf].aux_monitor1);

		for (i = 0; i < active_xlane; i++) {
			ln_cr_done[i] = (tmp >> (i * 4)) & 0x1;         /* bit0: CR_DONE */
			ln_eq_done[i] = (tmp >> (1 + i * 4)) & 0x1;     /* bit1: CHANNEL_EQ_DONE */
			ln_sl_done[i] = (tmp >> (2 + i * 4)) & 0x1;     /* bit2: SYMBOL_LOCKED */
		}

		interlane_align_done = (tmp >> 16) & 0x1;

		if (dp_param.dp_link_xlane == 4) {
			lt_done = ln_cr_done[0] & ln_cr_done[1] & ln_cr_done[2] & ln_cr_done[3] &
			          ln_eq_done[0] & ln_eq_done[1] & ln_eq_done[2] & ln_eq_done[3] &
			          ln_sl_done[0] & ln_sl_done[1] & ln_sl_done[2] & ln_sl_done[3] &
			          interlane_align_done;
		} else if (dp_param.dp_link_xlane == 2) {
			lt_done = ln_cr_done[0] & ln_cr_done[1] &
			          ln_eq_done[0] & ln_eq_done[1] &
			          ln_sl_done[0] & ln_sl_done[1] &
			          interlane_align_done;
		} else {  /* 1 lane */
			lt_done = ln_cr_done[0] & ln_eq_done[0] & ln_sl_done[0] & interlane_align_done;
		}

		if (lt_done) {
			training_success = true;
			break;
		}

		lt_cnt++;
		eq_interval = eq_interval + 5;
	}

skip_tps23:
	aux_data.addr = DPCD_TRAINING_PATTERN_SET_ADDR;
	aux_data.size = 0;
	aux_data.data_low = TRAINING_PATTERN_DISABLED;
	aux_data.data_high = 0;
	ls9a_aux_config(adev, WRITE, aux_data, intf);
	udelay(cr_interval * 1000);

training_end:
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, 0x3830038b);
	value = dc_readl(adev, gdc_reg->dp_reg[intf].link_cfg0);
	value |= (0x3 << 20);
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, value);

	if (training_success) {
		DRM_DEBUG("Interface %d: DP soft training completed successfully\n", intf);
	} else if (dp_detect_flag == 1) {
		DRM_ERROR("Interface %d: DP training aborted - no device detected\n", intf);
	} else {
		DRM_ERROR("Interface %d: DP training failed.\n", intf);
	}

	return;
}

bool ls9a_dp_read_edid(struct loonggpu_device *adev, struct loonggpu_dc_crtc *dc_crtc, unsigned char *edid)
{
	struct loonggpu_dc_video *dc_video = &dc_crtc->dc_video;
	int dp_num = dc_video->dp_num;
	unsigned int *internal_edid;
	unsigned int block_num;
	unsigned int value;
	dp_feature_t dp_param = {0};
	int i;
	int ret;
	aux_msg_t aux_data;

	internal_edid = kzalloc(EDID_LENGTH * 2, GFP_KERNEL);
	if (!internal_edid)
		return false;

	if (dc_video->cfgok == 0) {
		ret = dp_check_bandwidth(148500, &dp_param);
		if (!ret)
			return 0;

		ls9a_dp_phy_init(adev, dp_param, dp_num, 0, 0);
	}

	value = dc_readl(adev, gdc_reg->dp_reg[dp_num].aux_channel8);
	value |= 0x3 | (0xff << 16);
	dc_writel(adev, gdc_reg->dp_reg[dp_num].aux_channel8, value);

	/* block0 */
	for (i = 0; i < 16; i++) {
		aux_data.addr = 0x50;
		aux_data.size = 7;
		aux_data.data_low = 0;
		aux_data.data_high = 0;
		ret = ls9a_aux_config(adev, READ, aux_data, dp_num);
		internal_edid[i + i] = dc_readl(adev, gdc_reg->dp_reg[dp_num].aux_monitor1);
		internal_edid[i + i + 1] = dc_readl(adev, gdc_reg->dp_reg[dp_num].aux_monitor2);
	}

	/* block1 */
	block_num = (internal_edid[31] >> 16) & 0xff;

	if (block_num) {
		for (i = 16; i < 32; i++) {
			aux_data.addr = 0x50;
			aux_data.size = 7;
			aux_data.data_low = 0;
			aux_data.data_high = 0;
			ret = ls9a_aux_config(adev, READ, aux_data, dp_num);
			internal_edid[i + i] = dc_readl(adev, gdc_reg->dp_reg[dp_num].aux_monitor1);
			internal_edid[i + i + 1] = dc_readl(adev, gdc_reg->dp_reg[dp_num].aux_monitor2);
		}
	}

	value = dc_readl(adev, gdc_reg->dp_reg[dp_num].aux_channel8);
	value &= ~0x1;
	dc_writel(adev, gdc_reg->dp_reg[dp_num].aux_channel8, value);

	memcpy(edid, internal_edid, EDID_LENGTH * 2);
	kfree(internal_edid);

	return true;
}

int ls9a1000_dp_init(struct loonggpu_dc_crtc *dc_crtc, int intf)
{
	struct loonggpu_device *adev = dc_crtc->dc->adev;
	int dp_num = dc_crtc->dc_video.dp_num;
	int reg_val;

	dc_writel(adev, gdc_reg->dp_reg[dp_num].aux_monitor0, 0);

	reg_val = dc_readl(adev, gdc_reg->dp_reg[dp_num].aux_channel0);
	reg_val |= (0x1 << 1);
	reg_val |= (0x1 << 4);
	dc_writel(adev, gdc_reg->dp_reg[dp_num].aux_channel0, reg_val);

	dc_writel(adev, gdc_reg->dp_reg[dp_num].aux_channel8, 0x1a0ffff8);

	reg_val = dc_readl(adev, gdc_reg->dp_reg[dp_num].link_cfg0);
	reg_val |= 0x1;
	dc_writel(adev, gdc_reg->dp_reg[dp_num].link_cfg0, reg_val);

	msleep(50);

	return 0;
}

void ls9a1000_dp_pll_set(struct loonggpu_dc_crtc *dc_crtc, int intf, struct dc_timing_info *timing)
{
	struct loonggpu_device *adev = dc_crtc->dc->adev;
	dp_feature_t recheck_dp_param;
	dp_feature_t dp_param = {0};
	bool ret;
	int dp_num = dc_crtc->dc_video.dp_num;
	int reg_val;

	reg_val = dc_readl(adev, gdc_reg->dp_reg[dp_num].hpd_enable);
	dc_writel(adev, gdc_reg->dp_reg[dp_num].hpd_enable, 0);

	ret = dp_check_bandwidth(timing->clock, &dp_param);
	if (!ret) {
		dc_writel(adev, gdc_reg->dp_reg[dp_num].hpd_enable, reg_val);
		return;
	}
	ls9a_dp_phy_init(adev, dp_param, dp_num, 0, 0);

	ls9a_dp_recheck_bandwidth(adev, timing, &recheck_dp_param, dp_num);

	ls9a_dp_phy_init(adev, recheck_dp_param, dp_num, 0, 0);
	ls9a_dp_link_init(adev, recheck_dp_param, dp_num, timing);
	ls9a_dp_soft_training(adev, recheck_dp_param, dp_num);

	dc_writel(adev, gdc_reg->dp_reg[dp_num].hpd_enable, reg_val);

	return;
}

bool ls9a1000_dp_enable(struct loonggpu_dc_crtc *dc_crtc, int intf, bool enable)
{
	struct loonggpu_device *adev = dc_crtc->dc->adev;
	aux_msg_t aux_data;
	u32 ret;
	int link = dc_crtc->resource->base.link;
	int dp_num = dc_crtc->dc_video.dp_num;
	int video_num = dc_crtc->dc_video.video_num;

	dc_crtc->intf[intf].enabled = true;
	return true;

	if (!dc_crtc->intf[intf].connected)
		return false;

	if ((enable && dc_crtc->intf[intf].enabled) ||
		(!enable && !dc_crtc->intf[intf].enabled))
		return true;

	DRM_INFO("%s link:%d intf:%d video_num:%d dp_num:%d enable:%d\n",
		__func__, link, intf, video_num, dp_num, enable);

	if (enable) {
		aux_data.addr = DPCD_SET_POWER_ADDR;
		aux_data.size = 0;
		aux_data.data_low = DP_POWER_STATE_D0;
		aux_data.data_high = 0;
		ret = ls9a_aux_config(adev,  WRITE, aux_data, dp_num);
		if (!ret)
			dc_crtc->intf[intf].enabled = true;
	} else {
		aux_data.addr = DPCD_SET_POWER_ADDR;
		aux_data.size = 0;
		aux_data.data_low = DP_POWER_STATE_D3;
		aux_data.data_high = 0;
		ret = ls9a_aux_config(adev,  WRITE, aux_data, dp_num);
		if (!ret)
			dc_crtc->intf[intf].enabled = false;
	}

	DRM_INFO("9A DP enable:%d ret:%d\n", enable, ret);
	return true;
}

void ls9a1000_dp_suspend(struct loonggpu_dc_crtc *dc_crtc, int intf)
{
	return;
}

int ls9a1000_dp_resume(struct loonggpu_dc_crtc *dc_crtc, int intf)
{
	return 0;
}

int ls9a1000_dp_audio_init(struct loonggpu_dc_crtc *dc_crtc, int intf)
{
	return 0;
}

int ls9a1000_dp_noaudio_init(struct loonggpu_dc_crtc *dc_crtc, int intf)
{
	return 0;
}
