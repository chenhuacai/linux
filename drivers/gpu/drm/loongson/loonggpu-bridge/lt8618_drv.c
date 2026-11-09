// SPDX-License-Identifier: GPL-2.0-or-later

#include "lt8618_regs.h"

#define DEBUG_MSEEAGE 0
#define CHIP_PREFIX lt8618

/**
 * @section LT8618 helper functions
 */
DEFINE_SIMPLE_REG_SEQ_FUNC(sw_reset)
DEFINE_SIMPLE_REG_SEQ_FUNC(input_analog)
DEFINE_SIMPLE_REG_SEQ_FUNC(output_analog)

static int lt8618_hw_reset(struct loonggpu_bridge_phy *phy)
{
	int ret;
	int reset_gpio = 0;
	char *chip_name;
	char pin_name[NAME_SIZE_MAX];
	struct gpio_desc *gpiod;

	chip_name = phy->res->chip_name;
	//reset_gpio = phy->res->reset_gpio;
	ret = gpio_is_valid(reset_gpio);
	if (!ret)
		goto gpio_invalid;
	sprintf(pin_name, "%s-reset", chip_name);
	ret = gpio_request(reset_gpio, pin_name);
	if (!ret)
		goto gpio_nodev;

	gpiod = gpio_to_desc(reset_gpio);
	gpiod_set_value_cansleep(gpiod, 0);
	schedule_timeout(msecs_to_jiffies(100));
	gpiod_set_value_cansleep(gpiod, 1);
	schedule_timeout(msecs_to_jiffies(100));

	return 0;

gpio_nodev:
	DRM_ERROR("Failed to request reset gpio pin %d\n",
		      reset_gpio);
gpio_invalid:
	DRM_ERROR("Invalid reset gpio pin number %d\n",
		      reset_gpio);

	return -ENODEV;
}

static int lt8618_sw_enable(struct loonggpu_bridge_phy *phy)
{
	bridge_phy_reg_update_bits(phy, LT8618_REG_ENABLE,
			   LT8618_REG_CHIP_ENABLE_MSK, ENABLE_REG_BANK);

	return 0;
}

static int lt8618_video_input_analog(struct loonggpu_bridge_phy *phy)
{
	SIMPLE_REG_SEQ_FUNC(input_analog)(phy);

	return 0;
}

static int lt8618_video_input_param(struct loonggpu_bridge_phy *phy,
				    enum input_video_type video_type,
				    enum input_data_lane_seq lane_seq,
				    enum input_signal_sample_type signal_type)
{
	const struct reg_sequence lt8618_video_input_param_seq[] = {
		/* RGB_SYNV_DE */
		{ LT8618_REG_INPUT_VIDEO_TYPE, video_type * 0x10 + 0x80 },
		/* Input video signal sync type */
		{ LT8618_REG_INPUT_VIDEO_SYNC_GEN, video_type * 0x40 + 0x07 },
		/* RGB lane sequence */
		{ LT8618_REG_INPUT_DATA_LANE_SEQ, lane_seq * 0x10 },
		/* Input single sample type */
		{ LT8618_REG_INPUT_SIGNAL_SAMPLE_TYPE, signal_type * 0x40 },
		/* Input single source select */
		{ LT8618_REG_INPUT_SRC_SELECT, 0x00 },
	};

	regmap_multi_reg_write(phy->phy_regmap, lt8618_video_input_param_seq,
			       ARRAY_SIZE(lt8618_video_input_param_seq));

	return 0;
}

