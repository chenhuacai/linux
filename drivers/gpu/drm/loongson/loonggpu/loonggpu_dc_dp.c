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

#define DP_IN_MASK    BIT(24)
#define DP_OUT_MASK   BIT(25)
#define EDP_IN_MASK   BIT(28)
#define EDP_OUT_MASK  BIT(29)
#define DP_LOCK       BIT(26)
#define DP_OWNER      BIT(27)
#define EDP_LOCK      BIT(30)
#define EDP_OWNER     BIT(31)

static const dp_bandwidth_entry_t bw_table[] = {
	{ 162000 * 1 / 3, DP_PHY_1P62G, DP_PHY_X1 , DP_LINK_1P62G, DP_LINK_X1},
	{ 162000 * 2 / 3, DP_PHY_1P62G, DP_PHY_X2 , DP_LINK_1P62G, DP_LINK_X2},
	{ 162000 * 4 / 3, DP_PHY_1P62G, DP_PHY_X4 , DP_LINK_1P62G, DP_LINK_X4},
	{ 270000 * 1 / 3, DP_PHY_2P7G,  DP_PHY_X1 , DP_LINK_2P7G,  DP_LINK_X1},
	{ 270000 * 2 / 3, DP_PHY_2P7G,  DP_PHY_X2 , DP_LINK_2P7G,  DP_LINK_X2},
	{ 270000 * 4 / 3, DP_PHY_2P7G,  DP_PHY_X4 , DP_LINK_2P7G,  DP_LINK_X4},
};

static const dp_rate_info_t  rate_table[] = {
	{ DP_LINK_1P62G, DP_PHY_1P62G, 162 },
	{ DP_LINK_2P16G, DP_PHY_2P16G, 216 },
	{ DP_LINK_2P43G, DP_PHY_2P43G, 243 },
	{ DP_LINK_2P7G,  DP_PHY_2P7G,  270 },
	{ DP_LINK_3P24G, DP_PHY_3P24G, 324 },
	{ DP_LINK_4P32G, DP_PHY_4P32G, 432 },
};

/**
 * For some board designs that do not support multiple display interfaces over a single display link
 * and have strict power-saving requirements, the DP controller and DP PHY are powered down.
 **/
static void ls2k3000_close_dp(struct loonggpu_device *adev, int intf)
{
	u32 phy_cfg0, link_cfg0;

	phy_cfg0 = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg0);
	phy_cfg0 &= ~0x1;
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0, phy_cfg0);

	link_cfg0 = dc_readl(adev, gdc_reg->dp_reg[intf].link_cfg0);
	link_cfg0 &= ~0x1;
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, link_cfg0);
}


static bool is_use_dp(struct loonggpu_device *adev, int intf, struct connector_resource *connector_res)
{
	struct encoder_resource *encoder_res;

	encoder_res = adev->dc->link_info[intf].encoder->resource;
	if (connector_res && intf && encoder_res) {
		if ((encoder_res->chip != ENCODER_CHIP_ID_INTERNAL_DP)  \
			&& !connector_res->multi_interface) {
			adev->dp_status[1] = 0x8;
			ls2k3000_close_dp(adev, intf);
			return false;
		}
	}

	return true;
}

static bool is_csw_special_display(const struct edid *edid)
{
	char manufacturer[4];
	u16 product_code;

	if (!edid)
		return false;

	get_edid_manufacturer(edid, manufacturer);
	if (strncmp(manufacturer, "CSW", 3) != 0)
		return false;

	product_code = EDID_PRODUCT_ID(edid);

	return product_code == 5209;
}

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
	if ((lane == 2) && (idx == 2 || idx == 5))
		return 1;
	if ((lane == 1) && (idx == 1 || idx == 2 || idx == 4 || idx == 5))
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

	DRM_INFO("NOTE: This pclk is not within the normal range!!\n");
	return false;
}

unsigned int aux_config(struct loonggpu_device *adev, unsigned int rd_wr, aux_msg_t aux_msg, int intf)
{
	unsigned int dp_detect_flag = 0;
	unsigned int val32;
	int retry_budget = 20;
	unsigned char i;
	u32 value, err_reason;
	const char *op_str = (rd_wr == READ) ? "READ" : "WRITE";

	if (in_interrupt()) {
		pr_warn("DP_AUX can not be used in interrupt context.\n");
		return -1;
	}

	DRM_DEBUG_DRIVER("AUX %s: addr=0x%05x, size=%d.\n", op_str, aux_msg.addr, aux_msg.size);
	value = dc_readl(adev, gdc_reg->dp_reg[intf].aux_channel0);
	value &= ~(0x1 << 3);
	if (rd_wr == WRITE) {
		value |= (0x1 << 3);
	}
	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel0, value);

	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel1, aux_msg.addr & 0xfffff);//aux request addr
	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel2, (aux_msg.size - 1) & 0xfffff);//aux write request data size

	if (rd_wr == WRITE) {
		for (i = 0; i < aux_msg.size; i++) {//aux write request data    aux_channel3～7
			if (i < 8) {
				writeb((((aux_msg.data_low) >> (8*i)) & 0xff), adev->loongson_dc_rmmio + gdc_reg->dp_reg[intf].aux_channel3 + i);
				DRM_DEBUG_DRIVER("aux data write: [aux base: 0x%x]: 0x%x\n", gdc_reg->dp_reg[intf].aux_channel3 + i,  \
								readb(adev->loongson_dc_rmmio + gdc_reg->dp_reg[intf].aux_channel3 + i));
			} else
				writeb(((aux_msg.data_high >> (8 * (i - 8))) & 0xff), adev->loongson_dc_rmmio + gdc_reg->dp_reg[intf].aux_channel3 + i);
		}
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

			if (rd_wr == READ) {
				for (i = 0; i < aux_msg.size; i++) {
					DRM_DEBUG_DRIVER("aux data read: [aux base: 0x%x]: 0x%x\n", gdc_reg->dp_reg[intf].aux_monitor1 + i, \
										readb(adev->loongson_dc_rmmio + gdc_reg->dp_reg[intf].aux_monitor1 + i));
				}
			}
			return 0;
		}

		if (val32 & 0x1) {
			DRM_INFO("AUX %s: dp transaction fail --> addr=0x%05x, size=%d.\n", op_str, aux_msg.addr, aux_msg.size);
			err_reason = dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor5);

			if ((err_reason & (1 << 4)) || (err_reason & (1 << 7))) { // Bit 4: sink_deffer
				// Bit 4: sink_deffer, Bit 7: i2c_deffer
				const char *defer_type = (err_reason & (1 << 4)) ? "Sink" : "I2C";
				DRM_WARN("AUX %s: %s DEFER (device busy), %d retries left\n", op_str, defer_type, retry_budget);
				retry_budget--;
				if (retry_budget > 0) {
					value = dc_readl(adev, gdc_reg->dp_reg[intf].aux_channel0);
					value |= (0x1 << 2);
					dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel0, value);
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
			}
			else if (err_reason & (1 << 1)) { // Bit 1: rx_len_out_bound
				DRM_ERROR("AUX %s ERROR: received data length error!\n", op_str);
				goto fatal_error;
			}
			else if (err_reason & (1 << 2)) { // Bit 2: reply_command_invalid
				DRM_ERROR("AUX %s ERROR: Invalid reply command!\n", op_str);
				goto fatal_error;
			}
			else if (err_reason & (1 << 3)) { // Bit 3: sink_nack
				DRM_ERROR("AUX %s ERROR: DP Sink device responds with NACK (address or command not accepted)!\n", op_str);
				goto fatal_error;
			}
			else if (err_reason & (1 << 5)) { // Bit 5: i2c mode and reply command invalid
				DRM_ERROR("AUX %s ERROR: i2c mode and reply command invalid!\n", op_str);
				goto fatal_error;
			}
			else if (err_reason & (1 << 6)) { // Bit 6: i2c mode and sink reply nack
				DRM_ERROR("AUX %s ERROR: i2c mode and sink reply nack!\n", op_str);
				goto fatal_error;
			}
			else {
				DRM_ERROR("AUX %s UNKNOW ERROR, err_reason: 0x%08x\n", op_str, err_reason);
				goto fatal_error;
			}
		}

		retry_budget--;

	} while (retry_budget > 0);

	DRM_ERROR("AUX %s TIMEOUT, reg status:\n", op_str);
	DRM_ERROR("  MON0: 0x%08x, MON5: 0x%08x, MON6: 0x%08x, MON7: 0x%08x\n",
				dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor0),
				dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor5),
				dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor6),
				dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor7));

	dp_detect_flag = 1;
	return dp_detect_flag;

