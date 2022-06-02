/*
 * Copyright (c) 2018 Loongson Technology Co., Ltd.
 * Authors:
 *	Chen Zhu <zhuchen@loongson.cn>
 *	Yaling Fang <fangyaling@loongson.cn>
 *	Dandan Zhang <zhangdandan@loongson.cn>
 *	Huacai Chen <chenhc@lemote.com>
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include "loongson_drv.h"
#include "loongson_vbios.h"

#define VBIOS_START 0x1000
#define VBIOS_SIZE  0x40000
#define VBIOS_OFFSET 0x100000
#define VBIOS_DESC_OFFSET 0x6000
#define VBIOS_TITLE "Loongson-VBIOS"

/* VBIOS INFO ADDRESS TABLE */
struct acpi_viat_table {
	struct acpi_table_header header;
	unsigned long vbios_addr;
} __packed;

static u32 get_vbios_version(struct loongson_vbios *vbios)
{
	u32 minor, major, version;

	minor = vbios->version_minor;
	major = vbios->version_major;
	version = major * 10 + minor;

	return version;
}

static bool parse_vbios_i2c(struct desc_node *this, struct vbios_cmd *cmd)
{
	bool ret = true;
	int i, num, size;
	struct vbios_i2c *i2c;
	struct vbios_i2c *vbios_i2c = NULL;
	struct loongson_i2c *val = (struct loongson_i2c *)cmd->res;

	size = this->desc->size;
	vbios_i2c = kzalloc(size, GFP_KERNEL);
	if (!vbios_i2c)
		return false;

	memset(vbios_i2c, 0xff, size);
	memcpy(vbios_i2c, this->data, size);
	num = size / sizeof(*vbios_i2c);

	i2c = vbios_i2c;
	for (i = 0; (i < num && i < DC_I2C_BUS_MAX); i++) {
		val->i2c_id = (u32)i2c->id;
		val->use = true;
		val++;
		i2c++;
	}

	kfree(vbios_i2c);
	return ret;
}

static bool parse_vbios_crtc(struct desc_node *this, struct vbios_cmd *cmd)
{
	bool ret = true;
	u64 request = (u64)cmd->req;
	u32 *val = (u32 *)cmd->res;
	struct vbios_crtc crtc;

	memset(&crtc, 0xff, sizeof(crtc));
	memcpy(&crtc, this->data, min_t(u32, this->desc->size, sizeof(crtc)));

	switch (request) {
	case VBIOS_CRTC_ID:
		*val = crtc.crtc_id;
		break;
	case VBIOS_CRTC_ENCODER_ID:
		*val = crtc.encoder_id;
		break;
	case VBIOS_CRTC_MAX_FREQ:
		*val = crtc.max_freq;
		break;
	case VBIOS_CRTC_MAX_WIDTH:
		*val = crtc.max_width;
		break;
	case VBIOS_CRTC_MAX_HEIGHT:
		*val = crtc.max_height;
		break;
	case VBIOS_CRTC_IS_VB_TIMING:
		*val = crtc.is_vb_timing;
		break;
	default:
		ret = false;
		break;
	}

	return ret;
}

static bool parse_vbios_encoder(struct desc_node *this, struct vbios_cmd *cmd)
{
	bool ret = true;
	u64 request = (u64)cmd->req;
	u32 *val = (u32 *)cmd->res;
	struct vbios_encoder encoder;

	memset(&encoder, 0xff, sizeof(encoder));
	memcpy(&encoder, this->data,
	       min_t(u32, this->desc->size, sizeof(encoder)));

	switch (request) {
	case VBIOS_ENCODER_I2C_ID:
		*val = encoder.i2c_id;
		break;
	case VBIOS_ENCODER_CONNECTOR_ID:
		*val = encoder.connector_id;
		break;
	case VBIOS_ENCODER_TYPE:
		*val = encoder.type;
		break;
	case VBIOS_ENCODER_CONFIG_TYPE:
		*val = encoder.config_type;
		break;
	case VBIOS_ENCODER_CHIP:
		*val = encoder.chip;
		break;
	case VBIOS_ENCODER_CHIP_ADDR:
		*val = encoder.chip_addr;
		break;
	default:
		ret = false;
		break;
	}

	return ret;
}

