// SPDX-License-Identifier: GPL-2.0-or-later

#include "it66121_drv.h"
#include "it66121_regs.h"
#include "loonggpu_drv.h"
#include "loonggpu_dc_vbios.h"

#define DEBUG_MSEEAGE 0
#define CHIP_PREFIX it66121

#define it66121_hpd_status_to_str(status)\
	(status == hpd_status_plug_on ? "Plug on" :\
	(status == hpd_status_plug_off ? "Plug off" :\
	 "Unknown"))

/**
 * @section IT66121 helper functions
 */

static int it66121_int_enable(struct loonggpu_bridge_phy *phy, enum int_type interrupt);
static int it66121_int_disable(struct loonggpu_bridge_phy *phy, enum int_type interrupt);

DEFINE_SIMPLE_REG_SEQ_FUNC(reg_init)
DEFINE_SIMPLE_REG_SEQ_FUNC(power_on)
DEFINE_SIMPLE_REG_SEQ_FUNC(afe_high)
DEFINE_SIMPLE_REG_SEQ_FUNC(afe_low)
DEFINE_SIMPLE_REG_SEQ_FUNC(hdmi)

static int it66121_hdmi_output_mode(struct loonggpu_bridge_phy *phy, int mode)
{
	if ((enum hdmi_mode)mode == HDMI_MODE_NORMAL) {
		SIMPLE_REG_SEQ_FUNC(hdmi)(phy);
		SIMPLE_REG_SEQ_FUNC(power_on)(phy);
	}

	return 0;
}

static int it66121_mode_set(struct loonggpu_bridge_phy *phy,
			    const struct drm_display_mode *mode,
			    const struct drm_display_mode *adj_mode)
{
	return 0;
}

static int it66121_audio_enable(struct loonggpu_bridge_phy *phy)
{
	bridge_phy_reg_mask_seq(phy, it66121_audio_seq,
			ARRAY_SIZE(it66121_audio_seq));

	return 0;
}

static int it66121_sw_enable(struct loonggpu_bridge_phy *phy)
{
	/* Enable hdcp */
	bridge_phy_reg_mask_seq(phy, it66121_hdcp_seq,
			ARRAY_SIZE(it66121_hdcp_seq));

	/* Enable hpd */
	if (phy->res->hotplug == IRQ) {
		it66121_int_enable(phy, interrupt_hpd);
		DRM_DEV_DEBUG(to_dev(phy), "Enable it66121 HPD interrupt.\n");
	}
	return 0;
}

static int it66121_sw_reset(struct loonggpu_bridge_phy *phy)
{
	/*  hdcp reset */
	bridge_phy_reg_update_bits(phy, IT66121_REG_RESET,
			IT66121_REG_RESET_HDCP_MSK, RESET_HDCP);
	return 0;
}

static const struct bridge_phy_cfg_funcs it66121_cfg_funcs = {
	.sw_reset = it66121_sw_reset,
	.sw_enable = it66121_sw_enable,
	.mode_set = it66121_mode_set,
	.reg_init = SIMPLE_REG_SEQ_FUNC(reg_init),
	.afe_high = SIMPLE_REG_SEQ_FUNC(afe_high),
	.afe_low = SIMPLE_REG_SEQ_FUNC(afe_low),
	.hdmi_output_mode = it66121_hdmi_output_mode,
	.hdmi_audio = it66121_audio_enable,
};

static bool it66121_chip_id_verify(struct loonggpu_bridge_phy *phy, char *str)
{
	u8 buf[4];

	struct regmap *regmap = phy->phy_regmap;

	regmap_raw_read(regmap, IT66121_REG_VENDOR_ID_BASE, &buf,
			ARRAY_SIZE(buf));

	if (buf[0] != 'T' || buf[1] != 'I') {
		DRM_DEV_ERROR(to_dev(phy),
			      "Failed to verify IT66121 chip id\n");
		return false;
	}

	DRM_DEV_DEBUG(to_dev(phy), "PHY vendor: %c%c\n", buf[1], buf[0]);
	lg_strncpy(buf, str, ARRAY_SIZE(buf));

	return true;
}

