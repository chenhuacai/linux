// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2021 Loongson Technology Co., Ltd.
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include "ms7210_regs.h"

#define CHIP_PREFIX ms7210
#define MS7210_CHIP_ID1 0x20

/**
 * @section MS7210 helper functions
 */
DEFINE_SIMPLE_REG_SEQ_FUNC(afe_high)
DEFINE_SIMPLE_REG_SEQ_FUNC(afe_low)

static int ms7210_int_enable(struct loonggpu_bridge_phy *phy, enum int_type irq_type);
static int ms7210_int_disable(struct loonggpu_bridge_phy *phy, enum int_type irq_type);

static int ms7210_hw_reset(struct loonggpu_bridge_phy *phy)
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
		goto error_invalid;
	sprintf(pin_name, "%s-reset", chip_name);
	ret = gpio_request(reset_gpio, pin_name);
	if (!ret)
		goto error_nodev;

	gpiod = gpio_to_desc(reset_gpio);
	gpiod_set_value_cansleep(gpiod, 0);
	schedule_timeout(msecs_to_jiffies(5));
	gpiod_set_value_cansleep(gpiod, 1);
	schedule_timeout(msecs_to_jiffies(5));

	return 0;

error_nodev:
	DRM_DEV_ERROR(to_dev(phy), "Failed to request reset gpio pin %d\n",
		      reset_gpio);
error_invalid:
	DRM_DEV_ERROR(to_dev(phy), "Invalid reset gpio pin number %d\n",
		      reset_gpio);

	return -ENODEV;
}

static int ms7210_reg_init(struct loonggpu_bridge_phy *phy)
{
	regmap_multi_reg_write(phy->phy_regmap, ms7210_reg_init_seq,
			       ARRAY_SIZE(ms7210_reg_init_seq));

	regmap_multi_reg_write(phy->phy_regmap, ms7210_audio_init_seq,
			       ARRAY_SIZE(ms7210_audio_init_seq));

	return 0;
}

static int ms7210_sw_enable(struct loonggpu_bridge_phy *phy)
{
	ms7210_int_enable(phy, interrupt_hpd);
	return 0;
}

static int ms7210_mode_set(struct loonggpu_bridge_phy *phy,
			   const struct drm_display_mode *mode,
			   const struct drm_display_mode *adj_mode)
{
	int (*afe_set)(struct loonggpu_bridge_phy *phy);

	if (mode->clock > 100000)
		afe_set = phy->cfg_funcs->afe_high;
	else
		afe_set = phy->cfg_funcs->afe_low;

	afe_set(phy);

	return 0;
}

static const struct bridge_phy_cfg_funcs ms7210_cfg_funcs = {
	.hw_reset = ms7210_hw_reset,
	.reg_init = ms7210_reg_init,
	.sw_enable = ms7210_sw_enable,
	.mode_set = ms7210_mode_set,
	.afe_high = SIMPLE_REG_SEQ_FUNC(afe_high),
	.afe_low = SIMPLE_REG_SEQ_FUNC(afe_low),
};

/**
 * @section ms7210 INT and HPD functions
 */
static enum hpd_status ms7210_get_hpd_status(struct loonggpu_bridge_phy *phy)
{
	unsigned int val;

	regmap_read(phy->phy_regmap, MS7210_REG_HPD_STATUS, &val);
	if (test_bit(LINK_STATUS_OUTPUT_DC_POS, (unsigned long *)&val) ==
	    LINK_STATUS_STABLE) {
		return hpd_status_plug_on;
	}

	return hpd_status_plug_off;
}

static int ms7210_int_enable(struct loonggpu_bridge_phy *phy, enum int_type irq_type)
{
	if (irq_type == interrupt_hpd) {
		/* enable ms7210 hpd irq */
		bridge_phy_reg_update_bits(phy, MS7210_REG_INT_CTRL,
				INT_CTRL_HPD_MSK, INT_HPD_ENABLE);
		bridge_phy_reg_update_bits(phy, MS7210_REG_INT_STATUS,
				INT_STATUS_HPD_MSK, INT_HPD_PLUG_ON);
	}
	return 0;
}