static bool parse_vbios_cfg_encoder(struct desc_node *this,
				    struct vbios_cmd *cmd)
{
	bool ret = true;
	u64 request = (u64)cmd->req;
	u32 *val = (u32 *)cmd->res;
	struct cfg_encoder *cfg_encoder;
	struct cfg_encoder *cfg;
	struct vbios_cfg_encoder *vbios_cfg_encoder;
	u32 num, size, i = 0;

	vbios_cfg_encoder = (struct vbios_cfg_encoder *)this->data;
	size = sizeof(struct vbios_cfg_encoder);
	num = this->desc->size / size;

	switch (request) {
	case VBIOS_ENCODER_CONFIG_PARAM:
		cfg_encoder = (struct cfg_encoder *)kzalloc(
			sizeof(struct cfg_encoder) * num, GFP_KERNEL);
		cfg = cfg_encoder;
		for (i = 0; i < num; i++) {
			cfg->reg_num = vbios_cfg_encoder->reg_num;
			cfg->hdisplay = vbios_cfg_encoder->hdisplay;
			cfg->vdisplay = vbios_cfg_encoder->vdisplay;
			memcpy(&cfg->config_regs,
			       &vbios_cfg_encoder->config_regs,
			       sizeof(struct vbios_conf_reg) * 256);

			cfg++;
			vbios_cfg_encoder++;
		}
		cmd->res = (void *)cfg_encoder;
		break;
	case VBIOS_ENCODER_CONFIG_NUM:
		*val = num;
		break;
	default:
		ret = false;
		break;
	}

	return ret;
}

static bool parse_vbios_connector(struct desc_node *this, struct vbios_cmd *cmd)
{
	bool ret = true;
	u64 request = (u64)cmd->req;
	u32 *val = (u32 *)cmd->res;
	struct vbios_connector connector;

	memset(&connector, 0xff, sizeof(connector));
	memcpy(&connector, this->data,
	       min_t(u32, this->desc->size, sizeof(connector)));

	switch (request) {
	case VBIOS_CONNECTOR_I2C_ID:
		*val = connector.i2c_id;
		break;
	case VBIOS_CONNECTOR_INTERNAL_EDID:
		memcpy((u8 *)(ulong)val, connector.internal_edid,
		       EDID_LENGTH * 2);
		break;
	case VBIOS_CONNECTOR_TYPE:
		*val = connector.type;
		break;
	case VBIOS_CONNECTOR_HOTPLUG:
		*val = connector.hotplug;
		break;
	case VBIOS_CONNECTOR_EDID_METHOD:
		*val = connector.edid_method;
		break;
	case VBIOS_CONNECTOR_IRQ_GPIO:
		*val = connector.irq_gpio;
		break;
	case VBIOS_CONNECTOR_IRQ_PLACEMENT:
		*val = connector.gpio_placement;
		break;
	default:
		ret = false;
		break;
	}

	return ret;
}

static bool parse_vbios_backlight(struct desc_node *this, struct vbios_cmd *cmd)
{
	return 0;
}

static bool parse_vbios_pwm(struct desc_node *this, struct vbios_cmd *cmd)
{
	bool ret = true;
	u64 request = (u64)cmd->req;
	u32 *val = (u32 *)cmd->res;
	struct vbios_pwm *pwm = (struct vbios_pwm *)this->data;

	switch (request) {
	case VBIOS_PWM_ID:
		*val = pwm->pwm;
		break;
	case VBIOS_PWM_PERIOD:
		*val = pwm->peroid;
		break;
	case VBIOS_PWM_POLARITY:
		*val = pwm->polarity;
		break;
	default:
		ret = false;
		break;
	}

	return ret;
}

static bool parse_vbios_header(struct desc_node *this, struct vbios_cmd *cmd)
{
	return true;
}