static int it66121_debugfs_init(struct loonggpu_bridge_phy *phy);

static struct bridge_phy_misc_funcs it66121_misc_funcs = {
	.chip_id_verify = it66121_chip_id_verify,
	.debugfs_init = it66121_debugfs_init,
};

/**
 * @section IT66121 INT and HPD functions
 */
static enum hpd_status it66121_get_hpd_status(struct loonggpu_bridge_phy *phy)
{
	unsigned int val;

	regmap_read(phy->phy_regmap, IT66121_REG_SYS_STATUS, &val);
	if (test_bit(SYS_STATUS_HPD_POS, (unsigned long *)&val) ==
	    SYS_HPD_PLUG_ON) {
		return hpd_status_plug_on;
	}

	return hpd_status_plug_off;
}

static int it66121_int_enable(struct loonggpu_bridge_phy *phy, enum int_type interrupt)
{
	switch (interrupt) {
	case interrupt_hpd:
		bridge_phy_reg_update_bits(phy, IT66121_REG_INT_MASK(INT_MASK0),
				INT_MASK_HPD_MSK, INT_MASK_HPD_ENABLE);
		break;
	case interrupt_all:
		regmap_multi_reg_write(phy->phy_regmap, it66121_int_en_all_seq,
				ARRAY_SIZE(it66121_int_en_all_seq));
		break;
	default:
		DRM_DEV_ERROR(to_dev(phy), "Invalid interrupt type");
		return -EINVAL;
	}

	return 0;
}

static int it66121_int_disable(struct loonggpu_bridge_phy *phy, enum int_type interrupt)
{
	switch (interrupt) {
	case interrupt_hpd:
		bridge_phy_reg_update_bits(phy, IT66121_REG_INT_MASK(INT_MASK0),
				INT_MASK_HPD_MSK, INT_MASK_HPD_DISABLE);
		break;
	case interrupt_all:
		regmap_multi_reg_write(phy->phy_regmap, it66121_int_dis_all_seq,
				ARRAY_SIZE(it66121_int_dis_all_seq));
		break;
	default:
		DRM_DEV_ERROR(to_dev(phy), "Invalid interrupt type");
		return -EINVAL;
	}

	return 0;
}

static int it66121_clear_int(struct loonggpu_bridge_phy *phy)
{
	unsigned int val;

	regmap_read(phy->phy_regmap, IT66121_REG_SYS_STATUS, &val);
	bridge_phy_reg_update_bits(phy, IT66121_REG_SYS_STATUS,
			SYS_STATUS_CLEAR_INT_MSK, SYS_CLEAR_INT);

	regmap_read(phy->phy_regmap, IT66121_REG_SYS_STATUS, &val);
	DRM_DEV_DEBUG(to_dev(phy), "Do clear INT done [0E]=%x\n", val);
	return 0;
}

static int it66121_int_clear(struct loonggpu_bridge_phy *phy, enum int_type interrupt)
{
	switch (interrupt) {
	case interrupt_hpd:
		bridge_phy_reg_update_bits(phy,
				IT66121_REG_INT_CLEAR(INT_CLEAR0),
				INT_CLEAR_HPD, 1U);
		it66121_clear_int(phy);
		break;
	case interrupt_all:
		regmap_write(phy->phy_regmap, IT66121_REG_INT_CLEAR(INT_CLEAR0),
				INT_CLEAR0_MSK);
		regmap_write(phy->phy_regmap, IT66121_REG_INT_CLEAR(INT_CLEAR1),
				INT_CLEAR1_MSK);
		it66121_clear_int(phy);
		break;
	default:
		DRM_DEV_ERROR(to_dev(phy), "Invalid interrupt type");
		return -EINVAL;
	}
	return 0;
}