static int lt8618_video_input_timing_check(struct loonggpu_bridge_phy *phy)
{
	int hs_pol, vs_pol;
	int horiz_total, verti_total;
	int pixel_clk;
	u8 input_video_parameters[INPUT_VIDEO_TIMING_LEN];
	u8 freq_meter2[FREQ_METER2_LEN];
	struct regmap *regmap;
	struct videomode vmode = {};
	struct drm_display_mode mode = {};

	regmap = phy->phy_regmap;

	regmap_bulk_read(regmap, LT8618_REG_INPUT_VIDEO_TIMING_BASE,
			 &input_video_parameters, INPUT_VIDEO_TIMING_LEN);
	hs_pol = test_bit(INPUT_VIDEO_TIMING_HS_POL_POS,
			  (unsigned long *)&input_video_parameters);
	vs_pol = test_bit(INPUT_VIDEO_TIMING_VS_POL_POS,
			  (unsigned long *)&input_video_parameters);
	vmode.flags |= BIT(hs_pol);
	vmode.flags |= BIT(vs_pol + 2);
	vmode.vsync_len = input_video_parameters[1];
	vmode.hsync_len =
		(input_video_parameters[2] << 8) + input_video_parameters[3];
	vmode.vback_porch = input_video_parameters[4];
	vmode.vfront_porch = input_video_parameters[5];
	vmode.hback_porch =
		(input_video_parameters[6] << 8) + input_video_parameters[7];
	vmode.hfront_porch =
		(input_video_parameters[8] << 8) + input_video_parameters[9];
	verti_total =
		(input_video_parameters[10] << 8) + input_video_parameters[11];
	horiz_total =
		(input_video_parameters[12] << 8) + input_video_parameters[13];
	vmode.vactive =
		(input_video_parameters[14] << 8) + input_video_parameters[15];
	vmode.hactive =
		(input_video_parameters[16] << 8) + input_video_parameters[17];

	regmap_bulk_read(regmap, LT8618_REG_FREQ_METER2_BASE, &freq_meter2,
			 FREQ_METER2_LEN);
	pixel_clk = ((freq_meter2[FREQ_METER2_STATUS] & 0x0F) << 16) +
		    (freq_meter2[FREQ_METER2_PCLK_H] << 8) +
		    freq_meter2[FREQ_METER2_PCLK_L];
	/* Raw data is in kHz */
	pixel_clk *= 1000;

#if defined(CONFIG_VIDEOMODE_HELPERS)
	drm_display_mode_from_videomode((const struct videomode *)&vmode,
					&mode);
#endif
	DRM_DEBUG("Input timing check: total %dx%d, pclk=%d\n", horiz_total,
		  verti_total, pixel_clk);
	drm_mode_debug_printmodeline(&mode);

	return 0;
}

static int lt8618_video_input_cfg(struct loonggpu_bridge_phy *phy)
{
	lt8618_video_input_analog(phy);
	lt8618_video_input_param(phy, RGB_WITH_SYNC_DE, DATA_LANE_SEQ_RGB,
				 SDR_CLK);
	lt8618_video_input_timing_check(phy);

	return 0;
}

static int lt8618_video_output_analog(struct loonggpu_bridge_phy *phy)
{
	SIMPLE_REG_SEQ_FUNC(output_analog)(phy);

	return 0;
}

static int lt8618_video_output_pll_range(struct loonggpu_bridge_phy *phy)
{
	int vic;
	u8 ver, lv;
	unsigned int i;
	struct lt8618_device *lt8618_dev;
	struct drm_display_mode *mode;

	u8 pll_range_cgf_regs[] = { 0x25, 0x2c, 0x2d };
	struct reg_sequence pll_range_cgf[3] = {};

	lt8618_dev = (struct lt8618_device *)phy->priv;
	mode = phy->mode_config.input_mode.mode;

	if (!mode)
		return 0;

	drm_mode_debug_printmodeline(mode);
	ver = lt8618_dev->ver;
	lv = mode->clock / 50000;
	vic = drm_match_cea_mode(mode);
	DRM_DEBUG("8618 ver%d,clock%d,lv%d,vic%d\n", ver, mode->clock, lv, vic);
	lt8618_dev->pll_level = lv;

	for (i = 0; i < ARRAY_SIZE(pll_range_cgf_regs); i++) {
		pll_range_cgf[i].reg = 0x8100 + pll_range_cgf_regs[i];
		pll_range_cgf[i].def =
			lt8618_pll_range_timing[ver - 1][lv - 1][i];
	}
	regmap_multi_reg_write(phy->phy_regmap, pll_range_cgf,
			       ARRAY_SIZE(pll_range_cgf));

	return 0;
}

