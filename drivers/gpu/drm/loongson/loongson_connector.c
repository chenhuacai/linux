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

#include <linux/export.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/pm_runtime.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_edid.h>

#include "loongson_drv.h"

/**
 * loongson_connector_best_encoder
 *
 * @connector: point to the drm_connector structure
 *
 * Select the best encoder for the given connector. Used by both the helpers in
 * drm_atomic_helper_check_modeset() and legacy CRTC helpers
 */
static struct drm_encoder *loongson_connector_best_encoder(struct drm_connector
						  *connector)
{
	struct drm_encoder *encoder;

	/* There is only one encoder per connector */
	drm_connector_for_each_possible_encoder(connector, encoder)
		return encoder;

	return NULL;
}

/**
 * loongson_get_modes
 *
 * @connetcor: central DRM connector control structure
 *
 * Fill in all modes currently valid for the sink into the connector->probed_modes list.
 * It should also update the EDID property by calling drm_connector_update_edid_property().
 */
static int loongson_get_modes(struct drm_connector *connector)
{
	int id, ret = 0;
	enum loongson_edid_method ledid_method;
	struct edid *edid = NULL;
	struct loongson_connector *lconnector = to_loongson_connector(connector);
	struct i2c_adapter *adapter = lconnector->i2c->adapter;

	id = drm_connector_index(connector);

	ledid_method = lconnector->edid_method;
	switch (ledid_method) {
	case via_i2c:
	case via_encoder:
	default:
		edid = drm_get_edid(connector, adapter);
		break;
	case via_vbios:
		edid = kmalloc(EDID_LENGTH * 2, GFP_KERNEL);
		memcpy(edid, lconnector->vbios_edid, EDID_LENGTH * 2);
	}

	if (edid) {
		drm_connector_update_edid_property(connector, edid);
		ret = drm_add_edid_modes(connector, edid);
		kfree(edid);
	} else {
		ret += drm_add_modes_noedid(connector, 1920, 1080);
		drm_set_preferred_mode(connector, 1024, 768);
	}

	return ret;
}

static bool is_connected(struct loongson_connector *lconnector)
{
	unsigned char start = 0x0;
	struct i2c_adapter *adapter;
	struct i2c_msg msgs = {
		.addr = DDC_ADDR,
		.len = 1,
		.flags = 0,
		.buf = &start,
	};

	if (!lconnector->i2c)
		return false;

	adapter = lconnector->i2c->adapter;
	if (i2c_transfer(adapter, &msgs, 1) < 1) {
		DRM_DEBUG_KMS("display-%d not connect\n", lconnector->id);
		return false;
	}

	return true;
}

/**
 * loongson_connector_detect
 *
 * @connector: point to drm_connector
 * @force: bool
 *
 * Check to see if anything is attached to the connector.
 * The parameter force is set to false whilst polling,
 * true when checking the connector due to a user request
 */
static enum drm_connector_status loongson_connector_detect(struct drm_connector
						   *connector, bool force)
{
	int r;
	enum drm_connector_status ret = connector_status_connected;
	struct loongson_connector *lconnector = to_loongson_connector(connector);

	DRM_DEBUG("loongson_connector_detect connector_id=%d, ledid_method=%d\n",
			drm_connector_index(connector), lconnector->edid_method);

	if (lconnector->edid_method != via_vbios) {
		r = pm_runtime_get_sync(connector->dev->dev);
		if (r < 0)
			return connector_status_disconnected;

		if (is_connected(lconnector))
			ret = connector_status_connected;
		else
			ret = connector_status_disconnected;

		pm_runtime_mark_last_busy(connector->dev->dev);
		pm_runtime_put_autosuspend(connector->dev->dev);
	}

	return ret;
}

/**
 * These provide the minimum set of functions required to handle a connector
 *
 * Helper operations for connectors.These functions are used
 * by the atomic and legacy modeset helpers and by the probe helpers.
 */
static const struct drm_connector_helper_funcs loongson_connector_helper_funcs = {
        .get_modes = loongson_get_modes,
        .best_encoder = loongson_connector_best_encoder,
};

/**
 * These provide the minimum set of functions required to handle a connector
 *
 * Control connectors on a given device.
 * The functions below allow the core DRM code to control connectors,
 * enumerate available modes and so on.
 */
static const struct drm_connector_funcs loongson_connector_funcs = {
	.dpms = drm_helper_connector_dpms,
	.detect = loongson_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const unsigned short normal_i2c[] = { 0x50, I2C_CLIENT_END };


/**
 * loongson_connector_init
 *
 * @dev: drm device
 * @connector_id:
 *
 * Vga is the interface between host and monitor
 * This function is to init vga
 */
struct drm_connector *loongson_connector_init(struct drm_device *dev, unsigned int index)
{
	struct i2c_adapter *adapter;
	struct i2c_client *ddc_client;
	struct drm_connector *connector;
	struct loongson_encoder *loongson_encoder;
	struct loongson_connector *loongson_connector;
	struct loongson_drm_device *ldev = (struct loongson_drm_device*)dev->dev_private;

	const struct i2c_board_info ddc_info = {
		.type = "ddc-dev",
		.addr = DDC_ADDR,
		.flags = I2C_CLASS_DDC,
	};

	loongson_encoder = ldev->mode_info[index].encoder; 
	adapter = loongson_encoder->i2c->adapter;
	ddc_client = i2c_new_client_device(adapter, &ddc_info);
	if (IS_ERR(ddc_client)) {
		i2c_del_adapter(adapter);
		DRM_ERROR("Failed to create standard ddc client\n");
		return NULL;
	}

	loongson_connector = kzalloc(sizeof(struct loongson_connector), GFP_KERNEL);
	if (!loongson_connector)
		return NULL;

	ldev->connector_active0 = 0;
	ldev->connector_active1 = 0;
	loongson_connector->id = index;
	loongson_connector->ldev = ldev;
	loongson_connector->type = get_connector_type(ldev, index);
	loongson_connector->i2c_id = get_connector_i2cid(ldev, index);
	loongson_connector->hotplug = get_hotplug_mode(ldev, index);
	loongson_connector->edid_method = get_edid_method(ldev, index);
	if (loongson_connector->edid_method == via_vbios)
		loongson_connector->vbios_edid = get_vbios_edid(ldev, index);

	loongson_connector->i2c = &ldev->i2c_bus[index];
	if (!loongson_connector->i2c)
		DRM_INFO("connector-%d match i2c-%d err\n", index,
			 loongson_connector->i2c_id);

	connector = &loongson_connector->base;

	drm_connector_helper_add(connector, &loongson_connector_helper_funcs);

	drm_connector_init(dev, connector,
			   &loongson_connector_funcs, loongson_connector->type);

	drm_connector_register(connector);

	return connector;
}