static bool parse_vbios_default(struct desc_node *this, struct vbios_cmd *cmd)
{
	struct vbios_desc *vb_desc;

	vb_desc = this->desc;
	DRM_WARN("Current descriptor[T-%d][V-%d] cannot be interprete.\n",
		 vb_desc->type, vb_desc->ver);
	return false;
}

#define FUNC(t, v, f)                                                          \
	{                                                                      \
		.type = t, .ver = v, .func = f,                                \
	}

static struct desc_func tables[] = {
	FUNC(desc_header, ver_v1, parse_vbios_header),
	FUNC(desc_i2c, ver_v1, parse_vbios_i2c),
	FUNC(desc_crtc, ver_v1, parse_vbios_crtc),
	FUNC(desc_encoder, ver_v1, parse_vbios_encoder),
	FUNC(desc_connector, ver_v1, parse_vbios_connector),
	FUNC(desc_cfg_encoder, ver_v1, parse_vbios_cfg_encoder),
	FUNC(desc_backlight, ver_v1, parse_vbios_backlight),
	FUNC(desc_pwm, ver_v1, parse_vbios_pwm),
};

static inline parse_func *get_parse_func(struct vbios_desc *vb_desc)
{
	int i;
	u32 type = vb_desc->type;
	u32 ver = vb_desc->ver;
	parse_func *func = parse_vbios_default;
	u32 tt_num = ARRAY_SIZE(tables);

	for (i = 0; i < tt_num; i++) {
		if ((tables[i].ver == ver) && (tables[i].type == type)) {
			func = tables[i].func;
			break;
		}
	}

	return func;
}

static inline u32 insert_desc_list(struct loongson_drm_device *ldev,
				   struct vbios_desc *vb_desc)
{
	struct desc_node *node;
	parse_func *func = NULL;

	WARN_ON(!ldev || !vb_desc);
	node = (struct desc_node *)kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	func = get_parse_func(vb_desc);
	node->parse = func;
	node->desc = (void *)vb_desc;
	node->data = ((u8 *)ldev->vbios + vb_desc->offset);
	list_add_tail(&node->head, &ldev->desc_list);

	return 0;
}

static inline void free_desc_list(struct loongson_drm_device *ldev)
{
	struct desc_node *node, *tmp;

	list_for_each_entry_safe (node, tmp, &ldev->desc_list, head) {
		list_del(&node->head);
		kfree(node);
	}
}

#define DESC_LIST_MAX 1024

static u32 parse_vbios_desc(struct loongson_drm_device *ldev)
{
	u32 i, ret = 0;
	struct vbios_desc *desc;
	enum desc_type type = 0;
	u8 *vbios = (u8 *)ldev->vbios;

	WARN_ON(!vbios);

	desc = (struct vbios_desc *)(vbios + VBIOS_DESC_OFFSET);
	for (i = 0; i < DESC_LIST_MAX; i++) {
		type = desc->type;
		if (type == desc_max)
			break;

		ret = insert_desc_list(ldev, desc);
		if (ret)
			DRM_DEBUG_KMS("Parse T-%d V-%d failed[%d]\n", desc->ver,
				      desc->type, ret);

		desc++;
	}

	return ret;
}

static inline struct desc_node *get_desc_node(struct loongson_drm_device *ldev,
					      u16 type, u8 index)
{
	struct desc_node *node, *tmp;
	struct vbios_desc *vb_desc;

	list_for_each_entry_safe (node, tmp, &ldev->desc_list, head) {
		vb_desc = node->desc;
		if (vb_desc->type == type && vb_desc->index == index)
			return node;
	}

	return NULL;
}

static bool vbios_get_data(struct loongson_drm_device *ldev, struct vbios_cmd *cmd)
{
	struct desc_node *node;

	WARN_ON(!cmd);

	node = get_desc_node(ldev, cmd->type, cmd->index);
	if (node && node->parse)
		return node->parse(node, cmd);

	DRM_DEBUG_DRIVER("Failed to get node(%d,%d)\n", cmd->type, cmd->index);

	return false;
}