fatal_error:
	DRM_ERROR("AUX %s ERROR, reg status:\n", op_str);
	DRM_ERROR("  MON0: 0x%08x, MON5: 0x%08x, MON6: 0x%08x, MON7: 0x%08x\n",
				val32,
				dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor5),
				dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor6),
				dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor7));

	return -1;
}

#define MAX_TU_LAPTOP 52
static void dp_recheck_bandwidth(struct loonggpu_device *adev, struct dc_timing_info *timing, dp_feature_t *dp_param, int intf)
{
	struct loonggpu_connector *connector = adev->mode_info.connectors[intf];
	unsigned int i, aux_rate = 0, aux_lane = 0, tu_size = 0;
	unsigned int link_speed = 0, allow_fix = 0;
	unsigned int check_flag = 0;
	unsigned int max_tu_val = MAX_TU_LAPTOP;
	unsigned int vback_porch = 0;
	aux_msg_t aux_data;
	int max_rate;
	int max_lane;
	int ret = 0;
	int range_flag = 0;
	bool last_index = 0;

	aux_data.addr = DPCD_MAX_LINK_RATE_ADDR;
	aux_data.size = 1;
	aux_data.data_low = 0;
	aux_data.data_high = 0;
	ret = aux_config(adev, READ, aux_data, intf);
	if (ret != 0) {
		DRM_ERROR("Interface %d: Failed to read MAX_LINK_RATE! AUX returned: %d\n", intf, ret);
	}
	aux_rate = dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor1);

	aux_data.addr = DPCD_MAX_LANE_COUNT_ADDR;
	aux_data.size = 1;
	aux_data.data_low = 0;
	aux_data.data_high = 0;
	ret = aux_config(adev, READ, aux_data, intf);
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
		default:
			break;
		}

		last_index = range_flag ? (i == ARRAY_SIZE(bw_table) - 1)
							    : (max_rate * max_lane == bw_table[i].bw * 3);

		/* Calculate and check fixed vsync width */
		dp_param->fixed_vsync_width = (u32)((u64)65536 * 2 * 	\
							timing->clock / link_speed / timing->htotal);

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

	/* Workaround for bxc 2.8K (5.4G 2xlnae) resolution display issue */
	if (connector && connector->base.edid_blob_ptr &&
		is_csw_special_display(connector->base.edid_blob_ptr->data) &&  \
		timing->hdisplay == 2880 && timing->vdisplay == 1800) {
		dp_param->dp_phy_rate  = DP_PHY_2P7G;
		dp_param->dp_phy_xlane = DP_PHY_X4;
		dp_param->dp_link_rate = DP_LINK_2P7G;
		dp_param->dp_link_xlane = DP_LINK_X4;
	}

	DRM_INFO("Interface %d: dp_phy_rate: %d, dp_phy_xlane: %d, dp_link_rate: %d, dp_link_xlane: %d, dp_pixclk: %d\n",
				intf, dp_param->dp_phy_rate, dp_param->dp_phy_xlane,
				dp_param->dp_link_rate, dp_param->dp_link_xlane, dp_param->dp_pixclk);

	if (check_flag)
		DRM_INFO("Interface %d: This pclk is within the normal range.\n", intf);
	else
		DRM_INFO("Interface %d: NOTE: This pclk is not within the normal range!!\n", intf);

	return;
}