static int lt8618_video_output_pll_cfg(struct loonggpu_bridge_phy *phy)
{
	u8 clk_type;
	unsigned int i;
	unsigned int val;
	unsigned int lock, cali_val, cali_done;
	struct lt8618_device *lt8618_dev;
	struct reg_sequence pll_cfg[] = {
		{ 0x814d, 0x00 },
		{ 0x8127, 0x60 },
		{ 0x8128, 0x88 },
	};

	lt8618_dev = phy->priv;
	clk_type = phy->mode_config.input_mode.input_signal_type;
	if (clk_type == SDR_CLK) {
		if (lt8618_dev->ver == LT8618_VER_U2)
			pll_cfg[2].def = 0x00;
	} else if (clk_type == DDR_CLK) {
		if (lt8618_dev->ver == LT8618_VER_U2) {
			regmap_read(phy->phy_regmap, 0x812c, &val);
			val &= 0x7F;
			val = val * 2 | 0x80;
			regmap_write(phy->phy_regmap, 0x812c, val);
			pll_cfg[0].def = 0x04;
		} else if (lt8618_dev->ver == LT8618_VER_U3)
			pll_cfg[0].def = 0x05;
	}
	regmap_multi_reg_write(phy->phy_regmap, pll_cfg, ARRAY_SIZE(pll_cfg));

	/* sw_en_txpll_cal_en */
	regmap_read(phy->phy_regmap, 0x812B, &val);
	val &= 0xFD;
	regmap_write(phy->phy_regmap, 0x812B, val);

	/* sw_en_txpll_iband_set */
	regmap_read(phy->phy_regmap, 0x812E, &val);
	val &= 0xFE;
	regmap_write(phy->phy_regmap, 0x812E, val);

	/* txpll _sw_rst_n */
	regmap_multi_reg_write(phy->phy_regmap, lt8618_pll_cfg_seq,
			       ARRAY_SIZE(lt8618_pll_cfg_seq));

	if (lt8618_dev->ver == LT8618_VER_U3) {
		regmap_write(phy->phy_regmap, 0x812A, 0x10 * clk_type);
		regmap_write(phy->phy_regmap, 0x812A, 0x10 * clk_type + 0x20);
	}

	for (i = 0; i < 5; i++) {
		/* pll lock logic reset */
		regmap_write(phy->phy_regmap, 0x8016, 0xE3);
		regmap_write(phy->phy_regmap, 0x8016, 0xF3);

		regmap_read(phy->phy_regmap, 0x8215, &lock);
		regmap_read(phy->phy_regmap, 0x82EA, &cali_val);
		regmap_read(phy->phy_regmap, 0x82EB, &cali_done);
		lock &= 0x80;
		cali_done &= 0x80;

		if (lock && cali_done && (cali_val != 0xff))
			DRM_DEBUG("TXPLL Lock\n");
		else {
			regmap_write(phy->phy_regmap, 0x8016, 0xF1);
			/* txpll _sw_rst_n */
			regmap_write(phy->phy_regmap, 0x8018, 0xDC);
			regmap_write(phy->phy_regmap, 0x8018, 0xFC);
			regmap_write(phy->phy_regmap, 0x8016, 0xF3);
			DRM_DEBUG("TXPLL Reset\n");
		}
	}

	return 0;
}

static int lt8618_video_output_cfg(struct loonggpu_bridge_phy *phy)
{
	lt8618_video_output_analog(phy);
	lt8618_video_output_pll_range(phy);
	lt8618_video_output_pll_cfg(phy);

	return 0;
}

static int lt8618_video_output_timing(struct loonggpu_bridge_phy *phy,
				      const struct drm_display_mode *mode)
{
	unsigned int i;
	u8 video_timing_arr[18];
	struct videomode *vmode = &phy->mode_config.input_mode.vmode;
	u32 timing[] = {
		/* Horizontal timing */
		mode->htotal,
		vmode->hactive,
		vmode->hfront_porch,
		vmode->hback_porch,
		vmode->hsync_len,
		/* Vertical timing */
		vmode->vactive,
		vmode->vfront_porch,
		vmode->vback_porch,
		vmode->vsync_len,
	};

	DRM_DEBUG("LT8618: Set video output timing\n");
	drm_mode_debug_printmodeline(mode);
#if defined(CONFIG_VIDEOMODE_HELPERS)
	drm_display_mode_to_videomode(mode, vmode);
#endif
	for (i = 0; i < ARRAY_SIZE(video_timing_arr); i++) {
		video_timing_arr[i] = i % 2 ? timing[i / 2] & 0xFF :
						    (timing[i / 2] >> 8) & 0xFF;
	}

	regmap_bulk_write(phy->phy_regmap, 0x822C, video_timing_arr,
			  ARRAY_SIZE(video_timing_arr));

	return 0;
}