u32 get_connector_type(struct loongson_drm_device *ldev, u32 index)
{
	u32 type = -1;
	bool ret = false;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_connector;
	vbt_cmd.req = (void *)(ulong)VBIOS_CONNECTOR_TYPE;
	vbt_cmd.res = (void *)(ulong)&type;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		type = -1;

	return type;
}

u16 get_connector_i2cid(struct loongson_drm_device *ldev, u32 index)
{
	u16 i2c_id = -1;
	bool ret = false;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_connector;
	vbt_cmd.req = (void *)(ulong)VBIOS_CONNECTOR_I2C_ID;
	vbt_cmd.res = (void *)(ulong)&i2c_id;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		i2c_id = -1;

	return i2c_id;
}

u32 get_connector_irq_gpio(struct loongson_drm_device *ldev, u32 index)
{
	int ret;
	u32 irq_gpio;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_connector;
	vbt_cmd.req = (void *)(ulong)VBIOS_CONNECTOR_IRQ_GPIO;
	vbt_cmd.res = (void *)(ulong)&irq_gpio;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		return -1;

	return irq_gpio;
}

enum gpio_placement get_connector_gpio_placement(struct loongson_drm_device *ldev,
						 u32 index)
{
	int ret;
	enum gpio_placement irq_placement;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_connector;
	vbt_cmd.req = (void *)(ulong)VBIOS_CONNECTOR_IRQ_PLACEMENT;
	vbt_cmd.res = (void *)(ulong)&irq_placement;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		return -1;

	return irq_placement;
}

u16 get_hotplug_mode(struct loongson_drm_device *ldev, u32 index)
{
	u16 mode = -1;
	bool ret = false;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_connector;
	vbt_cmd.req = (void *)(ulong)VBIOS_CONNECTOR_HOTPLUG;
	vbt_cmd.res = (void *)(ulong)&mode;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		mode = -1;

	return mode;
}

u16 get_edid_method(struct loongson_drm_device *ldev, u32 index)
{
	bool ret = false;
	u16 method = via_null;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_connector;
	vbt_cmd.req = (void *)(ulong)VBIOS_CONNECTOR_EDID_METHOD;
	vbt_cmd.res = (void *)(ulong)&method;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		method = via_null;

	return method;
}

u8 *get_vbios_edid(struct loongson_drm_device *ldev, u32 index)
{
	u8 *edid = NULL;
	bool ret = false;
	struct vbios_cmd vbt_cmd;

	edid = kzalloc(sizeof(u8) * EDID_LENGTH * 2, GFP_KERNEL);
	if (!edid)
		return edid;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_connector;
	vbt_cmd.req = (void *)(ulong)VBIOS_CONNECTOR_INTERNAL_EDID;
	vbt_cmd.res = (void *)(ulong)edid;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		return NULL;

	return edid;
}

u32 get_vbios_pwm(struct loongson_drm_device *ldev, u32 index, u16 request)
{
	u32 value = -1;
	bool ret = false;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_pwm;
	vbt_cmd.req = (void *)(ulong)request;
	vbt_cmd.res = (void *)(ulong)&value;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		value = 0xffffffff;

	return value;
}

u32 get_crtc_id(struct loongson_drm_device *ldev, u32 index)
{
	u32 crtc_id = 0;
	bool ret = false;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_crtc;
	vbt_cmd.req = (void *)(ulong)VBIOS_CRTC_ID;
	vbt_cmd.res = (void *)(ulong)&crtc_id;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		crtc_id = 0;

	return crtc_id;
}

u32 get_crtc_max_freq(struct loongson_drm_device *ldev, u32 index)
{
	bool ret = false;
	u32 max_freq = 0;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_crtc;
	vbt_cmd.req = (void *)(ulong)VBIOS_CRTC_MAX_FREQ;
	vbt_cmd.res = (void *)(ulong)&max_freq;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		max_freq = 0;

	return max_freq;
}

u32 get_crtc_max_width(struct loongson_drm_device *ldev, u32 index)
{
	bool ret = false;
	u32 max_width = 0;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_crtc;
	vbt_cmd.req = (void *)(ulong)VBIOS_CRTC_MAX_WIDTH;
	vbt_cmd.res = (void *)(ulong)&max_width;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		max_width = LOONGSON_MAX_FB_WIDTH;

	return max_width;
}