#define MAX_TU_PC 53
static void edp_converters_recheck_bandwidth(struct loonggpu_device *adev, u32 clock, dp_feature_t *dp_param, int intf)
{
	unsigned int i, aux_rate = 0, aux_lane = 0, tu_size = 0;
	unsigned int check_flag = 0;
	unsigned int max_tu_val = MAX_TU_PC;
	aux_msg_t aux_data;
	uint64_t tmp;
	int max_rate;
	int max_lane;
	int ret = 0;

	aux_data.addr = DPCD_MAX_LINK_RATE_ADDR;
	aux_data.size = 1;
	aux_data.data_low = 0;
	aux_data.data_high = 0;
	aux_config(adev, READ, aux_data, intf);
	if (ret != 0) {
		DRM_ERROR("Interface %d: Failed to read MAX_LINK_RATE! AUX returned: %d\n", intf, ret);
	}
	aux_rate = dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor1);

	aux_data.addr = DPCD_MAX_LANE_COUNT_ADDR;
	aux_data.size = 1;
	aux_data.data_low = 0;
	aux_data.data_high = 0;
	aux_config(adev, READ, aux_data, intf);
	if (ret != 0) {
		DRM_ERROR("Interface %d: Failed to read MAX_LINK_CONUT! AUX returned: %d\n", intf, ret);
	}
	aux_lane = dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor1);

	max_rate = get_max_rate(aux_rate);
	max_lane = get_max_lane(aux_lane);

	dp_param->dp_pixclk = clock;

	for (i = 0; i < sizeof(bw_table) / sizeof(bw_table[0]); i++) {
		if (bw_table[i].bw < clock)
			continue;

		if (rate_lane_unsupported(max_rate, max_lane, i))
			continue;

		dp_param->dp_phy_rate  = bw_table[i].phy_rate;
		dp_param->dp_phy_xlane = bw_table[i].phy_lane;
		dp_param->dp_link_rate = bw_table[i].link_rate;
		dp_param->dp_link_xlane = bw_table[i].link_lane;

		tmp = ((dp_param->dp_pixclk * 64) / bw_table[i].bw);
		tu_size = tmp + 1;

		if (tu_size <= max_tu_val) {
			check_flag = 1;
			break;
		}

		if (max_rate * max_lane == bw_table[i].bw * 3) {
			dp_param->dp_pixclk = (max_tu_val * bw_table[i].bw) / 64;
			check_flag = 1;
			break;
		}
	}

	DRM_INFO("Interface %d: dp_phy_rate: %d, dp_phy_xlane: %d, dp_link_rate: %d, dp_link_xlane: %d, dp_pixclk: %d\n",
				intf, dp_param->dp_phy_rate, dp_param->dp_phy_xlane,
				dp_param->dp_link_rate, dp_param->dp_link_xlane, dp_param->dp_pixclk);

	if (check_flag)
		DRM_INFO("Interface %d: This pclk is within the normal range.\n", intf);
	else
		DRM_INFO("Interface %d: NOTE: This pclk is not within the normal range!!\n", intf);

	return;
}

static void dp_phy_init(struct loonggpu_device *adev, struct dc_timing_info *timing, dp_feature_t dp_param, int intf, uint32_t vswing, uint32_t preemp)
{
	struct loonggpu_connector *connector = adev->mode_info.connectors[intf];
	uint64_t CLK_HS;
	uint64_t pixelclk_div_N;
	uint64_t pixelclk_div_F;
	uint32_t ln_vswing[4]= {0}, ln_preemp[4] = {0};
	uint32_t value;
	uint32_t i;

	/* enable phy vswing */
	value = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg0 + 0x2c);
	value |= (0xf << 6);
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0 + 0x2c, value);

	/* make sure that vswing and preemp settings take effect immediately */
	value = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg0);
	value |= (0x1 << 31);

	/* set phy rate & lane */
	value &= ~(0x7 << 0x8);
	value |= dp_param.dp_phy_rate << 0x8;
	value &= ~(0x3 << 0x1);
	value |= dp_param.dp_phy_xlane << 0x1;
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0, value);

	/* set vswing
	* 0 or 1, 1; 2, 3/4; 3, 1/2; others, 1/4
	*/
	for (i = 0; i < 4; i++) {
		ln_vswing[i] = vswing;
		ln_preemp[i] = preemp;
	}

	value = ln_vswing[0] | (ln_vswing[1] << 3) | (ln_vswing[2] << 6) | (ln_vswing[3] << 9);
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg7, value);
	udelay(100000);

	// set pre-emphasiss
	value = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg2);
	value |= 0xf;
	value &= ~(0xff << 8);
	value |= (ln_preemp[0] << 8) | (ln_preemp[1] << 10) | (ln_preemp[2] << 12) | (ln_preemp[3] << 14);
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg2, value);
	udelay(100000);

	/* 2k3000 default */
	CLK_HS = 2700;
	pixelclk_div_N = CLK_HS * 1000 / dp_param.dp_pixclk;
	pixelclk_div_F = CLK_HS * 1000 * 65536 / dp_param.dp_pixclk - pixelclk_div_N * 65536;
	if (connector && connector->base.edid_blob_ptr &&
		is_csw_special_display(connector->base.edid_blob_ptr->data) &&  \
		timing->hdisplay == 2880 && timing->vdisplay == 1800)
	pixelclk_div_F = pixelclk_div_F / 10000 * 10000;

	/* DP/EDP phy reg 12 TODO*/
	value = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg12);
	value &= ~(0xffffffff);
	value |= (0x1 << 31) | (pixelclk_div_F << 8) | pixelclk_div_N;
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg12, value);

	/* enable phy tx */
	value = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg0);
	value &= ~0x1;
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0, value);

	value = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg0);
	value |= 0x1;
	dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg0, value);

	DRM_DEBUG_DRIVER("wait dp phy ready......\n");
	DRM_DEBUG_DRIVER("addr_dp_phy_base + 0x0 = %x \n", dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg0));
	DRM_DEBUG_DRIVER("addr_dp_phy_base + 0x4 = %x \n", dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg1));
	DRM_DEBUG_DRIVER("addr_dp_phy_base + 0x8 = %x \n", dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg2));
	DRM_DEBUG_DRIVER("addr_dp_phy_base + 0xc = %x \n", dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg3));
	DRM_DEBUG_DRIVER("addr_dp_phy_base + 0x10 = %x \n", dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg4));
	DRM_DEBUG_DRIVER("addr_dp_phy_base + 0x14 = %x \n", dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg5));
	DRM_DEBUG_DRIVER("addr_dp_phy_base + 0x18 = %x \n", dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg6));
	DRM_DEBUG_DRIVER("addr_dp_phy_base + 0x1c = %x \n", dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg7));
	DRM_DEBUG_DRIVER("addr_dp_phy_base + 0x20 = %x \n", dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg8));
	DRM_DEBUG_DRIVER("addr_dp_phy_base + 0x30 = %x \n", dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg12));
	DRM_DEBUG_DRIVER("addr_dp_phy_base + 0x80 = %x \n", dc_readl(adev, gdc_reg->dp_reg[intf].phy_monitor0));
	DRM_DEBUG_DRIVER("addr_dp_phy_base + 0x84 = %x \n", dc_readl(adev, gdc_reg->dp_reg[intf].phy_monitor1));
	DRM_DEBUG_DRIVER("addr_dp_phy_base + 0x88 = %x \n", dc_readl(adev, gdc_reg->dp_reg[intf].phy_monitor2));

	DRM_DEBUG_DRIVER("wait dp phy ready......\n");
	/* wait phy ready */
	while ((dc_readl(adev, gdc_reg->dp_reg[intf].phy_monitor0) & 0x3) != 0x3);

	DRM_DEBUG_DRIVER("dp phy ready......\n");
}