static int lt8618_afe_high(struct loonggpu_bridge_phy *phy)
{
	/* HDMI_TX_Phy */
	regmap_write(phy->phy_regmap, 0x8130, 0xEA);

	/* DC mode */
	regmap_write(phy->phy_regmap, 0x8131, 0x44);
	regmap_write(phy->phy_regmap, 0x8132, 0x4A);
	regmap_write(phy->phy_regmap, 0x8133, 0x0B);

	regmap_write(phy->phy_regmap, 0x8134, 0x00);
	regmap_write(phy->phy_regmap, 0x8135, 0x00);
	regmap_write(phy->phy_regmap, 0x8136, 0x00);
	regmap_write(phy->phy_regmap, 0x8137, 0x44);
	regmap_write(phy->phy_regmap, 0x813F, 0x0F);

	regmap_write(phy->phy_regmap, 0x8140, 0xA0);
	regmap_write(phy->phy_regmap, 0x8141, 0xA0);
	regmap_write(phy->phy_regmap, 0x8142, 0xA0);
	regmap_write(phy->phy_regmap, 0x8143, 0xA0);
	regmap_write(phy->phy_regmap, 0x8144, 0x0A);

	return 0;
}

static int lt8618_afe_set_tx(struct loonggpu_bridge_phy *phy, bool enable)
{
	if (enable)
		regmap_write(phy->phy_regmap, 0x8130, 0xEA);
	else
		regmap_write(phy->phy_regmap, 0x8130, 0x00);

	return 0;
}

static int lt8618_hdmi_output_mode(struct loonggpu_bridge_phy *phy, int mode)
{
	if ((enum hdmi_mode)mode)
		/* bit7 = 0 : DVI output; bit7 = 1: HDMI output */
		regmap_write(phy->phy_regmap, 0x82D6, 0x0E);
	else
		regmap_write(phy->phy_regmap, 0x82D6, 0x8E);

	return 0;
}

/* LT8618SXB supports YUV422, YUV444, RGB888
 * color space convert except YUV420
 */
static int lt8618_hdmi_csc(struct loonggpu_bridge_phy *phy)
{
	/* No csc */
	regmap_write(phy->phy_regmap, 0x82B9, 0x00);

	return 0;
}

static int lt8618_mode_set(struct loonggpu_bridge_phy *phy,
			   const struct drm_display_mode *mode,
			   const struct drm_display_mode *adj_mode)
{
	int vic;

	if (mode && phy->mode_config.input_mode.mode &&
		drm_mode_equal(mode, phy->mode_config.input_mode.mode))
		return 0;

	vic = drm_match_cea_mode(mode);
	phy->mode_config.input_mode.mode =
		drm_mode_duplicate(phy->bridge.dev, mode);

	lt8618_video_input_timing_check(phy);
	lt8618_video_output_cfg(phy);
	lt8618_video_output_timing(phy, mode);

	return 0;
}

static int lt8618_audio_enable(struct loonggpu_bridge_phy *phy)
{
	/* SPDIF 48KHz 32bit */
	regmap_write(phy->phy_regmap, 0x82d6, 0x8e);
	regmap_write(phy->phy_regmap, 0x82d7, 0x00);
	regmap_write(phy->phy_regmap, 0x8406, 0x0c);
	regmap_write(phy->phy_regmap, 0x8407, 0x10);
	regmap_write(phy->phy_regmap, 0x840f, 0x2b);
	regmap_write(phy->phy_regmap, 0x8434, 0xd4);

	regmap_write(phy->phy_regmap, 0x8435, (u8)(6144 / 0x10000));
	regmap_write(phy->phy_regmap, 0x8436, (u8)((6144 & 0x00ffff) / 0x1000));
	regmap_write(phy->phy_regmap, 0x8437, (u8)(6144 & 0x0000FF));
	regmap_write(phy->phy_regmap, 0x843c, 0x21);

	return 0;
}