u32 get_crtc_max_height(struct loongson_drm_device *ldev, u32 index)
{
	bool ret = false;
	u32 max_height = 0;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_crtc;
	vbt_cmd.req = (void *)(ulong)VBIOS_CRTC_MAX_HEIGHT;
	vbt_cmd.res = (void *)(ulong)&max_height;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		max_height = LOONGSON_MAX_FB_HEIGHT;

	return max_height;
}

u32 get_crtc_encoder_id(struct loongson_drm_device *ldev, u32 index)
{
	bool ret = false;
	u32 encoder_id = 0;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_crtc;
	vbt_cmd.req = (void *)(ulong)VBIOS_CRTC_ENCODER_ID;
	vbt_cmd.res = (void *)(ulong)&encoder_id;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		encoder_id = 0;

	return encoder_id;
}

bool get_crtc_is_vb_timing(struct loongson_drm_device *ldev, u32 index)
{
	bool ret = false;
	bool vb_timing = false;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_crtc;
	vbt_cmd.req = (void *)(ulong)VBIOS_CRTC_IS_VB_TIMING;
	vbt_cmd.res = (void *)(ulong)&vb_timing;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		vb_timing = false;

	return vb_timing;
}

struct crtc_timing *get_crtc_timing(struct loongson_drm_device *ldev, u32 index)
{
	return NULL;
}

u32 get_encoder_connector_id(struct loongson_drm_device *ldev, u32 index)
{
	bool ret = false;
	u32 connector_id = 0;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_encoder;
	vbt_cmd.req = (void *)(ulong)VBIOS_ENCODER_CONNECTOR_ID;
	vbt_cmd.res = (void *)(ulong)&connector_id;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		connector_id = 0;

	return connector_id;
}

u32 get_encoder_i2c_id(struct loongson_drm_device *ldev, u32 index)
{
	u32 i2c_id = 0;
	bool ret = false;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_encoder;
	vbt_cmd.req = (void *)(ulong)VBIOS_ENCODER_I2C_ID;
	vbt_cmd.res = (void *)(ulong)&i2c_id;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		i2c_id = 0;

	return i2c_id;
}

struct cfg_encoder *get_encoder_config(struct loongson_drm_device *ldev, u32 index)
{
	bool ret = false;
	struct vbios_cmd vbt_cmd;
	struct cfg_encoder *encoder_config = NULL;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_cfg_encoder;
	vbt_cmd.req = (void *)(ulong)VBIOS_ENCODER_CONFIG_PARAM;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (ret)
		encoder_config = (struct cfg_encoder *)vbt_cmd.res;

	return encoder_config;
}

u32 get_encoder_cfg_num(struct loongson_drm_device *ldev, u32 index)
{
	struct vbios_cmd vbt_cmd;
	bool ret = false;
	u32 cfg_num = 0;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_cfg_encoder;
	vbt_cmd.req = (void *)(ulong)VBIOS_ENCODER_CONFIG_NUM;
	vbt_cmd.res = (void *)(ulong)&cfg_num;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		cfg_num = 0;

	return cfg_num;
}

enum encoder_config get_encoder_config_type(struct loongson_drm_device *ldev,
					    u32 index)
{
	bool ret = false;
	enum encoder_config config_type = encoder_bios_config;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_encoder;
	vbt_cmd.req = (void *)(ulong)VBIOS_ENCODER_CONFIG_TYPE;
	vbt_cmd.res = (void *)(ulong)&config_type;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		config_type = encoder_bios_config;

	return config_type;
}

enum encoder_object get_encoder_chip(struct loongson_drm_device *ldev, u32 index)
{
	int ret;
	enum encoder_object chip;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_encoder;
	vbt_cmd.req = (void *)(ulong)VBIOS_ENCODER_CHIP;
	vbt_cmd.res = (void *)(ulong)&chip;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		return Unknown;

	return chip;
}