static void dp_link_init(struct loonggpu_device *adev, int link,
		dp_feature_t dp_param, int intf, struct dc_timing_info *timing)
{
	uint32_t phy_rate, phy_lane;
	uint32_t link_rate, link_lane;
	uint32_t mode_bpc, tu_video_size;
	uint64_t dp_link_clk;
	uint64_t tmp;
	uint64_t dp_color_ratio = 1;	/* 2k3000_default */
	uint32_t dp_color_depth = 8;	/* 2k3000_default */
	uint32_t valid_rate = 0;
	uint32_t value;
	uint32_t i;
	struct loonggpu_dc_crtc *dc_crtc;

	dc_crtc = adev->dc->link_info[link].crtc;
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
	value |= ((0x1 << 30) | 0xf);
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

	value = dc_readl(adev,  gdc_reg->dp_reg[intf].link_cfg1);
	value &= ~(0xf << 16);
	value |= (mode_bpc << 16);

	/* link rate & lane count */
	value &= ~(0xff);
	value |= dp_param.dp_link_rate;
	value &= ~(0xff << 8);
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

	/* set tu size */
	tmp  = ((dp_param.dp_pixclk * 3 * dp_color_ratio * 64) / (dp_link_clk * link_lane)) / 1000;
	tu_video_size = (unsigned int)(tmp & 0xffffffff) + 1;
	if (is_ls2k3000_laptop(dc_crtc)) {
		if (tu_video_size > MAX_TU_LAPTOP)
			tu_video_size = MAX_TU_LAPTOP;
	} else {
		if (tu_video_size > MAX_TU_PC)
			tu_video_size = MAX_TU_PC;
	}

	DRM_DEBUG_DRIVER("==========================================================\n");
	DRM_DEBUG_DRIVER("pixclk: 0x%x\n", dp_param.dp_pixclk);
	DRM_DEBUG_DRIVER("dp_color_ratio: 0x%llx\n", dp_color_ratio);
	DRM_DEBUG_DRIVER("dp_link_clk: 0x%llx\n", dp_link_clk);
	DRM_DEBUG_DRIVER("link_lane: 0x%x\n", link_lane);

	DRM_DEBUG_DRIVER("tmp: 0x%llx\n", tmp);
	DRM_DEBUG_DRIVER("tu_video_size: 0x%x\n", tu_video_size);
	DRM_DEBUG_DRIVER("==========================================================\n");

	value = dc_readl(adev,  gdc_reg->dp_reg[intf].link_cfg4);
	value &= ~(0xff);
	value |= tu_video_size;
	dc_writel(adev,  gdc_reg->dp_reg[intf].link_cfg4, value);
}