static const struct bridge_phy_cfg_funcs lt8618_cfg_funcs = {
	.hw_reset = lt8618_hw_reset,
	.sw_enable = lt8618_sw_enable,
	.sw_reset = SIMPLE_REG_SEQ_FUNC(sw_reset),
	.video_input_cfg = lt8618_video_input_cfg,
	.video_output_cfg = lt8618_video_output_cfg,
	.video_output_timing = lt8618_video_output_timing,
	.hdmi_output_mode = lt8618_hdmi_output_mode,
	.hdmi_csc = lt8618_hdmi_csc,
	.afe_high = lt8618_afe_high,
	.afe_set_tx = lt8618_afe_set_tx,
	.mode_set = lt8618_mode_set,
	.hdmi_audio = lt8618_audio_enable,
};

/**
 * @section LT8618 INT and HPD functions
 */
static enum hpd_status lt8618_get_hpd_status(struct loonggpu_bridge_phy *phy)
{
	unsigned int val;

	regmap_read(phy->phy_regmap, LT8618_REG_LINK_STATUS, &val);
	if (test_bit(LINK_STATUS_OUTPUT_DC_POS, (unsigned long *)&val) ==
	    LINK_STATUS_STABLE) {
		return hpd_status_plug_on;
	}

	return hpd_status_plug_off;
}

static int lt8618_int_enable(struct loonggpu_bridge_phy *phy, enum int_type interrupt)
{
	return 0;
}

static int lt8618_int_disable(struct loonggpu_bridge_phy *phy, enum int_type interrupt)
{
	return 0;
}
static int lt8618_int_clear(struct loonggpu_bridge_phy *phy, enum int_type interrupt)
{
	return 0;
}

static irqreturn_t lt8618_irq_handler(int irq_num, void *dev)
{
	return 0;
}

static irqreturn_t lt8618_isr_thread(int irq_num, void *dev)
{
	return 0;
}

static struct bridge_phy_hpd_funcs lt8618_hpd_funcs = {
	.get_hpd_status = lt8618_get_hpd_status,
	.int_enable = lt8618_int_enable,
	.int_disable = lt8618_int_disable,
	.int_clear = lt8618_int_clear,
	.irq_handler = lt8618_irq_handler,
	.isr_thread = lt8618_isr_thread,
};

/**
 * @section LT8618 DDC functions
 */
static void __lt8618_ddc_cmd(struct loonggpu_bridge_phy *phy, enum lt8618_ddc_cmd cmd)
{
	bridge_phy_reg_update_bits(phy, LT8618_REG_DDC_CMD,
			   LT8618_REG_DDC_CMD_MSK, cmd);
}

static int lt8618_ddc_fifo_abort(struct loonggpu_bridge_phy *phy)
{
	__lt8618_ddc_cmd(phy, CMD_ABORT);
	return 0;
}

static int lt8618_ddc_fifo_clear(struct loonggpu_bridge_phy *phy)
{
	__lt8618_ddc_cmd(phy, CMD_RESET);
	return 0;
}

static u8 lt8618_get_fifo_data_count(struct loonggpu_bridge_phy *phy)
{
	u8 val;

	regmap_read(phy->phy_regmap, LT8618_REG_FIFO_STATUS,
		    (unsigned int *)&val);

	return val >> 2;
}