static irqreturn_t it66121_isr_thread(int irq_num, void *dev)
{
	int ret;
	u8 irq[3];
	unsigned int sys, clk;
	bool hpd_event, hpd_status, interrupt_status;
	struct regmap *regmap;
	struct loonggpu_bridge_phy *phy;

	phy = (struct loonggpu_bridge_phy *)dev;
	regmap = phy->phy_regmap;
	DRM_DEV_DEBUG(to_dev(phy), "IT66121 isr thread\n");

	ret = regmap_read(regmap, IT66121_REG_SYS_CLK, &clk);
	if (ret < 0)
		goto error_io;
	ret = regmap_read(regmap, IT66121_REG_SYS_STATUS, &sys);
	if (ret < 0)
		goto error_io;
	interrupt_status =
		test_bit(SYS_STATUS_INT_ACTIVE_POS, (unsigned long *)&sys);
	if (interrupt_status != SYS_INT_ACTIVE) {
		DRM_DEV_DEBUG(to_dev(phy), "Not IT66121 interrupt [0E]=%x\n",
			      sys);

		return IRQ_NONE;
	}
	DRM_DEV_DEBUG(to_dev(phy),
		      "IT66121 interrupt active: [sys]=%x,[clk]=%x\n", sys,
		      clk);

	regmap_bulk_read(regmap, IT66121_REG_INT_STATUS_BASE, irq,
			 INT_STATUS_LEN);
	DRM_DEV_DEBUG(to_dev(phy), "IT66121 interrupt status: {%x,%x,%x}\n",
		      irq[0], irq[1], irq[2]);

	hpd_event = test_bit(INT_HPD_EVENT_POS, (unsigned long *)&irq[0]);
	hpd_status = test_bit(SYS_STATUS_HPD_POS, (unsigned long *)&sys);

	if (hpd_event == INT_HPD_EVENT) {
		DRM_DEV_INFO(to_dev(phy),
			     "IT66121 HPD event [CONNECTOR:%d %s %s]\n",
			     phy->connector->base.id, phy->connector->name,
			     it66121_hpd_status_to_str(hpd_status));

		it66121_int_clear(phy, interrupt_hpd);
		it66121_int_clear(phy, interrupt_all);
		//phy->hpd_status = hpd_status;
		drm_kms_helper_hotplug_event(phy->connector->dev);
	} else {
		DRM_DEV_DEBUG(to_dev(phy), "Not HPD event\n");
		it66121_int_clear(phy, interrupt_all);
	}

	return IRQ_HANDLED;
error_io:
	DRM_DEV_ERROR(to_dev(phy), "Failed to read IT66121 registers\n");

	return IRQ_NONE;
}

static struct bridge_phy_hpd_funcs it66121_hpd_funcs = {
	.get_hpd_status = it66121_get_hpd_status,
	.int_enable = it66121_int_enable,
	.int_disable = it66121_int_disable,
	.int_clear = it66121_int_clear,
	.isr_thread = it66121_isr_thread,
};

/**
 * @section IT66121 DDC functions
 */
static int it66121_ddc_fifo_abort(struct loonggpu_bridge_phy *phy)
{
	bridge_phy_reg_update_bits(phy, IT66121_REG_DDC_CMD,
			IT66121_REG_DDC_CMD_MSK, CMD_ABORT);
	return 0;
}

static inline int it66121_wait_ddc_ready(struct loonggpu_bridge_phy *phy)
{
	int ret, val;
	u32 busy = IT66121_DDC_STATUS_NOACK | IT66121_DDC_STATUS_WAIT_BUS |
		   IT66121_DDC_STATUS_ARBI_LOSE;

	ret = regmap_read_poll_timeout(phy->phy_regmap, IT66121_REG_DDC_STATUS,
				       val, true, IT66121_EDID_SLEEP_US,
				       IT66121_EDID_TIMEOUT_US);
	if (ret)
		return ret;

	if (val & busy)
		return -EAGAIN;

	return 0;
}