static void dp_soft_training(struct loonggpu_device *adev, dp_feature_t dp_param, int intf)
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
	uint32_t value, max_retries = 4;
	bool training_success = false;
	unsigned int bytes_to_read, active_xlane;
	unsigned int interval_value, cr_interval, eq_interval;//ms
	struct encoder_resource *encoder_resource = NULL;
	bool scale_flags = false;

	if (!intf) {
		encoder_resource = dc_get_vbios_resource(adev->dc->vbios, intf, LOONGGPU_RESOURCE_ENCODER);
		if (encoder_resource && ((encoder_resource->chip == ENCODER_CHIP_ID_EDP_ICNM7601) ||  \
			(encoder_resource->chip == ENCODER_CHIP_ID_EDP_CS5611AQ_S)))
			scale_flags = true;
	}

	dp_aux_lock(adev, intf, true);

	if (scale_flags)
		dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, 0x3a10038b);

	/* dp soft reset */
	value = dc_readl(adev, gdc_reg->dp_reg[intf].link_cfg0);
	value |= (1 << 31);
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, value);

	/* TPS1*/
	/* enable dp TPS1 */
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, 0x3850038b);

	aux_data.addr = DPCD_REV_ADDR;
	aux_data.size = 1;
	aux_data.data_low = 0;
	aux_data.data_high = 0;
	dp_detect_flag = aux_config(adev, READ, aux_data, intf);
	if (dp_detect_flag == 1) {
		DRM_ERROR("Interface %d: No DP device detected!\n", intf);
		goto training_end;
	}
	tmp = dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor1) & 0xFF;
	dpcd_rev_major = (tmp >> 4) & 0x0F;
	dpcd_rev_minor = tmp & 0x0F;
	DRM_INFO("Interface %d: DP version: %d.%d\n", intf, dpcd_rev_major, dpcd_rev_minor);

	aux_data.addr = DPCD_MAX_LANE_COUNT_ADDR;
	aux_data.size = 1;
	aux_data.data_low = 0;
	aux_data.data_high = 0;
	aux_config(adev, READ, aux_data, intf);
	tps3_flag = (dc_readl(adev,  gdc_reg->dp_reg[intf].aux_monitor1) >> 6) & 0x1;
	if(tps3_flag == 1)
		DRM_DEBUG_DRIVER("Interface %d: TPS3 training pattern supported\n", intf);

	aux_data.addr = DPCD_SET_POWER_ADDR;
	aux_data.size = 1;
	aux_data.data_low = DP_POWER_STATE_D3;
	aux_data.data_high = 0;
	aux_config(adev, WRITE, aux_data, intf);
	udelay(10000);
	aux_data.data_low = DP_POWER_STATE_D0;
	aux_config(adev, WRITE, aux_data, intf);
	udelay(10000);
	aux_data.data_low = DP_POWER_STATE_D0;
	aux_config(adev, WRITE, aux_data, intf);
	udelay(10000);

	aux_data.addr = DPCD_LINK_BW_SET_ADDR;
	aux_data.size = 1;
	aux_data.data_low = dp_param.dp_link_rate;
	aux_data.data_high = 0;
	aux_config(adev, WRITE, aux_data, intf);

	aux_data.addr = DPCD_LANE_COUNT_SET_ADDR;
	aux_data.size = 1;
	aux_data.data_low = 0;
	aux_data.data_high = 0;
	aux_config(adev, READ, aux_data, intf);

	aux_data.addr = DPCD_LANE_COUNT_SET_ADDR;
	aux_data.size = 1;
	aux_data.data_low = 0x80 | dp_param.dp_link_xlane;
	aux_data.data_high = 0;
	aux_config(adev, WRITE, aux_data, intf);

	aux_data.addr = DPCD_DOWNSPREAD_CTRL_ADDR;
	aux_data.size = 1;
	aux_data.data_low = 0x10;
	aux_data.data_high = 0;
	aux_config(adev, WRITE, aux_data, intf);

	aux_data.addr = DPCD_LANE_COUNT_SET_ADDR;
	aux_data.size = 1;
	aux_data.data_low = 0x80 | dp_param.dp_link_xlane;
	aux_data.data_high = 0;
	aux_config(adev, WRITE, aux_data, intf);

	aux_data.addr = DPCD_TRAINING_AUX_RD_INTERVAL_ADDR;
	aux_data.size = 1;
	aux_data.data_low = 0;
	aux_data.data_high = 0;
	aux_config(adev, READ, aux_data, intf);
	interval_value = dc_readl(adev, gdc_reg->dp_reg[intf].aux_monitor1) & 0x7F;
	DRM_DEBUG_DRIVER("Interface %d: Link Status/Adjust Request read interval: 0x%x \n", intf, interval_value);
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

	DRM_INFO("Interface %d: Starting TPS1 (Clock Recovery) training\n", intf);
	aux_data.addr = DPCD_TRAINING_PATTERN_SET_ADDR;
	aux_data.size = 1;
	aux_data.data_low = TRAINING_PATTERN_1;
	aux_data.data_high = 0;
	aux_config(adev, WRITE, aux_data, intf);

	active_xlane = dp_param.dp_link_xlane;
	bytes_to_read = (active_xlane > 2) ? 2 : 1;
	lt_cnt = 0;

	while (lt_cnt < max_retries) {
		DRM_DEBUG_DRIVER("wait for CR_DONE!\n");
		udelay(cr_interval * 1000);

		/* training lane req from sink */
		aux_data.addr = DPCD_ADJUST_REQUEST_LANE0_1_ADDR;
		aux_data.size = bytes_to_read;
		aux_data.data_low = 0;
		aux_data.data_high = 0;
		aux_config(adev, READ, aux_data, intf);

		tmp = dc_readl(adev,  gdc_reg->dp_reg[intf].aux_monitor1);
		for (i = 0; i < active_xlane; i++) {
			ln_preemp[i] = (tmp >> (2 + i * 4)) & 0x3;
			ln_vswing[i] = (tmp >> (i * 4)) & 0x3;
			ln_vswing_max[i] = (ln_vswing[i] == 0x3) ? 1 : 0;
			ln_preemp_max[i] = (ln_preemp[i] == 0x3) ? 1 : 0;
			ln_set[i] = ln_vswing[i] | (ln_vswing_max[i] << 2) |
				(ln_preemp[i] << 3) | (ln_preemp_max[i] << 5);
		}

		if (!(dpcd_rev_major == 1 && dpcd_rev_minor < 2)) {
			/* set dp phy pe and vs */
			value = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg2);
			value |= 0xf;
			value &= ~(0xff << 8);
			value |= (ln_preemp[0] << 8) | (ln_preemp[1] << 10) | (ln_preemp[2] << 12) | (ln_preemp[3] << 14);
			dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg2, value);
			udelay(100 * 1000);

			value = (4 - ln_vswing[0]) | ((4 - ln_vswing[1]) << 3) | ((4 - ln_vswing[2]) << 6) | ((4 - ln_vswing[3]) << 9);
			dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg7, value);
			udelay(100 * 1000);
		}

		for (i = 0; i < active_xlane; i++) {
			aux_data.addr = (DPCD_TRAINING_LANE0_SET_ADDR + i);
			aux_data.size = 1;
			aux_data.data_low = ln_set[i];
			aux_data.data_high = 0;
			aux_config(adev, WRITE, aux_data,  intf);
		}
		udelay(cr_interval * 1000);

		aux_data.addr = DPCD_LANE0_1_STATUS_ADDR;
		aux_data.size = bytes_to_read;
		aux_data.data_low = 0;
		aux_data.data_high = 0;
		aux_config(adev, READ, aux_data, intf);
		tmp = dc_readl(adev,  gdc_reg->dp_reg[intf].aux_monitor1);

		for (i = 0; i < active_xlane; i++)
			ln_cr_done[i] = (tmp >> (i * 4)) & 0x1;

		cr_done = (active_xlane == 4) ? (ln_cr_done[0] & ln_cr_done[1] & ln_cr_done[2] & ln_cr_done[3]) :
					(active_xlane == 2) ? (ln_cr_done[0] & ln_cr_done[1]) :
					(active_xlane == 1) ? (ln_cr_done[0]) : 0;

		if (cr_done) {
			DRM_INFO("Interface %d: TPS1 Clock Recovery successful after %d attempts. cr_interval: %dms\n",
			          intf, lt_cnt + 1, cr_interval);
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
		DRM_INFO("Interface %d: Starting TPS3 training\n", intf);
		aux_data.addr = DPCD_TRAINING_PATTERN_SET_ADDR;
		aux_data.size = 1;
		aux_data.data_low = TRAINING_PATTERN_3;
		aux_data.data_high = 0;
		aux_config(adev, WRITE, aux_data, intf);
		dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, 0x3810038b | (0x4 << 22)); // enable TPS3
	} else {
		DRM_INFO("Interface %d: Starting TPS2 training\n", intf);
		aux_data.addr = DPCD_TRAINING_PATTERN_SET_ADDR;
		aux_data.size = 1;
		aux_data.data_low = TRAINING_PATTERN_2;
		aux_data.data_high = 0;
		aux_config(adev, WRITE, aux_data, intf);
		dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, 0x3810038b | (0x2 << 22)); // enable TPS2
	}

	lt_cnt = 0;
	lt_done = 0;

	while (lt_cnt < max_retries) {
		DRM_DEBUG_DRIVER("wait a second!\n");
		udelay(eq_interval * 1000);

		/* training lane req from sink */
		aux_data.addr = DPCD_ADJUST_REQUEST_LANE0_1_ADDR;
		aux_data.size = bytes_to_read;
		aux_data.data_low = 0;
		aux_data.data_high = 0;
		aux_config(adev, READ, aux_data, intf);
		tmp = dc_readl(adev,  gdc_reg->dp_reg[intf].aux_monitor1);

		for (i = 0; i < active_xlane; i++) {
			ln_vswing[i] = (tmp >> (i * 4)) & 0x3;
			ln_preemp[i] = (tmp >> (2 + i * 4)) & 0x3;
			ln_vswing_max[i] = (ln_vswing[i] == 0x3) ? 1 : 0;
			ln_preemp_max[i] = (ln_preemp[i] == 0x3) ? 1 : 0;
			ln_set[i] = ln_vswing[i] | (ln_vswing_max[i] << 2) |
				(ln_preemp[i] << 3) | (ln_preemp_max[i] << 5);
		}

                if (!(dpcd_rev_major == 1 && dpcd_rev_minor < 2)) {
                        /* set dp phy pe and vs */
                        value = dc_readl(adev, gdc_reg->dp_reg[intf].phy_cfg2);
                        value |= 0xf;
                        value &= ~(0xff << 8);
                        value |= (ln_preemp[0] << 8) | (ln_preemp[1] << 10) | (ln_preemp[2] << 12) | (ln_preemp[3] << 14);
                        dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg2, value);
                        udelay(100 * 1000);

                        value = (4 - ln_vswing[0]) | ((4 - ln_vswing[1]) << 3) | ((4 - ln_vswing[2]) << 6) | ((4 - ln_vswing[3]) << 9);
                        dc_writel(adev, gdc_reg->dp_reg[intf].phy_cfg7, value);
                        udelay(100 * 1000);
                }

		for (i = 0; i < active_xlane; i++) {
			DRM_DEBUG_DRIVER("ln_set[%d] = %x \n", i, ln_set[i]);
			aux_data.addr = (DPCD_TRAINING_LANE0_SET_ADDR + i);
			aux_data.size = 1;
			aux_data.data_low = ln_set[i];
			aux_data.data_high = 0;
			aux_config(adev, WRITE, aux_data, intf);
		}

		DRM_DEBUG_DRIVER("wait a second!\n");
		udelay(eq_interval * 1000);

		aux_data.addr = DPCD_LANE0_1_STATUS_ADDR;
		aux_data.size = bytes_to_read;
		aux_data.data_low = 0;
		aux_data.data_high = 0;
		aux_config(adev, READ, aux_data, intf);
		tmp = dc_readl(adev,  gdc_reg->dp_reg[intf].aux_monitor1);

		for (i = 0; i < active_xlane; i++) {
			ln_cr_done[i] = (tmp >> (i * 4)) & 0x1;         /* bit0: CR_DONE */
			ln_eq_done[i] = (tmp >> (1 + i * 4)) & 0x1;     /* bit1: CHANNEL_EQ_DONE */
			ln_sl_done[i] = (tmp >> (2 + i * 4)) & 0x1;     /* bit2: SYMBOL_LOCKED */
		}

		aux_data.addr = DPCD_LANE_ALIGN_STATUS_UPDATED_ADDR;
		aux_data.size = 1;
		aux_data.data_low = 0;
		aux_data.data_high = 0;
		aux_config(adev, READ, aux_data, intf);
		tmp = dc_readl(adev,  gdc_reg->dp_reg[intf].aux_monitor1);
		interlane_align_done = tmp & 0x1;   /* Bit 0 = INTERLANE_ALIGN_DONE */

		if (active_xlane == 4) {
			lt_done = ln_cr_done[0] & ln_cr_done[1] & ln_cr_done[2] & ln_cr_done[3] &
			          ln_eq_done[0] & ln_eq_done[1] & ln_eq_done[2] & ln_eq_done[3] &
			          ln_sl_done[0] & ln_sl_done[1] & ln_sl_done[2] & ln_sl_done[3] &
			          interlane_align_done;
		} else if (active_xlane == 2) {
			lt_done = ln_cr_done[0] & ln_cr_done[1] &
			          ln_eq_done[0] & ln_eq_done[1] &
			          ln_sl_done[0] & ln_sl_done[1] &
			          interlane_align_done;
		} else {  /* 1 lane */
			lt_done = ln_cr_done[0] & ln_eq_done[0] & ln_sl_done[0] & interlane_align_done;
		}

		if (lt_done) {
			DRM_INFO("Interface %d: TPS%s training successful after %d attempts. eq/sl/al_interval: %dms\n",
			          intf, tps3_flag ? "3" : "2", lt_cnt + 1, eq_interval);
			training_success = true;
			break;
		}

		lt_cnt++;
		eq_interval = eq_interval + 5;
	}

	if (!lt_done) {
		DRM_WARN("Interface %d: TPS%s training failed after %d attempts\n", intf, tps3_flag ? "3" : "2", max_retries);
		aux_data.addr = DPCD_LANE0_1_STATUS_ADDR;
		if (scale_flags)
			aux_data.size = bytes_to_read;
		else
			aux_data.size = 3;
		aux_data.data_low = 0;
		aux_data.data_high = 0;
		aux_config(adev, READ, aux_data, intf);
		tmp = dc_readl(adev,  gdc_reg->dp_reg[intf].aux_monitor1);
		DRM_WARN("Interface %d: lane status:0x%x, active_xlane:%d\n", intf, tmp, active_xlane);
		if (scale_flags) {
			aux_data.addr = DPCD_LANE_ALIGN_STATUS_UPDATED_ADDR;
			aux_data.size = 1;
			aux_data.data_low = 0;
			aux_data.data_high = 0;
			aux_config(adev, READ, aux_data, intf);
			interlane_align_done = dc_readl(adev,  gdc_reg->dp_reg[intf].aux_monitor1);
			DRM_WARN("Interface %d: lane status:0x%x, lane align status:0x%x, active_xlane:%d\n", intf, tmp, interlane_align_done, active_xlane);
		}
	}