static int lt8618_ddc_fifo_fetch(struct loonggpu_bridge_phy *phy, u8 *buf, u8 block,
				 size_t len, size_t offset)
{
	u8 *pfifo;
	size_t pos;
	struct regmap *regmap;
	unsigned int i, retry;
	u8 ddc_status, fifo_status, fifo_data_count;
	bool ddc_idle, fifo_empty;
	u8 ddc_cfg[5];
	enum hpd_status hpd_status;

	pfifo = buf;
	regmap = phy->phy_regmap;
	pos = offset + 128 * block;

	if (len > LT8618_FIFO_MAX_LENGTH || pos > EDID_LENGTH * 2) {
		DRM_ERROR("[EDID]: Failed to request DDC FIFO!\n");
		DRM_ERROR("Invalid len or pos {%ldB,%ld}\n ", len, pos);
		return -EOVERFLOW;
	}

	hpd_status = lt8618_get_hpd_status(phy);
	if (hpd_status != hpd_status_plug_on) {
		DRM_ERROR("[EDID]: Failed to fetch FIFO, connector plug off\n");
		return -ENODEV;
	}

	ddc_cfg[0] = 0x0A, ddc_cfg[1] = 0xC9, ddc_cfg[2] = 0xA0;
	ddc_cfg[3] = pos, ddc_cfg[4] = len;
	regmap_bulk_write(regmap, LT8618_REG_DDC_BASE, ddc_cfg, ARRAY_SIZE(ddc_cfg));

	regmap_write(regmap, LT8618_REG_DDC_CMD, 0x36);
	regmap_write(regmap, LT8618_REG_DDC_CMD, 0x34);
	regmap_write(regmap, LT8618_REG_DDC_CMD, 0x37);

	ddc_idle = fifo_empty = 0;
	for (retry = 0; retry < 5; retry++) {
		schedule_timeout(msecs_to_jiffies(1));
		regmap_read(regmap, LT8618_REG_DDC_STATUS,
			    (unsigned int *)&ddc_status);
		ddc_idle = test_bit(LT8618_REG_DDC_BUS_DONE_POS,
				    (unsigned long *)&ddc_status);
		if (ddc_idle)
			break;
		DRM_DEBUG("FIFO fetch not complete, status=%x\n", ddc_status);
	}

	for (i = 0; i < len; i++) {
		regmap_read(regmap, LT8618_REG_FIFO_CONTENT,
			    (unsigned int *)pfifo);
		if (DEBUG_MSEEAGE) {
			fifo_data_count = lt8618_get_fifo_data_count(phy);
			DRM_DEBUG("FIFO[%2d]=%02x, remaining=%2d\n", i, *pfifo,
				  fifo_data_count);
			regmap_read(regmap, LT8618_REG_FIFO_STATUS,
				    (unsigned int *)&fifo_status);
			if (test_bit(LT8618_REG_FIFO_EMPTY_POS,
			    (unsigned long *)&fifo_status) == FIFO_EMPTY) {
				DRM_DEBUG("FIFO is empty, read back %dB\n", i);
				break;
			}
		}
		pfifo++;
	}

	return 0;
}

static int lt8618_get_edid_block(void *data, u8 *buf, unsigned int block,
				 size_t len)
{
	int ret;
	u8 *pfifo, *pbuf;
	size_t len_max, seek;
	unsigned int i, batch_num;
	struct loonggpu_bridge_phy *phy;
	u8 edid_buf[128];
	u8 fifo_buf[32];

	phy = data;
	pfifo = fifo_buf;
	pbuf = edid_buf;
	batch_num = len / LT8618_FIFO_MAX_LENGTH;
	DRM_DEBUG("[EDID]: Request EDID block-%d %ldB, divided %d batches\n",
		block, len, batch_num);
	if (len > EDID_LENGTH) {
		ret = -EOVERFLOW;
		DRM_ERROR("[EDID]: Failed to request EDID %ldB!\n", len);
		return ret;
	}

	len_max = LT8618_FIFO_MAX_LENGTH;
	for (i = 0; i < batch_num; i++) {
		seek = i * LT8618_FIFO_MAX_LENGTH;
		ret = lt8618_ddc_fifo_fetch(phy, pfifo, block, len_max, seek);
		if (ret) {
			DRM_DEV_ERROR(to_dev(phy),
				"[EDID]: Failed to fetched FIFO batches[%d/%d], ret=%d\n",
				i + 1, batch_num, ret);

			return -EIO;
		}
		DRM_DEBUG("[EDID]: Fetched EDID block-%d[%d/%d] %ldByete\n",
			  block, i + 1, batch_num, len_max);
		memcpy(pbuf, pfifo, len_max);
		pbuf += len_max;
	}
	memcpy(buf, edid_buf, len);

	return 0;
}

static int lt8618_get_modes(struct loonggpu_bridge_phy *phy,
			    struct drm_connector *connector)
{
	const struct edid *edid;
	unsigned int count;

	edid = lg_drm_do_get_edid(connector, lt8618_get_edid_block, phy);
	if (edid) {
		drm_connector_update_edid_property(connector, edid);
		count = drm_add_edid_modes(connector, (struct edid *)edid);
		kfree(edid);
	} else {
		count = drm_add_modes_noedid(connector, 1920, 1080);
		drm_set_preferred_mode(connector, 1024, 768);
		DRM_DEBUG("Update default edid method\n");
	}

	return count;
}

static struct bridge_phy_ddc_funcs lt8618_ddc_funcs = {
	.ddc_fifo_fetch = lt8618_ddc_fifo_fetch,
	.ddc_fifo_abort = lt8618_ddc_fifo_abort,
	.ddc_fifo_clear = lt8618_ddc_fifo_clear,
	.get_edid_block = lt8618_get_edid_block,
	.get_modes = lt8618_get_modes,
};