u8 get_encoder_chip_addr(struct loongson_drm_device *ldev, u32 index)
{
	int ret;
	u8 chip_addr;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_encoder;
	vbt_cmd.req = (void *)(ulong)VBIOS_ENCODER_CHIP_ADDR;
	vbt_cmd.res = (void *)(ulong)&chip_addr;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		return Unknown;

	return chip_addr;
}

enum encoder_type get_encoder_type(struct loongson_drm_device *ldev, u32 index)
{
	bool ret = false;
	enum encoder_type type = encoder_dac;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = index;
	vbt_cmd.type = desc_encoder;
	vbt_cmd.req = (void *)(ulong)VBIOS_ENCODER_TYPE;
	vbt_cmd.res = (void *)(ulong)&type;
	ret = vbios_get_data(ldev, &vbt_cmd);
	if (!ret)
		type = encoder_dac;

	return type;
}

bool get_loongson_i2c(struct loongson_drm_device *ldev)
{
	bool ret = false;
	struct vbios_cmd vbt_cmd;

	vbt_cmd.index = 0;
	vbt_cmd.type = desc_i2c;
	vbt_cmd.res = (void *)&ldev->i2c_bus;
	ret = vbios_get_data(ldev, &vbt_cmd);

	if (!ret) {
		ldev->i2c_bus[0].use = true;
		ldev->i2c_bus[1].use = true;
	}

	return true;
}

static void *get_vbios_from_acpi(struct loongson_drm_device *ldev)
{
	void *vbios = NULL;
#ifdef CONFIG_ACPI
	struct acpi_viat_table *viat;
	struct acpi_table_header *hdr;

	if (!ACPI_SUCCESS(acpi_get_table("VIAT", 1, &hdr)))
		return NULL;

	if (hdr->length != sizeof(struct acpi_viat_table)) {
		DRM_WARN("ACPI VIAT table present but broken (length error)\n");
		return NULL;
	}

	vbios = kmalloc(VBIOS_SIZE, GFP_KERNEL);
	if (!vbios)
		return NULL;

	viat = (struct acpi_viat_table *)hdr;
	memcpy(vbios, phys_to_virt(viat->vbios_addr), VBIOS_SIZE);

	DRM_INFO("Get VBIOS from ACPI success!\n");
#endif
	return vbios;
}

static void *get_vbios_from_vram(struct loongson_drm_device *ldev)
{
	void *bios, *vbios = NULL;
	u64 vram_base = pci_resource_start(ldev->gpu_pdev, 2);
	u64 vram_size = pci_resource_len(ldev->gpu_pdev, 2);
	u64 vbios_addr = vram_base + vram_size - VBIOS_OFFSET;

	bios = ioremap(vbios_addr, VBIOS_SIZE);
	if (!bios)
		return NULL;

	vbios = kmalloc(VBIOS_SIZE, GFP_KERNEL);
	if (!vbios)
		return NULL;

	memcpy(vbios, bios, VBIOS_SIZE);

	iounmap(bios);

	if (memcmp(vbios, VBIOS_TITLE, strlen(VBIOS_TITLE))) {
		kfree(vbios);
		return NULL;
	}

	DRM_INFO("Get VBIOS from VRAM success!\n");

	return vbios;
}

bool loongson_vbios_init(struct loongson_drm_device *ldev)
{
	void *vbios = NULL;
	struct loongson_vbios *header;

	vbios = get_vbios_from_vram(ldev);
	if (vbios)
		goto success;

	vbios = get_vbios_from_acpi(ldev);
	if (vbios)
		goto success;

	vbios = kzalloc(256 * 1024, GFP_KERNEL);
	if (!vbios)
		return false;

	header = vbios;
	header->crtc_num = 2;

success:
	header = ldev->vbios = vbios;
	ldev->num_crtc = header->crtc_num;

	DRM_INFO("Loongson VBIOS version %d.%d\n", header->version_major,
		 header->version_minor);

	INIT_LIST_HEAD(&ldev->desc_list);
	parse_vbios_desc(ldev);

	return true;
}

void loongson_vbios_exit(struct loongson_drm_device *ldev)
{
	free_desc_list(ldev);
	kfree(ldev->vbios);
}