skip_tps23:
	aux_data.addr = DPCD_TRAINING_PATTERN_SET_ADDR;
	aux_data.size = 1;
	aux_data.data_low = TRAINING_PATTERN_DISABLED;
	aux_data.data_high = 0;
	aux_config(adev, WRITE, aux_data, intf);
	udelay(cr_interval * 1000);

training_end:
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, 0x3830038b);
	value = dc_readl(adev, gdc_reg->dp_reg[intf].link_cfg0);
	value |= (0x3 << 20);
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, value);

	if (training_success) {
		DRM_INFO("Interface %d: DP soft training completed successfully\n", intf);
	} else if (dp_detect_flag == 1) {
		DRM_ERROR("Interface %d: DP training aborted - no device detected\n", intf);
	} else {
		DRM_ERROR("Interface %d: DP training failed.\n", intf);
	}

	dp_aux_lock(adev, intf, false);

	return;
}

static int dp_aa_aux_init(struct loonggpu_device *adev, int intf)
{
	unsigned int value;

	value = dc_readl(adev, gdc_reg->dp_reg[intf].link_cfg0);
	value |= (0x1 << 31);
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, value);
	msleep(100);

	value = dc_readl(adev, gdc_reg->dp_reg[intf].aux_channel0);
	value |= (0x1 << 1);
	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel0, value);

	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel8, 0x1a0ffff8);
	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel9, 0x0004aeae);

	value = dc_readl(adev, gdc_reg->dp_reg[intf].link_cfg0);
	value &= ~0x1;
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, value);
	value |= 0x1;
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, value);

	msleep(50);

	return 0;
}