/**
 *@section LT8618 HDMI AUX functions
 *
 */
static int lt8618_set_gcp_avmute(struct loonggpu_bridge_phy *phy, bool enable,
				 bool blue_screen)
{
	return 0;
}

static int lt8618_set_avi_infoframe(struct loonggpu_bridge_phy *phy,
				    const struct drm_display_mode *mode)
{
	return 0;
}

static struct bridge_phy_hdmi_aux_funcs lt8618_hdmi_aux_funcs = {
	.set_gcp_avmute = lt8618_set_gcp_avmute,
	.set_avi_infoframe = lt8618_set_avi_infoframe,
};

/**
 * @section Device debugfs attribute
 */
static bool lt8618_chip_id_verify(struct loonggpu_bridge_phy *phy, char *str);

static ssize_t lt8618_test_case_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	char name[32];
	struct i2c_client *i2c;
	struct loonggpu_bridge_phy *phy;

	i2c = to_i2c_client(dev);
	phy = (struct loonggpu_bridge_phy *)i2c_get_clientdata(i2c);

	lt8618_chip_id_verify(phy, name);

	return 0;
}

static ssize_t lt8618_test_case_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	int i;
	u8 *pfifo;
	u8 version_val[3];
	u8 edid_fifo[32];
	unsigned int reg, val;
	struct i2c_client *i2c;
	struct loonggpu_bridge_phy *phy;
	u8 edid[EDID_LENGTH];
	enum hpd_status hpd;
	u8 fifo_buf[32];

	i2c = to_i2c_client(dev);
	phy = (struct loonggpu_bridge_phy *)i2c_get_clientdata(i2c);

	if (sysfs_streq(buf, "0")) {
		reg = 0x80FF, val = 0x80;
		goto write;
	}

	if (sysfs_streq(buf, "1")) {
		regmap_bulk_read(phy->phy_regmap, LT8618_REG_CHIP_VERSION_BASE,
				 version_val, CHIP_VERSION_LEN);
		return count;
	}

	if (sysfs_streq(buf, "2"))
		lt8618_get_edid_block(phy, edid, 0, EDID_LENGTH);

	if (sysfs_streq(buf, "3")) {
		regmap_read(phy->phy_regmap, LT8618_REG_LINK_STATUS, &val);
		if (test_bit(LINK_STATUS_OUTPUT_DC_POS,
			     (unsigned long *)&val) == LINK_STATUS_STABLE) {
			DRM_DEBUG("LT8618 hpd status: Connected\n");
			hpd = hpd_status_plug_on;
		} else
			hpd = hpd_status_plug_off;
		pr_err("hpd_status=%d\n", hpd);
	}

	if (sysfs_streq(buf, "4")) {
		pfifo = fifo_buf;
		lt8618_ddc_fifo_fetch(phy, pfifo, 0, LT8618_FIFO_MAX_LENGTH, 0);
		for (i = 0; i < 32; i++)
			pr_err("edid[%d]=%x\n", i, fifo_buf[i]);
	}

	if (sysfs_streq(buf, "8")) {
		regmap_noinc_read(phy->phy_regmap, 0x8583, edid_fifo,
				  ARRAY_SIZE(edid_fifo));
		for (i = 0; i < ARRAY_SIZE(edid_fifo); i++)
			pr_err("edid fifo[%d]=%x\n", i, edid_fifo[i]);
		return count;
	}

	if (sysfs_streq(buf, "9")) {
		pfifo = edid_fifo;
		for (i = 0; i < 31; i++) {
			regmap_read(phy->phy_regmap, 0x8583,
				    (unsigned int *)pfifo);
			pr_err("edid fifo[%d]=%x\n", i, *pfifo++);
		}
		return count;
	}

	return count;
write:
	regmap_write(phy->phy_regmap, reg, val);
	return count;
}