static int it66121_get_edid_block(void *data, u8 *buf, unsigned int block,
				  size_t len)
{
	struct loonggpu_bridge_phy *phy;
	struct regmap *regmap;
	int remain = len;
	int offset = 0;
	int cnt, val, ret;

	offset = (block % 2) * len;
	phy = data;
	regmap = phy->phy_regmap;

	/* clear ddc fifo */
	regmap_write(regmap, IT66121_REG_DDC_CTRL, IT66121_MASTER_SEL_HOST);
	regmap_write(regmap, IT66121_REG_DDC_CMD, IT66121_DDC_COMMAND_FIFO_CLR);

	while (remain > 0) {
		cnt = (remain > IT66121_EDID_FIFO_SIZE) ?
				IT66121_EDID_FIFO_SIZE : remain;

		regmap_write(regmap, IT66121_REG_DDC_CTRL, IT66121_MASTER_SEL_HOST);
		regmap_write(regmap, IT66121_REG_DDC_CMD, IT66121_DDC_COMMAND_FIFO_CLR);

		it66121_wait_ddc_ready(phy);

		regmap_write(regmap, IT66121_REG_DDC_CTRL, IT66121_MASTER_SEL_HOST);
		regmap_write(regmap, IT66121_REG_DDC_BASE, IT66121_RX_EDID);
		regmap_write(regmap, IT66121_REG_DDC_OFFSET, offset);
		regmap_write(regmap, IT66121_REG_DDC_FIFO_SIZE, cnt);
		regmap_write(regmap, IT66121_REG_DDC_EDID_BLOCK, block);

		regmap_write(regmap, IT66121_REG_DDC_CMD, IT66121_DDC_COMMAND_EDID_READ);

		offset += cnt;
		remain -= cnt;

		msleep(20);
		it66121_wait_ddc_ready(phy);
		do {
			ret = regmap_read(regmap, IT66121_REG_FIFO_CONTENT, &val);
			*(buf++) = val;
			cnt--;
		} while (cnt > 0);
	}

	return 0;
}

static int it66121_get_modes(struct loonggpu_bridge_phy *phy,
			     struct drm_connector *connector)
{
	const struct edid *edid;
	unsigned int count = 0;
	edid = lg_drm_do_get_edid(connector, it66121_get_edid_block, phy);
	if (edid) {
		drm_connector_update_edid_property(connector, edid);
		count = drm_add_edid_modes(connector, (struct edid *)edid);
	} else {
		count = drm_add_modes_noedid(connector, 1920, 1080);
		DRM_DEV_INFO(to_dev(phy), "Update default edid method 1080p\n");
	}
	kfree(edid);
	return count;
}

static struct bridge_phy_ddc_funcs it66121_ddc_funcs = {
	.ddc_fifo_abort = it66121_ddc_fifo_abort,
	.get_edid_block = it66121_get_edid_block,
	.get_modes = it66121_get_modes,
};

/**
 * @section IT66121 AVI Infoframe functions
 */
static int it66121_set_avi_infoframe(struct loonggpu_bridge_phy *phy,
				     const struct drm_display_mode *mode)
{
	u8 *ptr, buf[HDMI_INFOFRAME_SIZE(AVI)];
	struct regmap *regmap;
	union hdmi_infoframe *hdmi_frame;
	struct hdmi_avi_infoframe *avi_frame;

	regmap = phy->phy_regmap;
	hdmi_frame = &phy->mode_config.hdmi_frame;
	avi_frame = &hdmi_frame->avi;
	hdmi_avi_infoframe_init(avi_frame);
	lg_drm_hdmi_avi_infoframe_from_display_mode(avi_frame, phy->connector, mode);
	hdmi_avi_infoframe_pack(avi_frame, buf, sizeof(buf));
	ptr = buf + HDMI_INFOFRAME_HEADER_SIZE;

#if (DEBUG_MSEEAGE)
	hdmi_infoframe_log(KERN_DEBUG, to_dev(phy), hdmi_frame);
	DRM_DEBUG("Set AVI reg block-0 0x%x size %d\n", AVI_BLOCK_0_BASE,
		  AVI_BLOCK_0_LENGTH);
	DRM_DEBUG("%x %x %x %x %x", ptr[0], ptr[1], ptr[2], ptr[3], ptr[4]);
	DRM_DEBUG("Set AVI reg checksum 0x%x=%x\n", AVI_BLOCK_SUM, buf[3]);
#endif

	regmap_bulk_write(regmap, AVI_BLOCK_0_BASE, ptr, AVI_BLOCK_0_LENGTH);
	regmap_write(regmap, AVI_BLOCK_SUM, buf[3]);
	ptr += AVI_BLOCK_0_LENGTH;
	regmap_bulk_write(regmap, AVI_BLOCK_1_BASE, ptr, AVI_BLOCK_1_LENGTH);

#if (DEBUG_MSEEAGE)
	DRM_DEBUG("Set AVI reg block-1 0x%x size %d\n", AVI_BLOCK_1_BASE,
		  AVI_BLOCK_1_LENGTH);
	DRM_DEBUG("%x %x %x %x %x %x %x %x\n", ptr[0], ptr[1], ptr[2], ptr[3],
		  ptr[4], ptr[5], ptr[6], ptr[7]);
#endif

	return 0;
}