static int dp_ca_aux_init(struct loonggpu_device *adev, int intf)
{
	unsigned int value;

	value = dc_readl(adev, gdc_reg->dp_reg[intf].link_cfg0);
	value |= BIT(31);
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, value);
	msleep(100);

	dc_writel(adev, gdc_reg->dp_reg[intf].aux_monitor0, 0);
	value = dc_readl(adev, gdc_reg->dp_reg[intf].aux_channel0);
	value |= BIT(1);
	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel0, value);

	value = dc_readl(adev, gdc_reg->dp_reg[intf].aux_channel0);
	value |= BIT(4);
	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel0, value);

	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel8, 0x1a0ffff8);

	value = dc_readl(adev, gdc_reg->dp_reg[intf].aux_channel9);
	value |= 0x32 | (0x32 << 8);
	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel9, value);

	value = dc_readl(adev, gdc_reg->dp_reg[intf].aux_channel10);
	value |= 0x32 | (0xa << 8) | (0xa << 12);
	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel10, value);

	value = dc_readl(adev, gdc_reg->dp_reg[intf].link_cfg0);
	value &= ~0x1;
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, value);
	value |= 0x1;
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, value);

	msleep(50);

	aux_preinit_for_edid_2k3000ca(adev, intf);

	return 0;
}

void ls2k3000_dp_pll_set(struct loonggpu_dc_crtc *crtc, int intf, struct dc_timing_info *timing)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	dp_feature_t recheck_dp_param;
	dp_feature_t dp_param;
	u32 link_cfg0;
	bool ret;

	ret = dp_check_bandwidth(timing->clock, &dp_param);
	if (!ret)
		return;

	link_cfg0 = dc_readl(adev, gdc_reg->dp_reg[intf].link_cfg0);
	link_cfg0 &= ~0x1;
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, link_cfg0);
	dc_writel(adev, gdc_reg->crtc_reg[intf].cfg, 0);

	link_cfg0 = dc_readl(adev, gdc_reg->dp_reg[intf].link_cfg0);
	link_cfg0 |= 0x1;
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, link_cfg0);

	if (is_ls2k3000_laptop(crtc))
		dp_recheck_bandwidth(adev, timing, &recheck_dp_param, intf);
	else
		edp_converters_recheck_bandwidth(adev, timing->clock, &recheck_dp_param, intf);

	dp_phy_init(adev, timing, recheck_dp_param, intf, 0, 0);
	dp_link_init(adev, crtc->resource->base.link, recheck_dp_param,
			intf, timing);
	dp_soft_training(adev, recheck_dp_param, intf);

	return;
}

bool ls2k3000_dp_enable(struct loonggpu_dc_crtc *crtc, int intf, bool enable)
{
	struct loonggpu_device *adev = crtc->dc->adev;
        aux_msg_t aux_data;
        u32 ret;

	if (!crtc->intf[intf].connected)
		return false;

	if ((enable && crtc->intf[intf].enabled) ||
		(!enable && !crtc->intf[intf].enabled))
		return true;

	dp_aux_lock(adev, intf, true);
	if (enable) {
		aux_data.addr = DPCD_SET_POWER_ADDR;
		aux_data.size = 1;
		aux_data.data_low = DP_POWER_STATE_D0;
		aux_data.data_high = 0;
		ret = aux_config(adev,  WRITE, aux_data, intf);
		if (!ret)
			crtc->intf[intf].enabled = true;
	} else {
		aux_data.addr = DPCD_SET_POWER_ADDR;
		aux_data.size = 1;
		aux_data.data_low = DP_POWER_STATE_D3;
		aux_data.data_high = 0;
		ret = aux_config(adev,  WRITE, aux_data, intf);
		if (!ret)
			crtc->intf[intf].enabled = false;
	}
	dp_aux_lock(adev, intf, false);

	DRM_INFO("SWITCH DISPLAY THROUGH AUX: %d-%d\n", enable, ret);
	return true;
}

void ls2k3000_dp_suspend(struct loonggpu_dc_crtc *crtc, int intf)
{
}

int ls2k3000_dp_resume(struct loonggpu_dc_crtc *crtc, int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	struct loonggpu_dc *dc = adev->dc;

	dc->hw_ops->first_hpd_detect(crtc, intf);
	return 0;
}

static int ls2k3000_dp_aux_detect_status(struct loonggpu_dc_crtc *crtc, int intf, struct connector_resource *resouce)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	unsigned int detect_flag = 1;
	aux_msg_t aux_data;
	int i;

	/**
	" When the hotplug mode is set to interrupt mode, the initial hotplug status during first boot cannot be
	* retrieved via interrupt when only a DP display is connected. Therefore, the first hotplug connection status
	* is probed via aux. Additionally, when both DP and EDP are connected simultaneously, the initial connection
	* status for both EDP and DP cannot be obtained through interrupts either, necessitating AUX probing for their
	* first connection states as well. However, when the hotplug mode is configured to FORCE_ON, AUX probing for
	* hotplug status is not required.
	*/
	if (resouce->hotplug != FORCE_ON) {
		dp_aux_lock(adev, intf, true);
		aux_data.addr = DPCD_REV_ADDR;
		aux_data.size = 1;
		aux_data.data_low = 0;
		aux_data.data_high = 0;
		detect_flag = aux_config(adev, READ, aux_data, intf);
		dp_aux_lock(adev, intf, false);
		for (i = 0; i < crtc->interfaces; i++) {
			switch (crtc->intf[i].type) {
			case INTERFACE_DP:
				if (!detect_flag) {
					adev->dp_status[1] = 0x2;
				} else {
					adev->dp_status[1] = 0x8;
				}
				break;
			case INTERFACE_EDP:
				if (!detect_flag) {
					adev->dp_status[0] = 0x1;
				} else {
					adev->dp_status[0] = 0x4;
				}
				break;
			}
		}
	}

	return 0;
}

int ls2k3000_dp_init(struct loonggpu_dc_crtc *crtc, int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	struct connector_resource *connector_res;

	connector_res = adev->dc->link_info[intf].connector;
	if (!is_use_dp(adev, intf, connector_res))
		return 0;

	if (adev->chip == dev_2k3000 && adev->dc_revision == 0x10)
		dp_aa_aux_init(adev, intf);
	else if (adev->chip == dev_2k3000 && adev->dc_revision == 0x1)
		dp_ca_aux_init(adev, intf);

	ls2k3000_dp_noaudio_init(crtc, intf);

	return ls2k3000_dp_aux_detect_status(crtc, intf, connector_res);
}

bool is_dp_hpd_irq(u32 reg)
{
	uint32_t mask = DP_IN_MASK | DP_OUT_MASK | EDP_IN_MASK | EDP_OUT_MASK;

	return (mask & reg) != 0;
}