static bool lt8618_chip_id_verify(struct loonggpu_bridge_phy *phy, char *str)
{
	u8 version_val[3];
	struct lt8618_device *lt8618_dev;

	regmap_bulk_read(phy->phy_regmap, LT8618_REG_CHIP_VERSION_BASE,
			 version_val, CHIP_VERSION_LEN);
	if (version_val[0] != 0x17 || version_val[1] != 0x02) {
		DRM_ERROR("Invalid lt8618 chip version {%02x,%02x,%02x}\n",
			  version_val[0], version_val[1], version_val[2]);
		strcpy("Unknown", str);
		return false;
	}

	phy->chip_version = version_val[2];
	lg_strncpy(str, version_val, ARRAY_SIZE(version_val));

	lt8618_dev = phy->priv;
	if (version_val[2] == 0xE1)
		lt8618_dev->ver = LT8618_VER_U2;
	else if (version_val[2] == 0xE2)
		lt8618_dev->ver = LT8618_VER_U3;
	else
		lt8618_dev->ver = LT8618_VER_Unknown;

	DRM_DEBUG("Get chip version: LT8618_VER_U%d\n", lt8618_dev->ver);

	return true;
}

static DEVICE_ATTR_RW(lt8618_test_case);

static int lt8618_debugfs_init(struct loonggpu_bridge_phy *phy)
{
	int ret;

	ret = device_create_file(to_dev(phy), &dev_attr_lt8618_test_case);

	return 0;
}

static struct bridge_phy_misc_funcs lt8618_misc_funcs = {
	.chip_id_verify = lt8618_chip_id_verify,
	.debugfs_init = lt8618_debugfs_init,
};
/**
 * @section LT8618 driver initialize
 */
static const struct regmap_access_table lt8618_read_table = {
	.yes_ranges = lt8618_rw_regs_range,
	.n_yes_ranges = ARRAY_SIZE(lt8618_rw_regs_range),
};

static const struct regmap_access_table lt8618_write_table = {
	.yes_ranges = lt8618_rw_regs_range,
	.n_yes_ranges = ARRAY_SIZE(lt8618_rw_regs_range),
	.no_ranges = lt8618_ro_regs_range,
	.n_no_ranges = ARRAY_SIZE(lt8618_ro_regs_range),
};

static const struct regmap_access_table lt8618_volatile_table = {
	.yes_ranges = lt8618_rw_regs_range,
	.n_yes_ranges = ARRAY_SIZE(lt8618_rw_regs_range),
};

static const struct regmap_range_cfg lt8618_regmap_range_cfg[] = {
	{
		.name = "lt8618_registers",
		.range_min = LT8618_REG_START,
		.range_max = LT8618_REG_END,
		.window_start = LT8618_REG_START,
		.window_len = LT8618_REG_PAGE,
		.selector_reg = LT8618_REG_PAGE_SELECT,
		.selector_mask = 0xFF,
		.selector_shift = 0,
	},
};

static const struct regmap_config lt8618_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.reg_stride = 1,
	.max_register = LT8618_REG_END,
	.ranges = lt8618_regmap_range_cfg,
	.num_ranges = ARRAY_SIZE(lt8618_regmap_range_cfg),

	.fast_io = false,
	.cache_type = REGCACHE_RBTREE,

	.volatile_reg = lt8618_register_volatile,
	.readable_reg = lt8618_register_readable,
	.rd_table = &lt8618_read_table,
	.wr_table = &lt8618_write_table,
};

int bridge_phy_lt8618_init(struct loonggpu_dc_bridge *dc_bridge)
{
	struct loonggpu_bridge_phy *lt8618_phy;
	struct lt8618_device *lt8618_dev;

	lt8618_phy = bridge_phy_alloc(dc_bridge);

	lt8618_dev = kmalloc(sizeof(struct lt8618_device), GFP_KERNEL);
	if (IS_ERR(lt8618_dev))
		return PTR_ERR(lt8618_dev);

	lt8618_phy->priv = lt8618_dev;

	lt8618_phy->cfg_funcs = &lt8618_cfg_funcs;
	lt8618_phy->regmap_cfg = &lt8618_regmap_config;
	lt8618_phy->misc_funcs = &lt8618_misc_funcs;
	lt8618_phy->ddc_funcs = &lt8618_ddc_funcs;
	lt8618_phy->hpd_funcs = &lt8618_hpd_funcs;
	lt8618_phy->hdmi_aux_funcs = &lt8618_hdmi_aux_funcs;
	lt8618_phy->is_ext_encoder = true;

	return bridge_phy_init(lt8618_phy);
}

int bridge_phy_lt8618_remove(struct loonggpu_bridge_phy *phy)
{
	return 0;
}