static struct bridge_phy_hdmi_aux_funcs it66121_hdmi_aux_funcs = {
	.set_avi_infoframe = it66121_set_avi_infoframe,
};

/**
 * @section IT66121 Regmap amd device attribute
 */
static const struct regmap_access_table it66121_read_table = {
	.yes_ranges = it66121_rw_regs_range,
	.n_yes_ranges = ARRAY_SIZE(it66121_rw_regs_range),
};

static const struct regmap_access_table it66121_write_table = {
	.yes_ranges = it66121_rw_regs_range,
	.n_yes_ranges = ARRAY_SIZE(it66121_rw_regs_range),
	.no_ranges = it66121_ro_regs_range,
	.n_no_ranges = ARRAY_SIZE(it66121_ro_regs_range),
};

static const struct regmap_access_table it66121_volatile_table = {
	.yes_ranges = it66121_rw_regs_range,
	.n_yes_ranges = ARRAY_SIZE(it66121_rw_regs_range),
};

static const struct regmap_range_cfg phy_regmap_range_cfg[] = {
	{
		.name = "it66121_registers",
		.range_min = IT66121_REG_START,
		.range_max = IT66121_REG_MAX,
		.window_start = IT66121_REG_START,
		.window_len = IT66121_REG_END,
		.selector_reg = 0x0f,
		.selector_mask = 0x0f,
		.selector_shift = 0,
	},
};

static const struct regmap_config phy_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.reg_stride = 1,
	.max_register = IT66121_REG_MAX,
	.ranges = phy_regmap_range_cfg,
	.num_ranges = ARRAY_SIZE(phy_regmap_range_cfg),

	.fast_io = false,
	.cache_type = REGCACHE_RBTREE,

	.reg_defaults_raw = it66121_reg_defaults_raw,
	.num_reg_defaults_raw = ARRAY_SIZE(it66121_reg_defaults_raw),

	.volatile_reg = it66121_register_volatile,
	.rd_table = &it66121_read_table,
	.wr_table = &it66121_write_table,
	.volatile_table = &it66121_volatile_table,
};

static int it66121_debugfs_init(struct loonggpu_bridge_phy *phy)
{
	return 0;
}

const struct bridge_phy_cfg_funcs *cfg_funcs;

int bridge_phy_it66121_init(struct loonggpu_dc_bridge *dc_bridge)
{
	int ret;
	struct loonggpu_bridge_phy *it66121_phy;

	it66121_phy = bridge_phy_alloc(dc_bridge);

	it66121_phy->cfg_funcs = &it66121_cfg_funcs;
	it66121_phy->regmap_cfg = &phy_regmap_config;
	it66121_phy->misc_funcs = &it66121_misc_funcs;
	it66121_phy->hpd_funcs = &it66121_hpd_funcs;
	it66121_phy->ddc_funcs = &it66121_ddc_funcs;
	it66121_phy->hdmi_aux_funcs = &it66121_hdmi_aux_funcs;
	it66121_phy->is_ext_encoder = true;

        ret = bridge_phy_init(it66121_phy);
	return ret;
}