static int ms7210_int_disable(struct loonggpu_bridge_phy *phy, enum int_type irq_type)
{
	if (irq_type == interrupt_hpd) {
		/* disable ms7210 hpd irq */
		bridge_phy_reg_update_bits(phy, MS7210_REG_INT_CTRL,
				INT_CTRL_HPD_MSK, INT_HPD_DISABLE);
		bridge_phy_reg_update_bits(phy, MS7210_REG_INT_STATUS,
				INT_STATUS_HPD_MSK, INT_HPD_PLUG_OFF);
	}
	return 0;
}

static int ms7210_int_clear(struct loonggpu_bridge_phy *phy, enum int_type irq_type)
{
	if (irq_type == interrupt_hpd)
		/* clear ms7210 hpd irq */
		bridge_phy_reg_update_bits(phy, MS7210_REG_INT_CLEAR,
				INT_CLEAR_HPD_MSK, INT_HPD_CLEAR);
	return 0;
}

static irqreturn_t ms7210_irq_handler(int irq_num, void *dev)
{
	return IRQ_WAKE_THREAD;
}

static irqreturn_t ms7210_isr_thread(int irq_num, void *dev)
{
	struct loonggpu_bridge_phy *phy = (struct loonggpu_bridge_phy *)dev;

	ms7210_int_clear(phy, interrupt_hpd);
	drm_kms_helper_hotplug_event(phy->connector->dev);
	return IRQ_HANDLED;
}

static struct bridge_phy_hpd_funcs ms7210_hpd_funcs = {
	.int_disable = ms7210_int_disable,
	.int_enable = ms7210_int_enable,
	.int_clear = ms7210_int_clear,
	.get_hpd_status = ms7210_get_hpd_status,
	.irq_handler = ms7210_irq_handler,
	.isr_thread = ms7210_isr_thread,
};

/**
 * @section MS7210  DDC functions
 */
static void ms7210_ddc_ctrl(struct loonggpu_bridge_phy *phy, bool enabled)
{
	regmap_write(phy->phy_regmap, MS7210_REG_DDC_CTRL, CTRL_ON);
	if (enabled)
		regmap_write(phy->phy_regmap, MS7210_REG_DDC_ENABLE,
			     DDC_ENABLE);
	else
		regmap_write(phy->phy_regmap, MS7210_REG_DDC_ENABLE,
			     DDC_DISENABLE);
}

static int ms7210_get_modes(struct loonggpu_bridge_phy *phy,
			    struct drm_connector *connector)
{
	struct edid *edid;
	unsigned int count;
	struct i2c_adapter *i2c_adap;

	i2c_adap = &phy->li2c->adapter;
	ms7210_ddc_ctrl(phy, true);

	edid = drm_get_edid(connector, i2c_adap);
	if (edid) {
		drm_connector_update_edid_property(connector, edid);
		count = drm_add_edid_modes(connector, edid);
		kfree(edid);
	}

	ms7210_ddc_ctrl(phy, false);

	return count;
}

static struct bridge_phy_ddc_funcs ms7210_ddc_funcs = {
	.get_modes = ms7210_get_modes,
};

/**
 *@section ms7210 HDMI AUX functions
 *
 */

static int ms7210_set_gcp_avmute(struct loonggpu_bridge_phy *phy, bool enable,
				 bool blue_screen)
{
	return 0;
}

static int ms7210_set_avi_infoframe(struct loonggpu_bridge_phy *phy,
				    const struct drm_display_mode *mode)
{
	return 0;
}

static struct bridge_phy_hdmi_aux_funcs ms7210_hdmi_aux_funcs = {
	.set_gcp_avmute = ms7210_set_gcp_avmute,
	.set_avi_infoframe = ms7210_set_avi_infoframe,
};

/**
 * @section Device debugfs attribute
 */
static bool ms7210_chip_id_verify(struct loonggpu_bridge_phy *phy, char *str);

static ssize_t ms7210_test_case_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	char name[32];
	struct i2c_client *i2c;
	struct loonggpu_bridge_phy *phy;

	i2c = to_i2c_client(dev);
	phy = (struct loonggpu_bridge_phy *)i2c_get_clientdata(i2c);

	ms7210_chip_id_verify(phy, name);

	return 0;
}