void dp_aux_lock(struct loonggpu_device *adev, int intf, bool lock)
{
	u32 int_reg = 0;
	u32 lock_mask;
	u32 tries = 100;

	if (lock) {
		lock_mask = intf ? DP_LOCK : EDP_LOCK;
		while (tries--) {
			int_reg	= dc_readl(adev, gdc_reg->global_reg.intr_en);
			if (!(int_reg & lock_mask))
				break;

			msleep(1);
		}

		if (!tries)
			DRM_ERROR("dp aux lock timeout!\n");
	} else {
		int_reg	= dc_readl(adev, gdc_reg->global_reg.intr_en);
		/* TODO: use owner to determine whether to release the lock
		 * because we cannot release the lock of the cpu firmware
		 */
	}

	switch (intf) {
	case 0:
		if (lock)
			int_reg |= (EDP_LOCK | EDP_OWNER);
		else
			int_reg &= ~(EDP_LOCK | EDP_OWNER);
		break;
	case 1:
		if (lock)
			int_reg |= (DP_LOCK | DP_OWNER);
		else
			int_reg &= ~(DP_LOCK | DP_OWNER);
		break;
	}

	dc_writel(adev, gdc_reg->global_reg.intr_en, int_reg);
}

void l2k3000_hpd_irq_handler(struct loonggpu_device *adev, struct loonggpu_iv_entry *entry)
{
	u32 hpd_status = 0;
	u32 dp_hpd_en = 0;

	dp_hpd_en = dc_readl(adev, gdc_reg->global_reg.intr_en);
	hpd_status = (dp_hpd_en >> 24) & 0xff;

	switch (hpd_status) {
	case DP_HPD_IN:
		dc_writel(adev, gdc_reg->global_reg.intr_en, dp_hpd_en & (~DP_IN_MASK));
		adev->dp_status[1] = 0x2;
		entry->src_id = DC_INT_ID_HPD_DP;
		loonggpu_irq_dispatch(adev, entry);
		break;
	case DP_HPD_OUT:
		dc_writel(adev, gdc_reg->global_reg.intr_en, dp_hpd_en & (~DP_OUT_MASK));
		adev->dp_status[1] = 0x8;
		entry->src_id = DC_INT_ID_HPD_DP;
		loonggpu_irq_dispatch(adev, entry);
		break;
	case EDP_HPD_IN:
		dc_writel(adev, gdc_reg->global_reg.intr_en, dp_hpd_en & (~EDP_IN_MASK));
		adev->dp_status[0] = 0x1;
		entry->src_id = DC_INT_ID_HPD_EDP;
		loonggpu_irq_dispatch(adev, entry);
		break;
	case EDP_HPD_OUT:
		dc_writel(adev, gdc_reg->global_reg.intr_en, dp_hpd_en & (~EDP_OUT_MASK));
		adev->dp_status[0] = 0x4;
		entry->src_id = DC_INT_ID_HPD_EDP;
		loonggpu_irq_dispatch(adev, entry);
		break;
	}
}

void aux_preinit_for_edid_2k3000ca(struct loonggpu_device *adev, int intf)
{
	u32 value;

	value = dc_readl(adev, gdc_reg->dp_reg[intf].link_cfg0);
	value |= BIT(31);
	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, value);

	dc_writel(adev, gdc_reg->dp_reg[intf].link_cfg0, 0x3850038b);
}

void l2k3000_dp_first_hdp_detect(struct loonggpu_dc_crtc *crtc, int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	struct connector_resource *connector_res;
	dp_feature_t dp_param;
	aux_msg_t aux_msg;

	connector_res = adev->dc->link_info[intf].connector;
	if (!is_use_dp(adev, intf, connector_res))
		return;

	/*
	* When entering S3 or S4 states with no EDP or DP connected, the enable bits of the aux_channel0and
	* link_cfg0registers are cleared. Therefore, prior to wake-up, a dp_aux_initoperation must be performed
	* to ensure the aux_channel0register is enabled for correct EDID reading and AUX connection state probing.
	*/
	if (adev->chip == dev_2k3000 && adev->dc_revision == 0x10)
		dp_aa_aux_init(adev, intf);
	else if (adev->chip == dev_2k3000 && adev->dc_revision == 0x1)
		dp_ca_aux_init(adev, intf);
	dp_param.dp_phy_rate = DP_PHY_1P62G;
	dp_param.dp_phy_xlane = DP_PHY_X4;
	dp_param.dp_link_rate = DP_LINK_1P62G;
	dp_param.dp_link_xlane = DP_LINK_X4;
	dp_param.dp_pixclk = 148500;

	/*
	* The AUX functionality depends on the EDP/DP PHY. Therefore, the EDP/DP PHY must be
	* initialized prior to probing the link status via AUX
	*/
	dp_phy_init(adev, crtc->timing, dp_param, intf, 0x7, 0);

	if (adev->chip == dev_2k3000 && adev->dc_revision == 0x1)
		aux_preinit_for_edid_2k3000ca(adev, intf);

	/*
	* Prior to entering S3 or S4 states, if no EDP/DP display is connected, a power off and on
	* cycle must be performed via the AUX channel. Otherwise, certain displays may fail to report
	* the correct connection status
	*/
	dp_aux_lock(adev, intf, true);
	aux_msg.addr = DPCD_SET_POWER_ADDR;
	aux_msg.size = 1;
	aux_msg.data_low = DP_POWER_STATE_D3;
	aux_msg.data_high = 0;
	aux_config(adev,  WRITE, aux_msg, intf);
	udelay(10000);

	aux_msg.addr = DPCD_SET_POWER_ADDR;
	aux_msg.size = 1;
	aux_msg.data_low = DP_POWER_STATE_D0;
	aux_msg.data_high = 0;
	aux_config(adev,  WRITE, aux_msg, intf);
	udelay(10000);
	dp_aux_lock(adev, intf, false);

	ls2k3000_dp_aux_detect_status(crtc, intf, connector_res);
}

int ls2k3000_dp_audio_init(struct loonggpu_dc_crtc *crtc, int intf)
{
	ls2k3000_dp_noaudio_init(crtc, intf);
	return 0;
}

int ls2k3000_dp_noaudio_init(struct loonggpu_dc_crtc *crtc, int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	u32 value;

	value = dc_readl(adev,  gdc_reg->dp_reg[intf].sdp_cfg5);
	value &= ~(0x1 << 31);
	dc_writel(adev,  gdc_reg->dp_reg[intf].sdp_cfg5, value);

	value = dc_readl(adev,  gdc_reg->dp_reg[intf].sdp_cfg7);
	value &= ~(0x3 << 30);
	dc_writel(adev,  gdc_reg->dp_reg[intf].sdp_cfg7, value);

	return 0;
}

bool is_ls2k3000_laptop(struct loonggpu_dc_crtc *crtc)
{
	struct connector_resource *connector_resource;
	struct loonggpu_dc *dc = crtc->dc;

	connector_resource = dc_get_vbios_resource(dc->vbios,
			0, LOONGGPU_RESOURCE_CONNECTOR);

	if (connector_resource && connector_resource->type == DRM_MODE_CONNECTOR_eDP)
		return true;

	return false;
}