static bool ms7210_chip_id_verify(struct loonggpu_bridge_phy *phy, char *str)
{
	unsigned int version_val[5] = {};

	regmap_read(phy->phy_regmap, 0x0000, &version_val[0]);
	regmap_read(phy->phy_regmap, 0x0100, &version_val[1]);
	regmap_read(phy->phy_regmap, 0x0200, &version_val[2]);
	regmap_read(phy->phy_regmap, 0x0010, &version_val[3]);
	regmap_read(phy->phy_regmap, 0x0110, &version_val[4]);

	if (version_val[0] != MS7210_CHIP_ID0 ||
	    version_val[1] != MS7210_CHIP_ID1 ||
	    version_val[2] != MS7210_CHIP_ID2) {
		DRM_ERROR("Invalid ms7210 chip version {%02x,%02x,%02x}\n",
			  version_val[0], version_val[1], version_val[2]);
		strcpy("Unknown", str);
		return false;
	}

	phy->chip_version = version_val[0];
	strcpy(str, MS7210_CHIP_NAME);
	return true;
}

static ssize_t ms7210_test_case_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct i2c_client *i2c;
	struct loonggpu_bridge_phy *phy;

	i2c = to_i2c_client(dev);
	phy = (struct loonggpu_bridge_phy *)i2c_get_clientdata(i2c);
	if (sysfs_streq(buf, "0"))
		ms7210_int_enable(phy, interrupt_hpd);
	if (sysfs_streq(buf, "1"))
		ms7210_int_disable(phy, interrupt_hpd);
	if (sysfs_streq(buf, "2"))
		ms7210_int_clear(phy, interrupt_hpd);

	return count;
}
static DEVICE_ATTR_RW(ms7210_test_case);

static int ms7210_debugfs_init(struct loonggpu_bridge_phy *phy)
{
	int ret;

	ret = device_create_file(to_dev(phy), &dev_attr_ms7210_test_case);

	return 0;
}

static struct bridge_phy_misc_funcs ms7210_misc_funcs = {
	.chip_id_verify = ms7210_chip_id_verify,
	.debugfs_init = ms7210_debugfs_init,
};

/**
 * @section MS7210 driver initialize
 */
static const struct regmap_access_table ms7210_read_table = {
	.yes_ranges = ms7210_rw_regs_range,
	.n_yes_ranges = ARRAY_SIZE(ms7210_rw_regs_range),
};

static const struct regmap_access_table ms7210_write_table = {
	.yes_ranges = ms7210_rw_regs_range,
	.n_yes_ranges = ARRAY_SIZE(ms7210_rw_regs_range),
	.no_ranges = ms7210_ro_regs_range,
	.n_no_ranges = ARRAY_SIZE(ms7210_ro_regs_range),
};

static const struct regmap_access_table ms7210_volatile_table = {
	.yes_ranges = ms7210_rw_regs_range,
	.n_yes_ranges = ARRAY_SIZE(ms7210_rw_regs_range),
};

static const struct regmap_config ms7210_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.reg_stride = 1,
	.max_register = MS7210_REG_END,

	.fast_io = false,
	.cache_type = REGCACHE_RBTREE,

	.reg_defaults_raw = ms7210_reg_defaults_raw,
	.num_reg_defaults_raw = ARRAY_SIZE(ms7210_reg_defaults_raw),

	.volatile_reg = ms7210_register_volatile,
	.readable_reg = ms7210_register_readable,
	.rd_table = &ms7210_read_table,
	.wr_table = &ms7210_write_table,
};

int bridge_phy_ms7210_init(struct loonggpu_dc_bridge *dc_bridge)
{
	int ret;
	struct loonggpu_bridge_phy *ms7210_phy;
	struct ms7210_device *ms7210_dev;

	ms7210_phy = bridge_phy_alloc(dc_bridge);

	ms7210_phy->cfg_funcs = &ms7210_cfg_funcs;
	ms7210_phy->regmap_cfg = &ms7210_regmap_config;
	ms7210_phy->misc_funcs = &ms7210_misc_funcs;
	ms7210_phy->hpd_funcs = &ms7210_hpd_funcs;
	ms7210_phy->ddc_funcs = &ms7210_ddc_funcs;
	ms7210_phy->hdmi_aux_funcs = &ms7210_hdmi_aux_funcs;
	ms7210_phy->is_ext_encoder = true;

	ms7210_dev = kmalloc(sizeof(struct ms7210_device), GFP_KERNEL);
	if (IS_ERR(ms7210_dev))
		return PTR_ERR(ms7210_dev);

	ms7210_phy->priv = ms7210_dev;

	ret = bridge_phy_init(ms7210_phy);
	return ret;
}

int bridge_phy_ms7210_remove(struct loonggpu_bridge_phy *phy)
{
	return 0;
}

